#include "f3a/navmesh.h"
#include "f3a/fose_runtime.h"
#include "f3a/logger.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdint>

// FO3 1.7 navmesh layout, reverse-engineered from live dumps:
//
//   TESObjectCELL + 0x60  -> pointer to a BSSimpleArray holder
//   holder + 0x04         -> NavMesh** data
//   holder + 0x08         -> uint32 navmesh count
//
//   NavMesh (TESForm, typeID 0x43):
//     + 0x2C -> NavMeshVertex* (12 bytes each: float x,y,z)
//     + 0x30 -> uint32 vertex count
//     + 0x3C -> NavMeshTriangle* (16 bytes each)
//     + 0x40 -> uint32 triangle count
//
//   NavMeshTriangle (16 bytes): uint16 vertices[3], uint16 neighbors[3],
//                               uint16 flags, uint16 flags2.
//
// Struct OFFSETS are identical across the standard and no-gore editions (only
// global/function ADDRESSES differ between them), so these are hardcoded.

namespace f3a::navmesh {
namespace {

using game::Vec3;

constexpr int    kMaxTriangles = 40000;   // safety cap for the whole cell
constexpr float  kQuant        = 0.5f;    // vertex-merge grid (~2 units)

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

struct Tri {
    Vec3     pos[3];
    Vec3     centroid;
    uint64_t vkey[3];   // quantized vertex keys (for shared-edge detection)
};

uint64_t QuantKey(const Vec3& p)
{
    auto q = [](float v) -> uint64_t {
        int i = (int)std::lround(v * kQuant);
        return (uint64_t)(uint32_t)(i & 0x1FFFFF);   // 21 bits, signed-wrapped
    };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

float Dist(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Sign(const Vec3& p, const Vec3& a, const Vec3& b)
{
    return (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
}

bool PointInTri2D(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    float d1 = Sign(p, a, b), d2 = Sign(p, b, c), d3 = Sign(p, c, a);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

// Read every triangle of every navmesh in the player's cell into `tris`.
bool GatherCellTriangles(std::vector<Tri>& tris)
{
    auto* p = fose_rt::Player();
    if (!p) return false;
    auto* cell = reinterpret_cast<UInt8*>(p->parentCell);
    if (!Readable(cell, 0x64)) return false;

    UInt8* holder = *reinterpret_cast<UInt8**>(cell + 0x60);
    if (!Readable(holder, 0x0C)) return false;
    auto** meshes = *reinterpret_cast<UInt8***>(holder + 0x04);
    UInt32 meshCount = *reinterpret_cast<UInt32*>(holder + 0x08);
    if (!meshes || meshCount == 0 || meshCount > 256) return false;
    if (!Readable(meshes, meshCount * sizeof(void*))) return false;

    for (UInt32 m = 0; m < meshCount; ++m) {
        UInt8* nm = meshes[m];
        if (!Readable(nm, 0x44)) continue;
        float* verts  = *reinterpret_cast<float**>(nm + 0x2C);
        UInt32 vcount = *reinterpret_cast<UInt32*>(nm + 0x30);
        UInt8* tdata  = *reinterpret_cast<UInt8**>(nm + 0x3C);
        UInt32 tcount = *reinterpret_cast<UInt32*>(nm + 0x40);
        if (!verts || !tdata || vcount == 0 || tcount == 0) continue;
        if (vcount > 100000 || tcount > 100000) continue;
        if (!Readable(verts, vcount * 12) || !Readable(tdata, tcount * 16))
            continue;

        for (UInt32 t = 0; t < tcount; ++t) {
            if ((int)tris.size() >= kMaxTriangles) return true;
            UInt16* tri = reinterpret_cast<UInt16*>(tdata + t * 16);
            Tri out;
            bool ok = true;
            for (int i = 0; i < 3; ++i) {
                UInt16 vi = tri[i];
                if (vi >= vcount) { ok = false; break; }
                float* v = verts + vi * 3;
                out.pos[i] = { v[0], v[1], v[2] };
                out.vkey[i] = QuantKey(out.pos[i]);
            }
            if (!ok) continue;
            out.centroid = {
                (out.pos[0].x + out.pos[1].x + out.pos[2].x) / 3.0f,
                (out.pos[0].y + out.pos[1].y + out.pos[2].y) / 3.0f,
                (out.pos[0].z + out.pos[1].z + out.pos[2].z) / 3.0f };
            tris.push_back(out);
        }
    }
    return !tris.empty();
}

uint64_t EdgeKey(uint64_t a, uint64_t b)
{
    if (a > b) std::swap(a, b);
    return a * 1000003ULL ^ (b + 0x9E3779B97F4A7C15ULL);
}

// Locate the triangle under a world point: 2D containment, tie-broken by the
// triangle whose average height is closest to the point's Z. Falls back to the
// nearest centroid if the point isn't strictly inside any triangle.
int LocateTri(const std::vector<Tri>& tris, const Vec3& p)
{
    int best = -1;
    float bestZ = 1e30f;
    for (int i = 0; i < (int)tris.size(); ++i) {
        const Tri& t = tris[i];
        if (PointInTri2D(p, t.pos[0], t.pos[1], t.pos[2])) {
            float az = (t.pos[0].z + t.pos[1].z + t.pos[2].z) / 3.0f;
            float dz = std::fabs(az - p.z);
            if (dz < bestZ) { bestZ = dz; best = i; }
        }
    }
    if (best >= 0) return best;

    float bestD = 1e30f;
    for (int i = 0; i < (int)tris.size(); ++i) {
        float d = Dist(tris[i].centroid, p);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

} // namespace

bool BuildPath(const Vec3& from, const Vec3& to,
               std::vector<Vec3>& out_waypoints)
{
    out_waypoints.clear();

    std::vector<Tri> tris;
    if (!GatherCellTriangles(tris)) return false;

    // Edge -> triangles sharing it (by quantized vertex positions, so the
    // graph spans all navmeshes in the cell wherever their edges coincide).
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(tris.size() * 3);
    for (int i = 0; i < (int)tris.size(); ++i) {
        for (int e = 0; e < 3; ++e) {
            uint64_t k = EdgeKey(tris[i].vkey[e], tris[i].vkey[(e + 1) % 3]);
            edgeTris[k].push_back(i);
        }
    }

    std::vector<std::vector<int>> adj(tris.size());
    for (auto& kv : edgeTris) {
        auto& list = kv.second;
        for (size_t a = 0; a < list.size(); ++a)
            for (size_t b = a + 1; b < list.size(); ++b) {
                adj[list[a]].push_back(list[b]);
                adj[list[b]].push_back(list[a]);
            }
    }

    int start = LocateTri(tris, from);
    int goal  = LocateTri(tris, to);
    if (start < 0 || goal < 0) return false;
    if (start == goal) { out_waypoints.push_back(to); return true; }

    // A* over triangle centroids.
    const int N = (int)tris.size();
    std::vector<float> g(N, 1e30f);
    std::vector<int>   came(N, -1);
    std::vector<char>  closed(N, 0);
    using PQ = std::pair<float, int>;            // (fScore, tri)
    std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> open;

    g[start] = 0.0f;
    open.push({ Dist(tris[start].centroid, tris[goal].centroid), start });

    bool found = false;
    while (!open.empty()) {
        int cur = open.top().second;
        open.pop();
        if (cur == goal) { found = true; break; }
        if (closed[cur]) continue;
        closed[cur] = 1;
        for (int nb : adj[cur]) {
            if (closed[nb]) continue;
            float tentative = g[cur] + Dist(tris[cur].centroid, tris[nb].centroid);
            if (tentative < g[nb]) {
                g[nb] = tentative;
                came[nb] = cur;
                float f = tentative + Dist(tris[nb].centroid, tris[goal].centroid);
                open.push({ f, nb });
            }
        }
    }
    if (!found) return false;

    // Reconstruct triangle path start..goal.
    std::vector<int> triPath;
    for (int c = goal; c != -1; c = came[c]) triPath.push_back(c);
    std::reverse(triPath.begin(), triPath.end());

    // Waypoint per triangle hop = midpoint of the shared edge (passage centre).
    for (size_t i = 0; i + 1 < triPath.size(); ++i) {
        const Tri& A = tris[triPath[i]];
        const Tri& B = tris[triPath[i + 1]];
        // Find the two shared quantized vertices.
        Vec3 shared[2];
        int n = 0;
        for (int a = 0; a < 3 && n < 2; ++a)
            for (int b = 0; b < 3; ++b)
                if (A.vkey[a] == B.vkey[b]) { shared[n++] = A.pos[a]; break; }
        if (n == 2) {
            out_waypoints.push_back({ (shared[0].x + shared[1].x) * 0.5f,
                                      (shared[0].y + shared[1].y) * 0.5f,
                                      (shared[0].z + shared[1].z) * 0.5f });
        } else {
            out_waypoints.push_back(B.centroid);   // fallback
        }
    }
    out_waypoints.push_back(to);

    F3A_INFO("navmesh: path with %d waypoints over %d triangles.",
             (int)out_waypoints.size(), N);
    return true;
}

} // namespace f3a::navmesh
