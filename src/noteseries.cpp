// Note series table. See include/f3a/noteseries.h.

#include "f3a/noteseries.h"
#include "f3a/logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace f3a::noteseries {
namespace {

struct Row {
    uint32_t note, ref, space;
    float    x, y, z;
    uint16_t series, order;
};

std::mutex g_mutex;
bool g_loaded = false;
bool g_resolved = false;
std::vector<std::string> g_modules;
std::vector<Row> g_raw;        // file numbering
std::vector<Row> g_rows;       // game numbering, by series and order

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
        o.note = map(r.note);
        o.ref = map(r.ref);
        o.space = map(r.space);
        if (o.note && o.ref && o.space) g_rows.push_back(o);
    }
    F3A_INFO("NoteSeries: %u of %u notes for the loaded plugins", (unsigned)g_rows.size(),
             (unsigned)g_raw.size());
}

} // namespace

bool Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("NoteSeries: no table - no note series listed");
        return false;
    }
    char magic[4];
    uint32_t version = 0, nm = 0, n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "F3AN", 4) == 0 &&
              Get(f, &version) && version == 1 && Get(f, &nm) && Get(f, &n) &&
              nm >= 1 && nm <= 64 && n <= 100000;
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
        ok = Get(f, &r.note) && Get(f, &r.ref) && Get(f, &r.space) && Get(f, &r.x) &&
             Get(f, &r.y) && Get(f, &r.z) && Get(f, &r.series) && Get(f, &r.order);
        if (ok) ok = std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z);
        if (ok) rows.push_back(r);
    }
    std::fclose(f);
    if (!ok) {
        F3A_INFO("NoteSeries: the table is damaged - not used");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_modules = std::move(modules);
    g_raw = std::move(rows);
    g_loaded = true;
    g_resolved = false;
    F3A_INFO("NoteSeries: table read, %u notes", (unsigned)g_raw.size());
    return true;
}

void Resolve()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ResolveLocked();
}

std::vector<Missing> MissingNotes()
{
    std::vector<Missing> out;
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ResolveLocked();
        rows = g_rows;
    }
    if (rows.empty()) return out;
    const std::vector<uint32_t> mine = game::PlayerNoteIds();
    const std::unordered_set<uint32_t> have(mine.begin(), mine.end());
    // Per series: its notes (one note may lie in more than one place).
    size_t a = 0;
    while (a < rows.size()) {
        size_t b = a;
        while (b < rows.size() && rows[b].series == rows[a].series) ++b;
        std::unordered_set<uint32_t> notes, got;
        for (size_t i = a; i < b; ++i) {
            notes.insert(rows[i].note);
            if (have.count(rows[i].note)) got.insert(rows[i].note);
        }
        // Started (one note at least) and not finished.
        if (!got.empty() && got.size() < notes.size()) {
            for (size_t i = a; i < b; ++i) {
                if (have.count(rows[i].note)) continue;
                Missing m;
                m.note = rows[i].note;
                m.ref = rows[i].ref;
                m.space = rows[i].space;
                m.pos = { rows[i].x, rows[i].y, rows[i].z };
                m.name = game::NoteName(m.note);
                m.have = (int)got.size();
                m.total = (int)notes.size();
                if (!m.name.empty()) out.push_back(std::move(m));
            }
        }
        a = b;
    }
    return out;
}

} // namespace f3a::noteseries
