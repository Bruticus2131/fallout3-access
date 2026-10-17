// Walk and road graphs for long exterior walks. See include/f3a/roadnet.h.
//
// Loader, nearest-node grid and shortest path ported from FalloutNVAccess's
// RoadNetwork.cpp, and the road-route rules from its AutoWalk.cpp
// (MIT licence, (c) 2026 Alexander Epaneshnikov). The walk graph (version 3)
// and its route are ours.

#include "f3a/roadnet.h"
#include "f3a/game_access.h"
#include "f3a/logger.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <map>
#include <queue>
#include <unordered_map>

namespace f3a::roadnet {
namespace {

// Road graphs (versions 1-2) name their worldspace by editor id only. The
// form ids are Fallout3.esm's (load-order index 00).
struct KnownWorld { const char* edid; uint32_t id; };
const KnownWorld kWorlds[] = {
    { "Wasteland", 0x0000003C },
};

constexpr float kSearchRadius    = 8192.0f;   // a road point this near, or none
constexpr float kMinDistance     = 2000.0f;   // shorter walks go direct
constexpr float kMaxDetour       = 1.5f;      // whole road walk / straight line
constexpr float kMinDirectionDot = 0.5f;      // first road point within 60 degrees
constexpr float kMinSegment      = 2048.0f;   // kept road points this far apart
// Walk graphs (version 3).
constexpr float kWalkReach    = 1024.0f;  // graph points this near the ends are candidates
constexpr float kWalkOffroad  = 1.35f;    // the generator's open-ground factor
constexpr float kWalkLeg      = 1500.0f;  // a leg's point every this much of the route
constexpr float kWalkMaxRatio = 10.0f;    // longer than this x the straight line: distrust it (Arefu's deck: 7.4x)
// Getting on and off the graph: a unit of height costs this many of distance,
// so the graph point on the target's own level wins over one under it.
constexpr float kWalkClimb    = 4.0f;
// A leg's point also where the way climbs or drops steeply (a ramp, stairs):
// spaced 1500 apart the points skipped them, and the engine was left to find
// the ramp it had already failed to find.
constexpr float kWalkStepZ    = 150.0f;
constexpr uint8_t kFlagRoad   = 1;

struct Node { float x, y, z; };
struct Adj  { uint32_t to; float w; };

struct Net {
    std::vector<Node> nodes;
    std::vector<std::vector<Adj>> adj;
    uint32_t gridX = 0, gridY = 0;
    float cell = 512.0f, minX = 0.0f, minY = 0.0f;
    std::vector<uint32_t> cellOff, cellCnt, list;
    bool walk = false;                  // a walk graph (version 3)
    std::vector<uint8_t> flags;         // walk graphs: per node, kFlagRoad...
    std::vector<uint32_t> area;         // walk graphs: connected part each node is in
};

// Number the connected parts of a walk graph (an island - a roof, a ledge -
// reachable only through a building is a part of its own).
void NumberAreas(Net& n)
{
    const uint32_t N = (uint32_t)n.nodes.size();
    n.area.assign(N, UINT_MAX);
    uint32_t next = 0;
    std::vector<uint32_t> stack;
    for (uint32_t s = 0; s < N; ++s) {
        if (n.area[s] != UINT_MAX) continue;
        n.area[s] = next;
        stack.push_back(s);
        while (!stack.empty()) {
            uint32_t u = stack.back();
            stack.pop_back();
            for (const auto& a : n.adj[u])
                if (n.area[a.to] == UINT_MAX) { n.area[a.to] = next; stack.push_back(a.to); }
        }
        ++next;
    }
}

std::mutex g_mutex;
std::unordered_map<uint32_t, Net> g_nets;     // worldspace form id -> graph

template <class T> bool Get(FILE* f, T* v) { return std::fread(v, sizeof(T), 1, f) == 1; }

// A graph file found in the data folder, header read, body not yet: which
// graph of a worldspace to load depends on the DLCs the game has loaded.
struct FileInfo {
    std::wstring path;
    uint32_t version = 0, nNodes = 0, nEdges = 0;
    float cell = 0, minX = 0, minY = 0;
    std::string edid;
    std::string module;          // the plugin the worldspace comes from
    uint32_t localId = 0;        // its form id without the load-order byte
    std::vector<std::pair<std::string, uint32_t>> overlays;   // DLC changes built in, navmeshes
    long body = 0;               // where the nodes start
};
std::vector<FileInfo> g_files;
bool g_resolved = false;

std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool ReadName(FILE* f, std::string* out)
{
    uint8_t len = 0;
    if (!Get(f, &len) || len == 0) return false;
    out->resize(len);
    return std::fread(&(*out)[0], 1, len, f) == len;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

bool ReadHeader(const std::wstring& path, FileInfo* fi)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[4];
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "FNVR", 4) != 0) {
        std::fclose(f);                 // another kind of data file (doors.dat)
        return false;
    }
    uint32_t reserved = 0;
    float maxX = 0, maxY = 0;
    bool ok = Get(f, &fi->version) && fi->version >= 1 && fi->version <= 4 &&
              Get(f, &fi->nNodes) && Get(f, &fi->nEdges) && Get(f, &fi->cell) &&
              Get(f, &fi->minX) && Get(f, &fi->minY) && Get(f, &maxX) && Get(f, &maxY) &&
              Get(f, &reserved);
    char edid[33] = {};
    if (ok && fi->version >= 2) ok = std::fread(edid, 1, 32, f) == 32;
    fi->edid = edid;
    // Version 4 (DLC): the plugin, and the DLCs whose navmesh changes are in.
    if (ok && fi->version >= 4) {
        uint8_t n = 0;
        ok = ReadName(f, &fi->module) && Get(f, &n);
        for (uint8_t i = 0; ok && i < n; ++i) {
            std::string o;
            uint32_t count = 0;
            ok = ReadName(f, &o) && Get(f, &count);
            if (ok) fi->overlays.push_back({ o, count });
        }
    } else {
        fi->module = "Fallout3.esm";
    }
    // "Must be within" limits: a damaged file must not allocate gigabytes.
    if (ok) ok = fi->nNodes > 0 && fi->nNodes <= 2000000 && fi->nEdges <= 8000000 &&
                 std::isfinite(fi->cell) && fi->cell >= 16.0f && fi->cell <= 65536.0f;
    if (ok) {
        // A walk graph names its worldspace by form id (the reserved field).
        uint32_t ws = fi->version >= 3 ? reserved : 0;
        for (const auto& k : kWorlds) if (!ws && std::strcmp(edid, k.edid) == 0) ws = k.id;
        if (!ws) {
            F3A_INFO("RoadNet: '%s' is for worldspace '%s', which we do not know - skipped",
                     Narrow(path).c_str(), edid);
            std::fclose(f);
            return false;
        }
        fi->localId = ws & 0x00FFFFFFu;
        fi->body = std::ftell(f);
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("RoadNet: '%s' is damaged or not a road graph - skipped", Narrow(path).c_str());
        return false;
    }
    fi->path = path;
    return true;
}

