// Quest objects table. See include/f3a/questobjs.h.

#include "f3a/questobjs.h"
#include "f3a/game_access.h"
#include "f3a/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace f3a::questobjs {
namespace {

struct Row { uint32_t ref; Entry e; };

std::mutex g_mutex;
bool g_loaded = false;
bool g_resolved = false;
std::vector<std::string> g_modules;
std::vector<Row> g_raw;        // file numbering
std::vector<Row> g_rows;       // game numbering, sorted by ref

template <class T> bool Get(FILE* f, T* v) { return std::fread(v, sizeof(T), 1, f) == 1; }

std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void ResolveLocked()
{
    if (g_resolved || !g_loaded) return;
    g_resolved = true;
    std::vector<std::string> plugins = game::LoadedPlugins();
    if (plugins.empty()) plugins.push_back("Fallout3.esm");
    std::vector<int> byte(g_modules.size(), -1);
    for (size_t m = 0; m < g_modules.size(); ++m)
        for (size_t i = 0; i < plugins.size(); ++i)
            if (Lower(plugins[i]) == Lower(g_modules[m])) { byte[m] = (int)i; break; }
    auto map = [&](uint32_t id) -> uint32_t {
        uint32_t m = id >> 24;
        if (!id || m >= byte.size() || byte[m] < 0) return 0;
        return ((uint32_t)byte[m] << 24) | (id & 0x00FFFFFFu);
    };
    g_rows.clear();
    for (const auto& r : g_raw) {
        Row o = r;
        o.ref = map(r.ref);
        o.e.quest = map(r.e.quest);
        if (o.ref && o.e.quest) g_rows.push_back(o);
    }
    std::sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return a.ref < b.ref; });
    F3A_INFO("QuestObjects: %u of %u entries for the loaded plugins", (unsigned)g_rows.size(),
             (unsigned)g_raw.size());
}

} // namespace

bool Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("QuestObjects: no table - the quest objects category stays empty");
        return false;
    }
    char magic[4];
    uint32_t version = 0, nm = 0, n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "F3AQ", 4) == 0 &&
              Get(f, &version) && version == 1 && Get(f, &nm) && Get(f, &n) &&
              nm >= 1 && nm <= 64 && n <= 500000;
    std::vector<std::string> modules;
    std::vector<Row> rows;
    for (uint32_t i = 0; ok && i < nm; ++i) {
        char name[64];
        ok = std::fread(name, 1, 64, f) == 64;
        name[63] = 0;
        if (ok) modules.push_back(name);
    }
    for (uint32_t i = 0; ok && i < n; ++i) {
        Row r{};
        uint8_t pad[3];
        ok = Get(f, &r.ref) && Get(f, &r.e.quest) && Get(f, &r.e.stage) &&
             Get(f, &r.e.objective) && Get(f, &r.e.kind) && std::fread(pad, 1, 3, f) == 3;
        if (ok) ok = r.e.kind >= kActivator && r.e.kind <= kContainer;
        if (ok) rows.push_back(r);
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("QuestObjects: the table is damaged - not used");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_modules = std::move(modules);
    g_raw = std::move(rows);
    g_loaded = true;
    g_resolved = false;
    F3A_INFO("QuestObjects: table read, %u entries", (unsigned)g_raw.size());
    return true;
}

void Resolve()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
}

bool Lookup(uint32_t refid, std::vector<Entry>* out)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
    if (out) out->clear();
    auto it = std::lower_bound(g_rows.begin(), g_rows.end(), refid,
                               [](const Row& r, uint32_t id) { return r.ref < id; });
    bool any = false;
    for (; it != g_rows.end() && it->ref == refid; ++it) {
        any = true;
        if (out) out->push_back(it->e);
    }
    return any;
}

} // namespace f3a::questobjs
