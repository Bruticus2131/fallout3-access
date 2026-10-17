// The world's load-door graph. See include/f3a/worlddoors.h.

#include "f3a/worlddoors.h"
#include "f3a/logger.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace f3a::worlddoors {
namespace {

struct Space { uint32_t id; uint32_t flags; std::string name; };
struct Door  { uint32_t id; uint32_t space; float x, y, z; uint32_t dest; uint8_t kind; };
struct Ref   { uint32_t id; uint32_t space; };

// The file as read: ids numbered by its module table (0 = Fallout3.esm),
// door versions per plugin, places and links by id. Resolve() turns it into
// the tables below once the game has loaded its plugins.
struct RawDoor { uint32_t id, space; float x, y, z; uint32_t dest; uint8_t module, kind; };
struct Raw {
    std::vector<std::string> modules;
    std::vector<Space>   spaces;
    std::vector<RawDoor> doors;        // sorted by id, then module
    std::vector<Ref>     refs;         // space = space id
};

constexpr uint8_t kKindDeleted = 0xFE;   // the plugin removed the door
constexpr uint8_t kKindLanding = 0xFF;   // where a travel point lands the player

std::mutex g_mutex;
bool g_loaded = false;
bool g_resolved = false;
Raw g_raw;
std::vector<Space> g_spaces;                       // index = space index
std::unordered_map<uint32_t, uint32_t> g_space_of; // form id -> space index
std::vector<Door>  g_doors;                        // dest = door index
std::vector<std::vector<uint32_t>> g_by_space;     // space index -> door indices
std::vector<Ref>   g_refs;                         // sorted by id, space = index

constexpr float kDoorCost = 100.0f;   // going through a door: a load screen

float Dist(float ax, float ay, float az, float bx, float by, float bz)
{
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

template <class T> bool Get(FILE* f, T* v) { return std::fread(v, sizeof(T), 1, f) == 1; }

std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The file's ids -> the game's: each module's load-order byte, or a DLC the
// game has not loaded (its places, doors and references are left out, and a
// door leading into one leads nowhere known). Under g_mutex.
void ResolveLocked()
{
    if (g_resolved || !g_loaded) return;
    g_resolved = true;
    std::vector<std::string> plugins = game::LoadedPlugins();
    if (plugins.empty()) {
        F3A_INFO("WorldDoors: the game's plugin list cannot be read - Fallout3.esm's doors only");
        plugins.push_back("Fallout3.esm");
    }
    std::vector<int> byte(g_raw.modules.size(), -1);
    for (size_t m = 0; m < g_raw.modules.size(); ++m)
        for (size_t i = 0; i < plugins.size(); ++i)
            if (Lower(plugins[i]) == Lower(g_raw.modules[m])) { byte[m] = (int)i; break; }
    auto map = [&](uint32_t id) -> uint32_t {
        uint32_t m = id >> 24;
        if (!id || m >= byte.size() || byte[m] < 0) return 0;
        return ((uint32_t)byte[m] << 24) | (id & 0x00FFFFFFu);
    };
    g_spaces.clear();
    g_space_of.clear();
    for (const auto& sp : g_raw.spaces) {
        uint32_t id = map(sp.id);
        if (!id) continue;
        g_space_of[id] = (uint32_t)g_spaces.size();
        g_spaces.push_back({ id, sp.flags, sp.name });
    }
    // Of each door's versions, the last plugin's that the game has loaded.
    g_doors.clear();
    std::vector<uint32_t> destId;
    const auto& rd = g_raw.doors;
    for (size_t i = 0; i < rd.size();) {
        size_t j = i;
        const RawDoor* use = nullptr;
        for (; j < rd.size() && rd[j].id == rd[i].id; ++j)
            if (rd[j].module < byte.size() && byte[rd[j].module] >= 0) use = &rd[j];
        i = j;
        if (!use || use->kind == kKindDeleted) continue;
        uint32_t id = map(use->id);
        auto sp = g_space_of.find(map(use->space));
        if (!id || sp == g_space_of.end()) continue;
        g_doors.push_back({ id, sp->second, use->x, use->y, use->z, 0xFFFFFFFFu, use->kind });
        destId.push_back(map(use->dest));
    }
    std::unordered_map<uint32_t, uint32_t> doorOf;
    for (uint32_t i = 0; i < g_doors.size(); ++i) doorOf[g_doors[i].id] = i;
    for (uint32_t i = 0; i < g_doors.size(); ++i) {
        auto it = destId[i] ? doorOf.find(destId[i]) : doorOf.end();
        if (it != doorOf.end()) g_doors[i].dest = it->second;
    }
    g_by_space.assign(g_spaces.size(), {});
    for (uint32_t i = 0; i < g_doors.size(); ++i) g_by_space[g_doors[i].space].push_back(i);
    g_refs.clear();
    for (const auto& r : g_raw.refs) {
        uint32_t id = map(r.id);
        auto sp = g_space_of.find(map(r.space));
        if (id && sp != g_space_of.end()) g_refs.push_back({ id, sp->second });
    }
    std::sort(g_refs.begin(), g_refs.end(), [](const Ref& a, const Ref& b) { return a.id < b.id; });
    std::string dlcs;
    for (size_t m = 1; m < g_raw.modules.size(); ++m)
        if (byte[m] >= 0) dlcs += (dlcs.empty() ? "" : ", ") + g_raw.modules[m];
    F3A_INFO("WorldDoors: %u places, %u doors and travel points, %u persistent references "
             "(DLCs: %s)", (unsigned)g_spaces.size(), (unsigned)g_doors.size(),
             (unsigned)g_refs.size(), dlcs.empty() ? "none" : dlcs.c_str());
}

} // namespace

bool Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("WorldDoors: no door table - walks to other places use nearby doors only");
        return false;
    }
    char magic[4];
    uint32_t version = 0, nm = 0, ns = 0, nd = 0, nr = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "F3AD", 4) == 0 &&
              Get(f, &version) && version == 2 && Get(f, &nm) && Get(f, &ns) &&
              Get(f, &nd) && Get(f, &nr) &&
              nm >= 1 && nm <= 64 && ns <= 100000 && nd <= 200000 && nr <= 2000000;
    Raw raw;
    for (uint32_t i = 0; ok && i < nm; ++i) {
        char name[64];
        ok = std::fread(name, 1, 64, f) == 64;
        name[63] = 0;
        if (ok) raw.modules.push_back(name);
    }
    for (uint32_t i = 0; ok && i < ns; ++i) {
        uint32_t id = 0, flags = 0;
        char name[64];
        ok = Get(f, &id) && Get(f, &flags) && std::fread(name, 1, 64, f) == 64;
        name[63] = 0;
        if (ok) raw.spaces.push_back({ id, flags, name });
    }
    for (uint32_t i = 0; ok && i < nd; ++i) {
        RawDoor d{};
        uint16_t pad = 0;
        ok = Get(f, &d.id) && Get(f, &d.space) && Get(f, &d.x) && Get(f, &d.y) &&
             Get(f, &d.z) && Get(f, &d.dest) && Get(f, &d.module) && Get(f, &d.kind) &&
             Get(f, &pad);
        if (ok) ok = std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z) &&
                     d.module < nm && (i == 0 || raw.doors.back().id <= d.id);
        if (ok) raw.doors.push_back(d);
    }
    for (uint32_t i = 0; ok && i < nr; ++i) {
        Ref r{};
        ok = Get(f, &r.id) && Get(f, &r.space);
        if (ok) raw.refs.push_back(r);
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("WorldDoors: the door table is damaged or of an older kind - not used");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    F3A_INFO("WorldDoors: door table read (%u plugins, %u places, %u door versions); used once "
             "the game has loaded its plugins", nm, ns, nd);
    g_raw = std::move(raw);
    g_loaded = true;
    g_resolved = false;
    return true;
}