bool LoadBody(const FileInfo& fi, Net* out)
{
    FILE* f = _wfopen(fi.path.c_str(), L"rb");
    if (!f) return false;
    const uint32_t nNodes = fi.nNodes, nEdges = fi.nEdges;
    bool ok = std::fseek(f, fi.body, SEEK_SET) == 0;
    Net net;
    if (ok) {
        net.nodes.resize(nNodes);
        ok = std::fread(net.nodes.data(), sizeof(Node), nNodes, f) == nNodes;
    }
    if (ok) {
        net.adj.resize(nNodes);
        for (uint32_t i = 0; ok && i < nEdges; ++i) {
            uint32_t a = 0, b = 0; float w = 0;
            ok = Get(f, &a) && Get(f, &b) && Get(f, &w);
            if (ok && a < nNodes && b < nNodes && std::isfinite(w)) {
                net.adj[a].push_back({ b, w });
                net.adj[b].push_back({ a, w });
            }
        }
    }
    if (ok) ok = Get(f, &net.gridX) && Get(f, &net.gridY) &&
                 (uint64_t)net.gridX * net.gridY <= 16000000ull;
    if (ok) {
        uint32_t cells = net.gridX * net.gridY;
        net.cell = fi.cell; net.minX = fi.minX; net.minY = fi.minY;
        net.cellOff.resize(cells);
        net.cellCnt.resize(cells);
        ok = std::fread(net.cellOff.data(), 4, cells, f) == cells &&
             std::fread(net.cellCnt.data(), 4, cells, f) == cells;
        uint64_t total = 0;
        for (uint32_t i = 0; ok && i < cells; ++i) total += net.cellCnt[i];
        if (ok) ok = total <= 4ull * nNodes + 16;
        if (ok) {
            net.list.resize((size_t)total);
            ok = std::fread(net.list.data(), 4, (size_t)total, f) == total;
        }
        // Every index must point inside what was read.
        for (uint32_t i = 0; ok && i < cells; ++i)
            ok = (uint64_t)net.cellOff[i] + net.cellCnt[i] <= net.list.size();
        for (size_t i = 0; ok && i < net.list.size(); ++i) ok = net.list[i] < nNodes;
    }
    if (ok && fi.version >= 3) {
        net.walk = true;
        net.flags.resize(nNodes);
        ok = std::fread(net.flags.data(), 1, nNodes, f) == nNodes;
        if (ok) NumberAreas(net);
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("RoadNet: '%s' is damaged or not a road graph - skipped", Narrow(fi.path).c_str());
        return false;
    }
    *out = std::move(net);
    return true;
}

