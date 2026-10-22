#include "f3a/navmesh.h"
#include "f3a/fose_runtime.h"
#include "f3a/logger.h"
#include "navmesh_funnel.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

// Read every triangle of every navmesh in ONE cell, appending to `tris`.
void GatherMeshFromCell(UInt8* cell, std::vector<Tri>& tris)
{
    if (!Readable(cell, 0x64)) return;
    UInt8* holder = *reinterpret_cast<UInt8**>(cell + 0x60);   // NavMeshArray
    if (!Readable(holder, 0x0C)) return;
    auto** meshes = *reinterpret_cast<UInt8***>(holder + 0x04);
    UInt32 meshCount = *reinterpret_cast<UInt32*>(holder + 0x08);
    if (!meshes || meshCount == 0 || meshCount > 256) return;
    if (!Readable(meshes, meshCount * sizeof(void*))) return;

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
            if ((int)tris.size() >= kMaxTriangles) return;
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
}

// Read triangles from the player's cell AND, in exteriors, the neighbouring
// loaded cells — so a path can cross cell boundaries. The triangle graph is
// built by shared quantized vertices, so boundary vertices (identical world
// coords on both sides) stitch adjacent cells together automatically.
bool GatherCellTriangles(std::vector<Tri>& tris)
{
    auto* p = fose_rt::Player();
    if (!p) return false;
    UInt8* pcell = reinterpret_cast<UInt8*>(p->parentCell);
    if (!Readable(pcell, 0xC4)) return false;

    GatherMeshFromCell(pcell, tris);

    // worldSpace == null -> interior (single cell; nothing to stitch).
    UInt8* ws = *reinterpret_cast<UInt8**>(pcell + 0xC0);        // worldSpace
    UInt8* pcoords = *reinterpret_cast<UInt8**>(pcell + 0x44);   // coords
    if (Readable(ws, 0x34) && Readable(pcoords, 8)) {
        int px = *reinterpret_cast<int*>(pcoords);
        int py = *reinterpret_cast<int*>(pcoords + 4);
        UInt8* map = *reinterpret_cast<UInt8**>(ws + 0x30);      // cellMap
        if (Readable(map, 0x10)) {
            UInt32 nb = *reinterpret_cast<UInt32*>(map + 0x04);  // m_numBuckets
            UInt8** buckets = *reinterpret_cast<UInt8***>(map + 0x08);
            if (nb > 0 && nb < 1000000 &&
                Readable(buckets, nb * sizeof(void*))) {
                int scanned = 0;
                for (UInt32 b = 0; b < nb && scanned < 64 &&
                                   (int)tris.size() < kMaxTriangles; ++b) {
                    UInt8* entry = buckets[b];
                    for (int guard = 0; entry && guard < 8192; ++guard) {
                        if (!Readable(entry, 0x0C)) break;
                        UInt8* c = *reinterpret_cast<UInt8**>(entry + 8); // data
                        entry = *reinterpret_cast<UInt8**>(entry + 0);    // next
                        if (c == pcell || !Readable(c, 0x64)) continue;
                        UInt8* cc = *reinterpret_cast<UInt8**>(c + 0x44);
                        if (!Readable(cc, 8)) continue;
                        int cx = *reinterpret_cast<int*>(cc);
                        int cy = *reinterpret_cast<int*>(cc + 4);
                        int ddx = cx - px, ddy = cy - py;
                        if (ddx < 0) ddx = -ddx;
                        if (ddy < 0) ddy = -ddy;
                        if (ddx > 2 || ddy > 2) continue;   // within 5x5 grid
                        GatherMeshFromCell(c, tris);
                        ++scanned;
                    }
                }
                F3A_INFO("navmesh: gathered %d cells around (%d,%d)",
                         scanned + 1, px, py);
            }
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
// Which triangle is this point on? `exact` reports whether the point genuinely
// lies inside one, as opposed to us picking the nearest as an approximation.
//
// That distinction matters more than it looks. The nearest-triangle fallback
// used to be UNBOUNDED, so a quest marker kilometres away — in a cell the game
// has not even streamed in — got matched to whatever triangle happened to sit at
// the edge of the loaded area. A* then produced a confident-looking path to that
// edge, and BuildPath appended the real target on the end, so the walk marched
// to the boundary and then set off in a straight line toward coordinates it had
// no geometry for. Near targets worked, far ones went nowhere sensible.
//
// So the fallback is now bounded: beyond `maxFallback` the answer is "not on
// this navmesh", which is the honest one.
int LocateTri(const std::vector<Tri>& tris, const Vec3& p, float maxFallback,
              bool* exact = nullptr)
{
    if (exact) *exact = false;
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
    if (best >= 0) { if (exact) *exact = true; return best; }

    float bestD = 1e30f;
    for (int i = 0; i < (int)tris.size(); ++i) {
        float d = Dist(tris[i].centroid, p);
        if (d < bestD) { bestD = d; best = i; }
    }
    return (bestD <= maxFallback) ? best : -1;
}

// Defined at the end: keeping a route off the walls it turns round.
int InsetCornersOn(const std::vector<Tri>& tris, std::vector<Vec3>& wp, float clearance);

// Points the route hardly bends at, dropped: each one whose leaving out moves
// the route by no more than `tol` (nor its height by more than a step). A
// wall's corner gone round at the margin gives a point for every triangle
// meeting there - an arc of a dozen points a few units apart.
void DropStraightPoints(std::vector<Vec3>& wp, float tol)
{
    if (wp.size() < 3) return;
    std::vector<Vec3> kept;
    kept.reserve(wp.size());
    kept.push_back(wp[0]);
    size_t from = 0;                       // index in wp of kept.back()
    for (size_t i = 1; i + 1 < wp.size(); ++i) {
        const Vec3& a = wp[from];
        const Vec3& b = wp[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
        bool straight = true;
        for (size_t k = from + 1; k <= i && straight; ++k) {
            const Vec3& p = wp[k];
            float t = l > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0.0f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y;
            straight = qx * qx + qy * qy <= tol * tol &&
                       std::fabs(a.z + (b.z - a.z) * t - p.z) <= 24.0f;
        }
        if (!straight) { kept.push_back(wp[i]); from = i; }
    }
    kept.push_back(wp.back());
    wp.swap(kept);
}

} // namespace

// How far off the navmesh an endpoint may sit and still be matched to it.
// The player end is lenient (stairs, furniture, doorways put you just off the
// mesh constantly). The goal end is strict, because a loose match there is how a
// distant, unstreamed target gets mistaken for one right in front of us.
constexpr float kStartFallbackDist = 2000.0f;
constexpr float kGoalFallbackDist  = 800.0f;

constexpr float kAvoidRadius = 120.0f;   // a doorway's worth of triangles

bool GroundZ(float x, float y, float nearZ, float* outZ)
{
    if (!outZ) return false;
    std::vector<Tri> tris;
    if (!GatherCellTriangles(tris)) return false;
    Vec3 p{ x, y, nearZ };
    bool found = false;
    float bestDz = 1e30f, bestZ = 0.0f;
    for (const Tri& t : tris) {
        if (!PointInTri2D(p, t.pos[0], t.pos[1], t.pos[2])) continue;
        // The height of the triangle's plane at (x, y).
        const Vec3& a = t.pos[0]; const Vec3& b = t.pos[1]; const Vec3& c = t.pos[2];
        float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        float z;
        if (std::fabs(det) > 1e-3f) {
            float wa = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / det;
            float wb = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / det;
            z = wa * a.z + wb * b.z + (1.0f - wa - wb) * c.z;
        } else {
            z = t.centroid.z;
        }
        if (!std::isfinite(z)) continue;
        float dz = std::fabs(z - nearZ);
        if (dz < bestDz) { bestDz = dz; bestZ = z; found = true; }
    }
    if (found) *outZ = bestZ;
    return found;
}

// How far the route keeps off the mesh's edge (a wall, a drop) where it
// passes from one triangle to the next. The taut line otherwise runs right
// along the walls, and the player, who is wider than a line, rubs them.
constexpr float kWallMargin = 32.0f;

namespace {
bool PathOnTris(const std::vector<Tri>& tris, const Vec3& from, const Vec3& to,
                std::vector<Vec3>& out_waypoints, const std::vector<Vec3>* avoid,
                float clearance = 40.0f, float margin = kWallMargin);
}

bool BuildPath(const Vec3& from, const Vec3& to,
               std::vector<Vec3>& out_waypoints,
               const std::vector<Vec3>* avoid)
{
    out_waypoints.clear();
    std::vector<Tri> tris;
    if (!GatherCellTriangles(tris)) return false;
    return PathOnTris(tris, from, to, out_waypoints, avoid);
}

namespace {
std::vector<Tri> TrisFromCorners(const std::vector<Vec3>& corners)
{
    std::vector<Tri> tris;
    tris.reserve(corners.size() / 3);
    for (size_t i = 0; i + 2 < corners.size(); i += 3) {
        Tri t;
        for (int k = 0; k < 3; ++k) { t.pos[k] = corners[i + k]; t.vkey[k] = QuantKey(t.pos[k]); }
        t.centroid = { (t.pos[0].x + t.pos[1].x + t.pos[2].x) / 3.0f,
                       (t.pos[0].y + t.pos[1].y + t.pos[2].y) / 3.0f,
                       (t.pos[0].z + t.pos[1].z + t.pos[2].z) / 3.0f };
        tris.push_back(t);
    }
    return tris;
}

float Length2D(const std::vector<Vec3>& wp)
{
    float len = 0.0f;
    for (size_t i = 1; i < wp.size(); ++i) {
        float dx = wp[i].x - wp[i - 1].x, dy = wp[i].y - wp[i - 1].y;
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len;
}

// How far p is from the route (2D), counting only where the route is at
// p's height (on stairs the route passes over the same spot a floor up).
float DistToRoute2D(const Vec3& p, const std::vector<Vec3>& wp)
{
    float best = 1e30f;
    for (size_t i = 1; i < wp.size(); ++i) {
        const Vec3& a = wp[i - 1]; const Vec3& b = wp[i];
        float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
        float t = l > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        if (std::fabs(a.z + (b.z - a.z) * t - p.z) > 128.0f) continue;
        float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y;
        float d = std::sqrt(qx * qx + qy * qy);
        if (d < best) best = d;
    }
    return best;
}

constexpr float kSameWayNear   = 160.0f;   // each engine point this near ours
constexpr float kSameWayLonger = 1.20f;    // ours at most this much longer

bool RetraceOn(const std::vector<Tri>& tris, const std::vector<Vec3>& route,
               std::vector<Vec3>& out, float clearance)
{
    out.clear();
    if (route.size() < 2) return false;
    std::vector<Vec3> mine;
    if (!PathOnTris(tris, route.front(), route.back(), mine, nullptr, clearance, kWallMargin) ||
        mine.size() < 2)
        return false;
    const Vec3& e = mine.back(); const Vec3& g = route.back();
    if (std::fabs(e.x - g.x) > 8.0f || std::fabs(e.y - g.y) > 8.0f || std::fabs(e.z - g.z) > 64.0f)
        return false;                           // ours does not get there
    const float lenMine = Length2D(mine), lenRoute = Length2D(route);
    if (!(lenMine <= lenRoute * kSameWayLonger + 96.0f)) return false;
    for (const Vec3& p : route)
        if (!(DistToRoute2D(p, mine) <= kSameWayNear)) return false;   // another way round
    out = std::move(mine);
    return true;
}
} // namespace

bool BuildPathOnTriangles(const std::vector<Vec3>& corners, const Vec3& from, const Vec3& to,
                          std::vector<Vec3>& out_waypoints, float clearance, float margin)
{
    out_waypoints.clear();
    return PathOnTris(TrisFromCorners(corners), from, to, out_waypoints, nullptr, clearance, margin);
}

bool RetraceOnTriangles(const std::vector<Vec3>& corners, const std::vector<Vec3>& route,
                        std::vector<Vec3>& out, float clearance)
{
    return RetraceOn(TrisFromCorners(corners), route, out, clearance);
}

bool RetraceWithMargin(const std::vector<Vec3>& route, std::vector<Vec3>& out, float clearance)
{
    out.clear();
    if (route.size() < 2) return false;
    std::vector<Tri> tris;
    if (!GatherCellTriangles(tris)) return false;
    return RetraceOn(tris, route, out, clearance);
}

namespace {
bool PathOnTris(const std::vector<Tri>& tris, const Vec3& from, const Vec3& to,
                std::vector<Vec3>& out_waypoints, const std::vector<Vec3>* avoid,
                float clearance, float margin)
{
    out_waypoints.clear();

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

    // The player is always standing on or beside walkable ground, so a generous
    // bound is fine for the start. The goal is the one that must be judged
    // strictly — see LocateTri.
    bool goal_exact = false;
    int start = LocateTri(tris, from, kStartFallbackDist);
    int goal  = LocateTri(tris, to,   kGoalFallbackDist, &goal_exact);
    if (start < 0 || goal < 0) return false;
    if (start == goal) {
        if (goal_exact) out_waypoints.push_back(to);
        else            out_waypoints.push_back(tris[goal].centroid);
        return true;
    }

    // A* over triangle centroids.
    const int N = (int)tris.size();
    std::vector<float> g(N, 1e30f);
    std::vector<int>   came(N, -1);
    std::vector<char>  closed(N, 0);
    using PQ = std::pair<float, int>;            // (fScore, tri)
    std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> open;

    // Places to stay out of: closed before the search starts, so A* treats
    // them as walls. Never the start or the goal triangle themselves.
    if (avoid && !avoid->empty()) {
        int blocked = 0;
        for (int i = 0; i < N; ++i) {
            if (i == start || i == goal) continue;
            for (const auto& a : *avoid) {
                float dx = tris[i].centroid.x - a.x, dy = tris[i].centroid.y - a.y;
                float dz = tris[i].centroid.z - a.z;
                if (dx * dx + dy * dy <= kAvoidRadius * kAvoidRadius &&
                    std::fabs(dz) <= 160.0f) {
                    closed[i] = 1; ++blocked; break;
                }
            }
        }
        F3A_INFO("navmesh: %d triangle(s) blocked around %u avoided point(s)",
                 blocked, (unsigned)avoid->size());
    }

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

    // Only finish AT the target when the target is really on the mesh. Otherwise
    // the path ends at the closest walkable ground we found, and the caller walks
    // that far and re-paths from there — by then the game has streamed in more
    // cells, so the next path reaches further. Appending an off-mesh target here
    // is what produced the "walk to the edge, then head off into nothing" bug.
    const Vec3 end = goal_exact ? to : tris[goal].centroid;
    if (!goal_exact)
        F3A_INFO("navmesh: target is off-mesh; path ends at the nearest reachable "
                 "ground (re-path from there).");

    // The corners on the mesh's edge: those of an edge only one triangle has.
    std::unordered_set<uint64_t> wallCorner;
    for (const Tri& t : tris)
        for (int e = 0; e < 3; ++e) {
            auto it = edgeTris.find(EdgeKey(t.vkey[e], t.vkey[(e + 1) % 3]));
            if (it != edgeTris.end() && it->second.size() == 1) {
                wallCorner.insert(t.vkey[e]);
                wallCorner.insert(t.vkey[(e + 1) % 3]);
            }
        }

    // The way through: the edge each triangle shares with the next, seen
    // walking on (which end is on the left). One waypoint per triangle at each
    // edge's middle walked a zigzag across every open floor; the taut line
    // through the same edges bends only where it must, at an edge's end - an
    // end at a wall drawn in by `margin` (never past the edge's middle), so
    // the line goes round a wall's corner at that distance, not touching it.
    std::vector<FunnelPortal> portals;
    portals.reserve(triPath.size());
    bool portals_ok = true;
    for (size_t i = 0; i + 1 < triPath.size(); ++i) {
        const Tri& A = tris[triPath[i]];
        const Tri& B = tris[triPath[i + 1]];
        Vec3 shared[2];
        uint64_t sharedKey[2] = { 0, 0 };
        int n = 0;
        for (int a = 0; a < 3 && n < 2; ++a)
            for (int b = 0; b < 3; ++b)
                if (A.vkey[a] == B.vkey[b]) { sharedKey[n] = A.vkey[a]; shared[n++] = A.pos[a]; break; }
        if (n != 2) { portals_ok = false; break; }
        if (margin > 0.0f) {
            const Vec3 e0 = shared[0], e1 = shared[1];
            float w = std::sqrt((e1.x - e0.x) * (e1.x - e0.x) + (e1.y - e0.y) * (e1.y - e0.y));
            if (w > 1e-3f) {
                float f = (std::min)(margin, 0.5f * w) / w;   // a share of the edge
                auto toward = [&](const Vec3& p, const Vec3& q) {
                    return Vec3{ p.x + (q.x - p.x) * f, p.y + (q.y - p.y) * f, p.z + (q.z - p.z) * f };
                };
                if (wallCorner.count(sharedKey[0])) shared[0] = toward(e0, e1);
                if (wallCorner.count(sharedKey[1])) shared[1] = toward(e1, e0);
            }
        }
        FunnelPoint c{ A.centroid.x, A.centroid.y, A.centroid.z };
        FunnelPoint mid{ (shared[0].x + shared[1].x) * 0.5f, (shared[0].y + shared[1].y) * 0.5f, 0.0f };
        FunnelPoint s0{ shared[0].x, shared[0].y, shared[0].z };
        FunnelPoint s1{ shared[1].x, shared[1].y, shared[1].z };
        if (funnel_detail::Cross(c, mid, s0) > 0.0f) portals.push_back({ s0, s1 });
        else                                         portals.push_back({ s1, s0 });
    }
    if (portals_ok) {
        // The start first: the first bend can be far off, and the walk measures
        // "on the route" against the leg from the point before.
        out_waypoints.push_back(from);
        for (const auto& p : PullString({ from.x, from.y, from.z }, { end.x, end.y, end.z }, portals))
            out_waypoints.push_back({ p.x, p.y, p.z });
        DropStraightPoints(out_waypoints, 4.0f);
        // The bends sit on the mesh's edge, against what the way turns round.
        if (int moved = InsetCornersOn(tris, out_waypoints, clearance))
            F3A_INFO("navmesh: %d bend(s) moved off the walls", moved);
    } else {
        // An edge that would not match (two meshes meeting badly): the middle
        // of every edge, as before.
        for (size_t i = 0; i + 1 < triPath.size(); ++i) {
            const Tri& A = tris[triPath[i]];
            const Tri& B = tris[triPath[i + 1]];
            Vec3 shared[2];
            int n = 0;
            for (int a = 0; a < 3 && n < 2; ++a)
                for (int b = 0; b < 3; ++b)
                    if (A.vkey[a] == B.vkey[b]) { shared[n++] = A.pos[a]; break; }
            if (n == 2)
                out_waypoints.push_back({ (shared[0].x + shared[1].x) * 0.5f,
                                          (shared[0].y + shared[1].y) * 0.5f,
                                          (shared[0].z + shared[1].z) * 0.5f });
            else
                out_waypoints.push_back(B.centroid);
        }
        out_waypoints.push_back(end);
    }

    F3A_INFO("navmesh: path with %d waypoints over %d triangles.",
             (int)out_waypoints.size(), N);
    return true;
}
} // namespace

namespace {
int InsetCornersOn(const std::vector<Tri>& tris, std::vector<Vec3>& wp, float clearance);
}

int InsetCorners(std::vector<Vec3>& wp, float clearance)
{
    if (wp.size() < 3 || !(clearance > 0.0f)) return 0;
    std::vector<Tri> tris;
    if (!GatherCellTriangles(tris)) return 0;
    return InsetCornersOn(tris, wp, clearance);
}

namespace {
// The height of a triangle's plane at (x, y); its centroid's on a sliver.
float PlaneZ(const Tri& t, float x, float y)
{
    const Vec3& a = t.pos[0]; const Vec3& b = t.pos[1]; const Vec3& c = t.pos[2];
    float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::fabs(det) <= 1e-3f) return t.centroid.z;
    float wa = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / det;
    float wb = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / det;
    return wa * a.z + wb * b.z + (1.0f - wa - wb) * c.z;
}

// 2D distance from p to the segment a-b.
float SegDist2D(const Vec3& p, const Vec3& a, const Vec3& b)
{
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
    float t = l > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y;
    return std::sqrt(qx * qx + qy * qy);
}

int InsetCornersOn(const std::vector<Tri>& tris, std::vector<Vec3>& wp, float clearance)
{
    if (wp.size() < 3 || !(clearance > 0.0f)) return 0;
    // The triangles round one corner and its two legs.
    std::vector<const Tri*> around;
    auto gather = [&](float x0, float y0, float x1, float y1) {
        around.clear();
        for (const Tri& t : tris) {
            float tx0 = (std::min)({ t.pos[0].x, t.pos[1].x, t.pos[2].x });
            float tx1 = (std::max)({ t.pos[0].x, t.pos[1].x, t.pos[2].x });
            float ty0 = (std::min)({ t.pos[0].y, t.pos[1].y, t.pos[2].y });
            float ty1 = (std::max)({ t.pos[0].y, t.pos[1].y, t.pos[2].y });
            if (tx1 < x0 || tx0 > x1 || ty1 < y0 || ty0 > y1) continue;
            around.push_back(&t);
        }
    };
    // On the mesh, at this height: a point beside a walkway is over the
    // ground far below it, and that does not count. A hair outside an edge
    // is rounding (a leg along a wall), not off.
    auto onMesh = [&](const Vec3& p) {
        for (const Tri* t : around) {
            bool in = PointInTri2D(p, t->pos[0], t->pos[1], t->pos[2]);
            for (int e = 0; e < 3 && !in; ++e)
                in = SegDist2D(p, t->pos[e], t->pos[(e + 1) % 3]) < 1.0f;
            if (in && std::fabs(PlaneZ(*t, p.x, p.y) - p.z) < 48.0f) return true;
        }
        return false;
    };
    // The moved corner c and both legs to it on the mesh, every 8 units -
    // checking the point alone let a leg cut across the next wall's corner.
    // The ends are as they are (the player may stand just off the mesh).
    auto legsOn = [&](const Vec3& a, const Vec3& c, const Vec3& b) {
        gather((std::min)({ a.x, b.x, c.x }) - 2.0f, (std::min)({ a.y, b.y, c.y }) - 2.0f,
               (std::max)({ a.x, b.x, c.x }) + 2.0f, (std::max)({ a.y, b.y, c.y }) + 2.0f);
        if (!onMesh(c)) return false;
        for (const Vec3* e : { &a, &b }) {
            const bool endOn = onMesh(*e);
            float dx = e->x - c.x, dy = e->y - c.y, dz = e->z - c.z;
            float len = std::sqrt(dx * dx + dy * dy);
            int steps = (int)(len / 8.0f) + 1;
            for (int k = 1; k < steps; ++k) {
                float t = (float)k / steps;
                if (!endOn && (1.0f - t) * len < 32.0f) break;
                if (!onMesh({ c.x + dx * t, c.y + dy * t, c.z + dz * t })) return false;
            }
        }
        return true;
    };
    const std::vector<Vec3> src = wp;   // every corner judged on the route as given
    int moved = 0;
    for (size_t i = 1; i + 1 < src.size(); ++i) {
        const Vec3& a = src[i - 1];
        const Vec3& p = src[i];
        const Vec3& b = src[i + 1];
        if (std::fabs(p.z - a.z) > 24.0f || std::fabs(b.z - p.z) > 24.0f) continue;
        float ax = p.x - a.x, ay = p.y - a.y, bx = b.x - p.x, by = b.y - p.y;
        float la = std::sqrt(ax * ax + ay * ay), lb = std::sqrt(bx * bx + by * by);
        if (la < 1.0f || lb < 1.0f) continue;
        ax /= la; ay /= la; bx /= lb; by /= lb;
        float dot = ax * bx + ay * by;
        if (dot > 0.966f) continue;                     // under ~15 degrees: no corner
        float ox = ax - bx, oy = ay - by;               // out of the turn
        float ol = std::sqrt(ox * ox + oy * oy);
        if (ol < 1e-3f) continue;
        ox /= ol; oy /= ol;
        float k = clearance * ((1.0f - dot) / 0.5f + 0.3f);
        if (k > clearance) k = clearance;
        float half = 0.5f * (la < lb ? la : lb);
        if (k > half) k = half;
        // The leg in is from the corner before as it now stands (moved or
        // not), the leg out to the next as given: when that one moves, its
        // own check covers the leg between the two.
        for (float f : { 1.0f, 0.5f }) {
            Vec3 c{ p.x + ox * k * f, p.y + oy * k * f, p.z };
            if (legsOn(wp[i - 1], c, b)) { wp[i] = c; ++moved; break; }
        }
    }
    return moved;
}
} // namespace

} // namespace f3a::navmesh
