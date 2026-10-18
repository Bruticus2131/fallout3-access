// Items worth a mention. See include/f3a/itemvalue.h.

#include "f3a/itemvalue.h"
#include "f3a/game_access.h"
#include "f3a/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace f3a::itemvalue {
namespace {

std::mutex g_mutex;
bool g_loaded = false;
bool g_resolved = false;
std::vector<std::string> g_modules;
std::vector<uint32_t> g_raw;      // file numbering
std::vector<uint32_t> g_ids;      // game numbering, sorted

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
    g_ids.clear();
    for (uint32_t id : g_raw) {
        uint32_t m = id >> 24;
        if (m < byte.size() && byte[m] >= 0)
            g_ids.push_back(((uint32_t)byte[m] << 24) | (id & 0x00FFFFFFu));
    }
    std::sort(g_ids.begin(), g_ids.end());
}

template <class T> bool Get(FILE* f, T* v) { return std::fread(v, sizeof(T), 1, f) == 1; }

} // namespace

bool Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("ItemValue: no table - exploration names every item");
        return false;
    }
    char magic[4];
    uint32_t version = 0, nm = 0, n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "F3AV", 4) == 0 &&
              Get(f, &version) && version == 1 && Get(f, &nm) && Get(f, &n) &&
              nm >= 1 && nm <= 64 && n <= 200000;
    std::vector<std::string> modules;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; ok && i < nm; ++i) {
        char name[64];
        ok = std::fread(name, 1, 64, f) == 64;
        name[63] = 0;
        if (ok) modules.push_back(name);
    }
    if (ok) {
        ids.resize(n);
        ok = std::fread(ids.data(), 4, n, f) == n;
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("ItemValue: the table is damaged - not used");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_modules = std::move(modules);
    g_raw = std::move(ids);
    g_loaded = true;
    g_resolved = false;
    F3A_INFO("ItemValue: %u items worth a mention", (unsigned)g_raw.size());
    return true;
}

void Resolve()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
}

bool Worthwhile(uint32_t baseId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_loaded) return true;
    ResolveLocked();
    return std::binary_search(g_ids.begin(), g_ids.end(), baseId);
}

} // namespace f3a::itemvalue