// Load the graphs of the worldspaces the game has, once it has read its data
// files: a DLC's worldspace gets the load-order byte the DLC has now, and of a
// worldspace's graphs the one built with the DLC changes that match the loaded
// DLCs best (counted in navmeshes) is taken. Under g_mutex.
void ResolveLocked()
{
    if (g_resolved) return;
    g_resolved = true;
    std::vector<std::string> plugins = game::LoadedPlugins();
    if (plugins.empty()) {
        F3A_INFO("RoadNet: the game's plugin list cannot be read - using Fallout3.esm's "
                 "graphs only");
        plugins.push_back("Fallout3.esm");
    }
    std::unordered_map<std::string, uint32_t> index;
    for (uint32_t i = 0; i < plugins.size(); ++i) index.emplace(Lower(plugins[i]), i);
    std::map<std::pair<std::string, uint32_t>, std::vector<const FileInfo*>> worlds;
    for (const auto& fi : g_files) worlds[{ Lower(fi.module), fi.localId }].push_back(&fi);
    for (const auto& [key, files] : worlds) {
        auto it = index.find(key.first);
        if (it == index.end()) {
            F3A_INFO("RoadNet: '%s' (%s) - that plugin is not loaded", files[0]->edid.c_str(),
                     files[0]->module.c_str());
            continue;
        }
        // Cost of a graph: navmeshes of DLCs built in but not loaded, plus of
        // DLCs loaded (and changing this worldspace) but not built in.
        std::unordered_map<std::string, uint32_t> changes;
        for (const FileInfo* fi : files)
            for (const auto& o : fi->overlays)
                changes[Lower(o.first)] = (std::max)(changes[Lower(o.first)], o.second);
        const FileInfo* best = nullptr;
        uint64_t bestCost = UINT64_MAX;
        for (const FileInfo* fi : files) {
            uint64_t cost = 0;
            for (const auto& [name, count] : changes) {
                bool in = false;
                for (const auto& o : fi->overlays) in = in || Lower(o.first) == name;
                if (in != (index.count(name) != 0)) cost += count;
            }
            if (cost < bestCost || (cost == bestCost && fi->overlays.size() < best->overlays.size())) {
                best = fi;
                bestCost = cost;
            }
        }
        Net net;
        if (!best || !LoadBody(*best, &net)) continue;
        uint32_t ws = (it->second << 24) | key.second;
        F3A_INFO("RoadNet: loaded %s graph '%s' (worldspace %08X, %s%s): %u nodes, %u edges",
                 best->version >= 3 ? "walk" : "road", best->edid.c_str(), ws,
                 best->module.c_str(), best->overlays.empty() ? "" : " with the DLC changes",
                 best->nNodes, best->nEdges);
        g_nets[ws] = std::move(net);
    }
}