void Resolve()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
}

bool Loaded()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_loaded;
}

uint32_t RefSpace(uint32_t refid)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    auto it = std::lower_bound(g_refs.begin(), g_refs.end(), refid,
                               [](const Ref& r, uint32_t id) { return r.id < id; });
    if (it == g_refs.end() || it->id != refid) return 0;
    return g_spaces[it->space].id;
}

std::string SpaceName(uint32_t space)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    auto it = g_space_of.find(space);
    return it == g_space_of.end() ? std::string() : g_spaces[it->second].name;
}

bool NextDoor(uint32_t fromSpace, const game::Vec3& from,
              uint32_t toSpace, const game::Vec3& to, Hop* out,
              const std::vector<uint32_t>* exclude, const AreaFn* area)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    if (!g_loaded || !out) return false;
    auto fs = g_space_of.find(fromSpace);
    auto ts = g_space_of.find(toSpace);
    if (fs == g_space_of.end() || ts == g_space_of.end()) return false;
    const uint32_t S = fs->second, T = ts->second;

    // Dijkstra over doors. A door's cost = the way to stand at it; going
    // through it lands at its linked door, and from there every door of
    // that space (or the target, in the target's space) is a straight walk.
    const size_t N = g_doors.size();
    auto excluded = [&](uint32_t i) {
        if (!exclude) return false;
        for (uint32_t id : *exclude) if (id == g_doors[i].id) return true;
        return false;
    };
    // Which part of its space each door stands in (asked once per door).
    std::vector<int> doorArea(N, -2);
    auto areaOfDoor = [&](uint32_t i) {
        if (doorArea[i] == -2) {
            const Door& d = g_doors[i];
            doorArea[i] = area ? (*area)(g_spaces[d.space].id, game::Vec3{ d.x, d.y, d.z }) : -1;
        }
        return doorArea[i];
    };
    auto same = [](int a, int b) { return a < 0 || b < 0 || a == b; };
    const int fromArea = area ? (*area)(fromSpace, from) : -1;
    const int toArea   = area ? (*area)(toSpace, to) : -1;
    std::vector<float> dist(N, (std::numeric_limits<float>::max)());
    std::vector<uint32_t> prev(N, 0xFFFFFFFFu);
    using Q = std::pair<float, uint32_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
    for (uint32_t i : g_by_space[S]) {
        if (excluded(i) || !same(fromArea, areaOfDoor(i))) continue;
        const Door& d = g_doors[i];
        float c = Dist(from.x, from.y, from.z, d.x, d.y, d.z);
        if (c < dist[i]) { dist[i] = c; pq.push({ c, i }); }
    }
    float best = (std::numeric_limits<float>::max)();
    uint32_t bestDoor = 0xFFFFFFFFu;               // last door before the target
    // Same space and same part of it: walking there needs no door.
    if (S == T && same(fromArea, toArea)) best = Dist(from.x, from.y, from.z, to.x, to.y, to.z);
    while (!pq.empty()) {
        auto [c, i] = pq.top();
        pq.pop();
        if (c > dist[i]) continue;
        if (c >= best) break;
        const Door& d = g_doors[i];
        if (d.dest == 0xFFFFFFFFu) continue;
        const Door& land = g_doors[d.dest];
        float cl = c + kDoorCost;
        const int landArea = areaOfDoor(d.dest);
        if (land.space == T && same(landArea, toArea)) {
            float total = cl + Dist(land.x, land.y, land.z, to.x, to.y, to.z);
            if (total < best) { best = total; bestDoor = i; }
        }
        for (uint32_t k : g_by_space[land.space]) {
            if (k == d.dest || excluded(k) || !same(landArea, areaOfDoor(k)))
                continue;   // straight back out / missing / another part of the space
            const Door& e = g_doors[k];
            float ck = cl + Dist(land.x, land.y, land.z, e.x, e.y, e.z);
            if (ck < dist[k]) { dist[k] = ck; prev[k] = i; pq.push({ ck, k }); }
        }
    }
    if (bestDoor == 0xFFFFFFFFu) return false;      // none, or no door needed
    // Back to the first door of the chain.
    uint32_t first = bestDoor;
    int count = 1;
    while (prev[first] != 0xFFFFFFFFu && count < 64) { first = prev[first]; ++count; }
    const Door& fd = g_doors[first];
    out->door  = fd.id;
    out->pos   = { fd.x, fd.y, fd.z };
    out->to_space = fd.dest != 0xFFFFFFFFu ? g_spaces[g_doors[fd.dest].space].id : 0;
    out->to_name  = fd.dest != 0xFFFFFFFFu ? g_spaces[g_doors[fd.dest].space].name : "";
    out->doors = count;
    out->walk  = best;
    out->travel = fd.kind == kKindLanding ? 0 : fd.kind;
    return true;
}

} // namespace f3a::worlddoors