uint32_t Nearest(const Net& n, float x, float y, float maxDist)
{
    int cx = (int)std::floor((x - n.minX) / n.cell);
    int cy = (int)std::floor((y - n.minY) / n.cell);
    float best2 = maxDist * maxDist;
    uint32_t best = UINT_MAX;
    int maxRing = (int)(maxDist / n.cell) + 1;
    for (int ring = 0; ring <= maxRing; ++ring) {
        bool found = false;
        for (int dx = -ring; dx <= ring; ++dx)
            for (int dy = -ring; dy <= ring; ++dy) {
                if (ring > 0 && std::abs(dx) != ring && std::abs(dy) != ring) continue;
                int gx = cx + dx, gy = cy + dy;
                if (gx < 0 || gy < 0 || gx >= (int)n.gridX || gy >= (int)n.gridY) continue;
                uint32_t c = (uint32_t)gy * n.gridX + (uint32_t)gx;
                for (uint32_t i = 0; i < n.cellCnt[c]; ++i) {
                    uint32_t k = n.list[n.cellOff[c] + i];
                    float ex = n.nodes[k].x - x, ey = n.nodes[k].y - y;
                    float d2 = ex * ex + ey * ey;
                    if (d2 < best2) { best2 = d2; best = k; found = true; }
                }
            }
        if (found) {
            float ringDist = (ring + 1) * n.cell;
            if (ringDist * ringDist > best2) break;
        }
    }
    return best;
}

bool ShortestPath(const Net& n, uint32_t s, uint32_t e, std::vector<uint32_t>* out)
{
    const size_t N = n.nodes.size();
    std::vector<float> dist(N, (std::numeric_limits<float>::max)());
    std::vector<uint32_t> prev(N, UINT_MAX);
    using Q = std::pair<float, uint32_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
    dist[s] = 0.0f;
    pq.push({ 0.0f, s });
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (u == e) break;
        if (d > dist[u]) continue;
        for (const auto& a : n.adj[u]) {
            float nd = d + a.w;
            if (nd < dist[a.to]) { dist[a.to] = nd; prev[a.to] = u; pq.push({ nd, a.to }); }
        }
    }
    if (s != e && prev[e] == UINT_MAX) return false;
    out->clear();
    for (uint32_t c = e; c != UINT_MAX; c = prev[c]) {
        out->push_back(c);
        if (out->size() > N) return false;          // a broken prev chain
    }
    std::reverse(out->begin(), out->end());
    return true;
}

float Dist2D(float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    return std::sqrt(dx * dx + dy * dy);
}

// Every node within `r` of (x, y), from the grid.
void NodesWithin(const Net& n, float x, float y, float r, std::vector<uint32_t>* out)
{
    out->clear();
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    int gx0 = (int)std::floor((x - r - n.minX) / n.cell);
    int gx1 = (int)std::floor((x + r - n.minX) / n.cell);
    int gy0 = (int)std::floor((y - r - n.minY) / n.cell);
    int gy1 = (int)std::floor((y + r - n.minY) / n.cell);
    if (gx0 < 0) gx0 = 0;
    if (gy0 < 0) gy0 = 0;
    if (gx1 >= (int)n.gridX) gx1 = (int)n.gridX - 1;
    if (gy1 >= (int)n.gridY) gy1 = (int)n.gridY - 1;
    for (int gy = gy0; gy <= gy1; ++gy)
        for (int gx = gx0; gx <= gx1; ++gx) {
            uint32_t c = (uint32_t)gy * n.gridX + (uint32_t)gx;
            for (uint32_t i = 0; i < n.cellCnt[c]; ++i) {
                uint32_t k = n.list[n.cellOff[c] + i];
                if (Dist2D(n.nodes[k].x, n.nodes[k].y, x, y) <= r) out->push_back(k);
            }
        }
}

// A route over a walk graph: A* from every graph point near the player to any
// near the target. Getting on and off the graph costs like open ground, and a
// difference in height a lot extra — the point on the right level must win —
// but a cost, not a cut-off: a door placed below the ground it stands on (a
// Megaton door is 540 units under its walkway) must still be reachable. The heuristic is the
// straight distance, which no edge undercuts (an edge costs at least its
// length).
bool WalkRoute(const Net& n, const game::Vec3& from, const game::Vec3& to,
               std::vector<game::Vec3>* out, float* walk, float* roadShare)
{
    std::vector<uint32_t> starts, ends;
    NodesWithin(n, from.x, from.y, kWalkReach, &starts);
    NodesWithin(n, to.x, to.y, kWalkReach, &ends);
    // On the right level only, when there is one: under Arefu's deck the
    // ground is within reach on the map, and ending a route there (2700 units
    // below the target) looked cheaper than the long way up the ramp. A level
    // is not insisted on when none is near (Megaton's door is 540 units under
    // its walkway): then the nearest heights are kept.
    auto keepLevel = [&n](std::vector<uint32_t>& v, float z) {
        float minDz = (std::numeric_limits<float>::max)();
        for (uint32_t k : v) minDz = (std::min)(minDz, std::fabs(n.nodes[k].z - z));
        float limit = (std::max)(300.0f, minDz + 150.0f);
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](uint32_t k) { return !(std::fabs(n.nodes[k].z - z) <= limit); }),
                v.end());
    };
    keepLevel(starts, from.z);
    keepLevel(ends, to.z);
    if (starts.empty() || ends.empty()) {
        F3A_INFO("RoadNet: the walk graph has no point near the %s",
                 starts.empty() ? "player" : "target");
        return false;
    }
    const size_t N = n.nodes.size();
    std::vector<float> g(N, (std::numeric_limits<float>::max)());
    std::vector<uint32_t> prev(N, UINT_MAX);
    std::unordered_map<uint32_t, float> egress;      // end candidate -> cost on to the target
    for (uint32_t k : ends) {
        const Node& p = n.nodes[k];
        egress[k] = Dist2D(p.x, p.y, to.x, to.y) * kWalkOffroad +
                    std::fabs(p.z - to.z) * kWalkClimb;
    }
    using Q = std::pair<float, uint32_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
    for (uint32_t k : starts) {
        const Node& p = n.nodes[k];
        float c = Dist2D(p.x, p.y, from.x, from.y) * kWalkOffroad +
                  std::fabs(p.z - from.z) * kWalkClimb;
        if (c < g[k]) {
            g[k] = c;
            pq.push({ c + Dist2D(p.x, p.y, to.x, to.y), k });
        }
    }
    float best = (std::numeric_limits<float>::max)();
    uint32_t bestNode = UINT_MAX;
    size_t popped = 0;
    while (!pq.empty()) {
        auto [f, u] = pq.top();
        pq.pop();
        if (f >= best) break;
        const Node& pu = n.nodes[u];
        float gu = g[u];
        if (f > gu + Dist2D(pu.x, pu.y, to.x, to.y) + 0.5f) continue;   // a stale entry
        if (++popped > N * 2) break;                                    // a guard; cannot happen
        auto it = egress.find(u);
        if (it != egress.end() && gu + it->second < best) {
            best = gu + it->second;
            bestNode = u;
        }
        for (const auto& a : n.adj[u]) {
            float nc = gu + a.w;
            if (nc < g[a.to]) {
                g[a.to] = nc;
                prev[a.to] = u;
                const Node& pv = n.nodes[a.to];
                pq.push({ nc + Dist2D(pv.x, pv.y, to.x, to.y), a.to });
            }
        }
    }
    if (bestNode == UINT_MAX) {
        F3A_INFO("RoadNet: the walk graph has no way from here to the target");
        return false;
    }
    std::vector<uint32_t> path;
    for (uint32_t c = bestNode; c != UINT_MAX; c = prev[c]) {
        path.push_back(c);
        if (path.size() > N) return false;          // a broken prev chain
    }
    std::reverse(path.begin(), path.end());
    float length = 0.0f, road = 0.0f, acc = 0.0f;
    float lastZ = n.nodes[path.front()].z;
    out->clear();
    for (size_t i = 1; i < path.size(); ++i) {
        const Node& p = n.nodes[path[i - 1]];
        const Node& q = n.nodes[path[i]];
        float d = Dist2D(p.x, p.y, q.x, q.y);
        length += d;
        if ((n.flags[path[i - 1]] & kFlagRoad) && (n.flags[path[i]] & kFlagRoad)) road += d;
        acc += d;
        float climb = std::fabs(q.z - lastZ);
        bool steep = climb > kWalkStepZ && climb > 0.3f * acc;
        if ((acc >= kWalkLeg || steep) && i + 1 < path.size()) {
            out->push_back({ q.x, q.y, q.z });
            acc = 0.0f;
            lastZ = q.z;
        }
    }
    // The graph's last point too: the way may end up a ramp (Arefu's deck)
    // within a leg's length of the target, and without it the walk had no leg
    // at all - and "arrived" on the ground under the target.
    {
        const Node& last = n.nodes[path.back()];
        if (path.size() >= 2 && (out->empty() || out->back().x != last.x || out->back().y != last.y))
            out->push_back({ last.x, last.y, last.z });
    }
    const Node& first = n.nodes[path.front()];
    const Node& last  = n.nodes[path.back()];
    length += Dist2D(from.x, from.y, first.x, first.y) + Dist2D(last.x, last.y, to.x, to.y);
    float direct = Dist2D(from.x, from.y, to.x, to.y);
    float share = length > 1.0f ? road / length : 0.0f;
    // "Must be within": a NaN or an absurd detour fails here, closed.
    if (!(length <= kWalkMaxRatio * direct + 4000.0f)) {
        F3A_INFO("RoadNet: a walk route of %.0f vs direct %.0f is not believable - not used",
                 length, direct);
        out->clear();
        return false;
    }
    F3A_INFO("RoadNet: walk route - %u leg point(s), %.0f units vs direct %.0f (x%.2f), "
             "%.0f%% on roads, %u graph points searched", (unsigned)out->size(), length,
             direct, direct > 1.0f ? length / direct : 0.0f, share * 100.0f, (unsigned)popped);
    if (walk) *walk = length;
    if (roadShare) *roadShare = share;
    return true;
}

} // namespace

int LoadDir(const std::wstring& dir)
{
    int found = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dat").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        F3A_INFO("RoadNet: no road graphs in the data folder - long walks go direct");
        return 0;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        FileInfo fi;
        if (!ReadHeader(dir + L"\\" + fd.cFileName, &fi)) continue;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_files.push_back(std::move(fi));
        g_resolved = false;
        ++found;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    F3A_INFO("RoadNet: %d graph file(s); loaded when the game has read its plugins", found);
    return found;
}

void Resolve()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
}

bool Has(uint32_t worldspace)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    return g_nets.count(worldspace) != 0;
}

int Area(uint32_t worldspace, const game::Vec3& p)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    auto it = g_nets.find(worldspace);
    if (it == g_nets.end() || !it->second.walk) return -1;
    const Net& n = it->second;
    std::vector<uint32_t> cand;
    NodesWithin(n, p.x, p.y, 400.0f, &cand);
    // Only a confident answer: a graph point close by, at about this height.
    // A guess here would send the walk into buildings for nothing.
    float best = (std::numeric_limits<float>::max)();
    int area = -1;
    for (uint32_t k : cand) {
        const Node& q = n.nodes[k];
        float dz = std::fabs(q.z - p.z);
        if (!(dz <= 200.0f)) continue;
        float c = Dist2D(q.x, q.y, p.x, p.y) + 4.0f * dz;
        if (c < best) { best = c; area = (int)n.area[k]; }
    }
    return area;
}

bool Route(uint32_t worldspace, const game::Vec3& from, const game::Vec3& to,
           std::vector<game::Vec3>* out, float* walk, float* roadShare, float minDistance)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    auto it = g_nets.find(worldspace);
    if (it == g_nets.end() || !out) return false;
    const Net& n = it->second;
    float direct = Dist2D(from.x, from.y, to.x, to.y);
    if (!(direct >= minDistance)) return false;
    if (n.walk) return WalkRoute(n, from, to, out, walk, roadShare);
    if (roadShare) *roadShare = 1.0f;
    uint32_t a = Nearest(n, from.x, from.y, kSearchRadius);
    uint32_t b = Nearest(n, to.x, to.y, kSearchRadius);
    if (a == UINT_MAX || b == UINT_MAX || a == b) {
        F3A_INFO("RoadNet: no road route (%s)", a == b && a != UINT_MAX
                 ? "the same road point is nearest both ends" : "no road near one end");
        return false;
    }
    std::vector<uint32_t> path;
    if (!ShortestPath(n, a, b, &path) || path.size() < 2) {
        F3A_INFO("RoadNet: the roads near the two ends do not connect");
        return false;
    }
    float road = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) {
        const Node& p = n.nodes[path[i - 1]];
        const Node& q = n.nodes[path[i]];
        road += Dist2D(p.x, p.y, q.x, q.y);
    }
    // Fewer, longer legs: keep a road point only every kMinSegment of road.
    std::vector<uint32_t> kept{ path.front() };
    float acc = 0.0f;
    for (size_t i = 1; i + 1 < path.size(); ++i) {
        const Node& p = n.nodes[path[i - 1]];
        const Node& q = n.nodes[path[i]];
        acc += Dist2D(p.x, p.y, q.x, q.y);
        if (acc >= kMinSegment) { kept.push_back(path[i]); acc = 0.0f; }
    }
    kept.push_back(path.back());

    const Node& na = n.nodes[kept.front()];
    const Node& nb = n.nodes[kept.back()];
    float toRoad = Dist2D(from.x, from.y, na.x, na.y);
    float offRoad = Dist2D(nb.x, nb.y, to.x, to.y);
    float total = toRoad + road + offRoad;
    float ratio = total / direct;
    float tnx = na.x - from.x, tny = na.y - from.y;
    float ttx = to.x - from.x, tty = to.y - from.y;
    float ln = std::sqrt(tnx * tnx + tny * tny), lt = std::sqrt(ttx * ttx + tty * tty);
    // Right next to the first road point, its direction means nothing.
    float dot = (ln > 300.0f && lt > 1.0f) ? (tnx * ttx + tny * tty) / (ln * lt) : 1.0f;
    bool accept = ratio <= kMaxDetour && dot >= kMinDirectionDot;
    F3A_INFO("RoadNet: road route %s - %u leg(s), walk %.0f vs direct %.0f (ratio %.2f, "
             "to road %.0f, road %.0f, off road %.0f, direction %.2f)",
             accept ? "taken" : "rejected", (unsigned)kept.size(), total, direct, ratio,
             toRoad, road, offRoad, dot);
    if (!accept) return false;
    out->clear();
    for (uint32_t k : kept) out->push_back({ n.nodes[k].x, n.nodes[k].y, n.nodes[k].z });
    if (walk) *walk = total;
    return true;
}

} // namespace f3a::roadnet
