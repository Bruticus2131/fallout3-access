#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/strings.h"

#include <windows.h>
#include <objidl.h>
#include <shlobj.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace f3a::modules::audiodesc {
namespace {

// Audio description: what the screen shows that no sound tells.
//
// Descriptions live in a text file in the mod's language (audiodesc_pl.txt or
// audiodesc_en.txt in Fallout3Access), each under a header that says when it
// is due:
//
//   [stage <quest form id> <stage>]   the quest reaches that stage, seen
//                                     happen while playing (never a stage a
//                                     save was made in) and the quest running
//   [npc <name>]                      the first look at someone of that name
//   [npc <name> @<quest form id>]     ...while that quest is running (the
//                                     prologue has a Dad and an Amata of
//                                     every age, all with the same name)
//   [movie <file> <seconds>]          that far into one of the game's films
//                                     (the opening, the "years later" cards):
//                                     said at that moment, in real time
//
// and every line under it is said in its own turn, in the gaps between
// spoken lines - the ACB's guidance for games, and what the player asked for:
// a description is never laid over a voice. A voice is known from the
// subtitles the HUD shows (their length gives a fair estimate of how long the
// line lasts), from the screen reader still talking, and from a dialogue
// menu being open (lines there are the conversation itself).
//
// For checking the descriptions against the real picture, a capture mode
// saves a small JPEG of the game window whenever a description is due (and a
// moment later), named after it, in My Games\Fallout3\FOSE\ad_shots.

struct Entry {
    enum class Kind { Stage, Npc, Movie } kind = Kind::Stage;
    uint32_t    quest = 0;            // stage: the quest; npc: the quest context (0 = any)
    int         stage = 0;
    std::string npc;                  // npc: the name as the game shows it; movie: the file
    double      at = 0.0;             // movie: seconds into the film
    std::vector<std::string> lines;
    bool        played = false;       // once per session
    std::string tag;                  // for logs and capture file names
    std::string alias;                // npc: the name in the game's language, when the
                                      // mod speaks another (from the twin file)
    std::vector<uint32_t> ids;        // npc: their records in Fallout3.esm (every age's),
                                      // so a game in any language finds them
};

// "[npc Name @quest #id,id]": the ids taken off the end. Hex, comma-separated.
std::string TakeIds(std::string rest, std::vector<uint32_t>* ids)
{
    size_t hash = rest.rfind(" #");
    if (hash == std::string::npos) return rest;
    if (ids) {
        const char* p = rest.c_str() + hash + 2;
        while (*p) {
            char* end = nullptr;
            unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) break;
            if (v) ids->push_back(static_cast<uint32_t>(v));
            p = (*end == ',') ? end + 1 : end;
            if (*end != ',') break;
        }
    }
    rest.resize(hash);
    size_t b = rest.find_last_not_of(" \t");
    rest.resize(b == std::string::npos ? 0 : b + 1);
    return rest;
}

std::vector<Entry> g_entries;
std::vector<uint32_t> g_quests;       // quests with entries (watched)
struct QState { uint32_t id; int stage; bool known; bool running; };
std::vector<QState> g_qstate;

std::deque<std::string> g_queue;      // lines waiting for a gap
DWORD g_queued_at = 0;
std::vector<std::string> g_last;      // the last description, for the repeat key
bool  g_enabled = true;
uint32_t g_last_cross = 0;
bool  g_dialog_was = false;
std::unordered_set<uint32_t> g_seen_actors;   // refs already looked at this session

constexpr DWORD kSettleMs  = 700;     // a scene a moment to start before speaking over it
constexpr DWORD kMaxHoldMs = 20000;   // never hold a line longer than this (outside dialogue)
constexpr DWORD kLoadSettleMs = 2000; // after a load, quest readings are not trusted this long
constexpr size_t kMaxDueAtOnce = 4;   // more stage descriptions due in one look = a jump, not play
DWORD g_settle_until = 0;

// ---- file ------------------------------------------------------------------------

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("AudioDesc: no description file - nothing to describe");
        return false;
    }
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF) all.erase(0, 3);   // BOM

    Entry* cur = nullptr;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t eol = all.find('\n', pos);
        std::string line = Trim(all.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = eol == std::string::npos ? all.size() : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            std::string h = Trim(line.substr(1, line.size() - 2));
            Entry e;
            if (h.rfind("stage ", 0) == 0) {
                unsigned q = 0; int st = 0;
                if (std::sscanf(h.c_str() + 6, "%x %d", &q, &st) != 2) { cur = nullptr; continue; }
                e.kind = Entry::Kind::Stage; e.quest = q; e.stage = st;
                char tag[48]; std::snprintf(tag, sizeof(tag), "q%08X_s%d", q, st);
                e.tag = tag;
            } else if (h.rfind("npc ", 0) == 0) {
                std::string rest = TakeIds(Trim(h.substr(4)), &e.ids);
                size_t at = rest.rfind(" @");
                if (at != std::string::npos) {
                    e.quest = (uint32_t)std::strtoul(rest.c_str() + at + 2, nullptr, 16);
                    rest = Trim(rest.substr(0, at));
                }
                e.kind = Entry::Kind::Npc; e.npc = rest;
                e.tag = "npc_" + rest;
                for (auto& c : e.tag) if (c == ' ') c = '_';
            } else if (h.rfind("movie ", 0) == 0) {
                // The file name may hold spaces; the last word is the time.
                std::string rest = Trim(h.substr(6));
                size_t sp = rest.find_last_of(' ');
                if (sp == std::string::npos) { cur = nullptr; continue; }
                e.kind = Entry::Kind::Movie;
                e.at = std::atof(rest.c_str() + sp + 1);
                e.npc = Trim(rest.substr(0, sp));
                for (auto& c : e.npc) c = (char)std::tolower((unsigned char)c);
                char tag[32]; std::snprintf(tag, sizeof(tag), "movie_%.0f", e.at);
                e.tag = tag;
            } else {
                cur = nullptr;
                continue;
            }
            g_entries.push_back(std::move(e));
            cur = &g_entries.back();
            continue;
        }
        if (cur) cur->lines.push_back(line);
    }
    for (const auto& e : g_entries)
        if (e.quest && std::find(g_quests.begin(), g_quests.end(), e.quest) == g_quests.end())
            g_quests.push_back(e.quest);
    for (uint32_t q : g_quests) g_qstate.push_back({ q, 0, false, false });
    poll::WatchQuestStages(g_quests);
    F3A_INFO("AudioDesc: %u description(s), %u quest(s) watched", (unsigned)g_entries.size(),
             (unsigned)g_quests.size());
    return true;
}

// The [npc] names of the twin file, in order, as aliases of ours.
void LoadAliases(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t eol = all.find('\n', pos);
        std::string line = Trim(all.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = eol == std::string::npos ? all.size() : eol + 1;
        if (line.size() < 6 || line[0] != '[' || line.back() != ']') continue;
        std::string h = Trim(line.substr(1, line.size() - 2));
        if (h.rfind("npc ", 0) != 0) continue;
        std::string rest = TakeIds(Trim(h.substr(4)), nullptr);
        size_t at = rest.rfind(" @");
        if (at != std::string::npos) rest = Trim(rest.substr(0, at));
        names.push_back(rest);
    }
    size_t i = 0, count = 0;
    for (auto& e : g_entries) if (e.kind == Entry::Kind::Npc) ++count;
    if (count != names.size()) {
        F3A_INFO("AudioDesc: the twin file lists %u people, ours %u - no aliases",
                 (unsigned)names.size(), (unsigned)count);
        return;
    }
    for (auto& e : g_entries) if (e.kind == Entry::Kind::Npc) e.alias = names[i++];
    F3A_INFO("AudioDesc: %u people also known by the game's names", (unsigned)count);
}

// ---- capture ---------------------------------------------------------------------

ULONG_PTR g_gdip = 0;
int       g_shot_no = 0;
struct Shot { DWORD at; std::string tag; };
std::vector<Shot> g_shots;

std::wstring ShotDir()
{
    wchar_t docs[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
        return {};
    std::wstring d = std::wstring(docs) + L"\\My Games\\Fallout3\\FOSE\\ad_shots";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

HWND GameWindow()
{
    struct Ctx { DWORD pid; HWND best; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { c->best = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

bool JpegClsid(CLSID* out)
{
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || !size) return false;
    std::vector<BYTE> buf(size);
    auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, enc) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(enc[i].MimeType, L"image/jpeg") == 0) { *out = enc[i].Clsid; return true; }
    return false;
}

// The game window as the screen shows it (windowed mode), scaled to 1280 wide.
void TakeShot(const std::string& tag)
{
    if (!g_gdip) {
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&g_gdip, &in, nullptr) != Gdiplus::Ok) { g_gdip = 0; return; }
    }
    HWND hw = GameWindow();
    if (!hw) return;
    RECT rc{};
    GetClientRect(hw, &rc);
    POINT tl{ 0, 0 };
    ClientToScreen(hw, &tl);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 64 || h < 64) return;
    int ow = w > 1280 ? 1280 : w, oh = (int)((long long)h * ow / w);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, ow, oh);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetStretchBltMode(mem, HALFTONE);
    StretchBlt(mem, 0, 0, ow, oh, screen, tl.x, tl.y, w, h, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    std::wstring dir = ShotDir();
    CLSID jpg;
    if (!dir.empty() && JpegClsid(&jpg)) {
        wchar_t name[160];
        std::wstring wtag(tag.begin(), tag.end());
        swprintf(name, 160, L"\\%03d_%s.jpg", ++g_shot_no, wtag.c_str());
        Gdiplus::Bitmap img(bmp, nullptr);
        Gdiplus::EncoderParameters ep;
        ULONG quality = 80;
        ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &quality;
        bool ok = img.Save((dir + name).c_str(), &jpg, &ep) == Gdiplus::Ok;
        F3A_INFO("AudioDesc: shot %03d_%s %s", g_shot_no, tag.c_str(), ok ? "saved" : "FAILED");
    }
    DeleteObject(bmp);
}

// ---- speaking ----------------------------------------------------------------------

void Enqueue(Entry& e, const char* why)
{
    if (e.played || e.lines.empty()) return;
    e.played = true;
    F3A_INFO("AudioDesc: '%s' due (%s), %u line(s)", e.tag.c_str(), why, (unsigned)e.lines.size());
    if (g_queue.empty()) g_queued_at = GetTickCount();
    for (const auto& l : e.lines) g_queue.push_back(l);
    g_last = e.lines;
    if (config::Get().audio_desc_capture) {
        DWORD now = GetTickCount();
        g_shots.push_back({ now + 300, e.tag + "_a" });
        g_shots.push_back({ now + 3000, e.tag + "_b" });
    }
}

bool Quiet()
{
    if (menu::IsOpen(menu::Id::Dialog)) return false;      // the conversation itself
    if (tolk::IsSpeaking()) return false;
    return GetTickCount() >= poll::VoiceQuietAt();
}

void SpeakQueued()
{
    if (g_queue.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_queued_at < kSettleMs) return;
    bool held = now - g_queued_at >= kMaxHoldMs && !menu::IsOpen(menu::Id::Dialog);
    if (!Quiet() && !held) return;
    std::string line = g_queue.front();
    g_queue.pop_front();
    g_queued_at = now;                    // the next line waits for this one
    F3A_INFO("AudioDesc: says '%s'", line.c_str());
    tolk::Speak(line, tolk::Priority::Ui, false);
}

// ---- triggers --------------------------------------------------------------------

uint32_t RunningPrologueQuest()
{
    for (const auto& q : g_qstate) if (q.known && q.running) return q.id;
    return 0;
}

// The first seconds after a game was loaded: quest readings not trusted yet.
bool Settling()
{
    return g_settle_until && (LONG)(GetTickCount() - g_settle_until) < 0;
}

void CheckStages()
{
    // Right after a load the game is still putting the save's quests in
    // place (and "Continue" shows the HUD a moment before the save is read,
    // with every quest at stage 0): nothing read then is trusted.
    if (Settling()) return;
    std::vector<Entry*> due;
    for (auto& q : g_qstate) {
        int st = 0;
        bool running = false;
        if (!poll::QuestStage(q.id, &st, &running)) continue;
        if (!q.known) {
            // The first reading after a load is only where things stand: a
            // description belongs to the moment its stage is reached in play,
            // never to a stage the save was made in.
            q.known = true;
            q.running = running;
            q.stage = st;
            continue;
        }
        q.running = running;
        if (st == q.stage) continue;
        int from = q.stage;
        q.stage = st;
        // Only progress of a quest that is running: the prologue's quests are
        // stopped when they end, so their scenes can never come back later.
        if (st < from || !running) continue;
        // Every stage passed since the last look, in order: several can be set
        // within one frame.
        for (auto& e : g_entries)
            if (e.kind == Entry::Kind::Stage && e.quest == q.id && e.stage > from && e.stage <= st &&
                !e.played)
                due.push_back(&e);
    }
    if (due.empty()) return;
    if (due.size() > kMaxDueAtOnce) {
        // Many scenes at once is not a scene being played - the quest states
        // jumped (a load the mod did not see). Said nothing.
        F3A_INFO("AudioDesc: %u descriptions due at once - a jump in the quests, not play; "
                 "none said", (unsigned)due.size());
        return;
    }
    std::sort(due.begin(), due.end(), [](Entry* a, Entry* b) {
        return a->quest != b->quest ? a->quest < b->quest : a->stage < b->stage;
    });
    for (Entry* e : due) Enqueue(*e, "stage reached");
}

// Someone looked at for the first time - under the crosshair, or the one who
// opened a conversation.
void DescribeActor(uint32_t refid)
{
    if (!refid || g_seen_actors.count(refid)) return;
    auto actors = game::ScanNearby(2500, 64, /*actors_only*/true, false);
    for (const auto& a : actors) {
        if (a.form_id != refid) continue;
        g_seen_actors.insert(refid);
        uint32_t ctx = RunningPrologueQuest();
        Entry* best = nullptr;
        for (auto& e : g_entries) {
            if (e.kind != Entry::Kind::Npc || e.played) continue;
            // By the name the game shows, or by the record behind it - the
            // names differ between the game's languages, the records do not.
            if (e.npc != a.name && e.alias != a.name &&
                std::find(e.ids.begin(), e.ids.end(), a.base_id) == e.ids.end())
                continue;
            if (e.quest && e.quest != ctx) continue;
            if (!best || (e.quest && !best->quest)) best = &e;   // the age-specific one wins
        }
        if (best) Enqueue(*best, "first look");
        return;
    }
}

void CheckActors()
{
    // Not until the quests' state is known: a person looked at in the first
    // seconds after a load would be marked seen with no quest to tell which
    // age's description is theirs, and never described.
    if (Settling()) return;
    for (const auto& q : g_qstate) if (!q.known) return;
    uint32_t cross = game::GetCrosshairActorID();
    if (cross && cross != g_last_cross) DescribeActor(cross);
    g_last_cross = cross;
    bool dialog = menu::IsOpen(menu::Id::Dialog);
    if (dialog && !g_dialog_was) {
        // A conversation someone else started: the nearest person is the one.
        auto closest = game::ScanNearby(400, 2, /*actors_only*/true, false);
        if (!closest.empty()) DescribeActor(closest[0].form_id);
    }
    g_dialog_was = dialog;
}

// The game's films: each description at its moment, in real time (a film
// has no gaps to wait for - the narration of the opening runs for two
// minutes - so the lines are timed to its pauses in the file itself).
bool     g_movies = true;
uint32_t g_movie_seq = 0;

void CheckMovies()
{
    std::string name;
    double t = 0.0;
    uint32_t seq = 0;
    if (!game::CurrentMovie(&name, &t, &seq)) return;
    if (seq != g_movie_seq) {
        // A new playback: its descriptions may be said again.
        g_movie_seq = seq;
        for (auto& e : g_entries) if (e.kind == Entry::Kind::Movie && e.npc == name) e.played = false;
        F3A_INFO("AudioDesc: film '%s' started", name.c_str());
    }
    if (!g_movies) return;
    for (auto& e : g_entries) {
        if (e.kind != Entry::Kind::Movie || e.played || e.npc != name || t < e.at) continue;
        e.played = true;
        // Far behind (the film was skipped ahead): only the latest one.
        if (t - e.at > 6.0) continue;
        for (const auto& l : e.lines) {
            F3A_INFO("AudioDesc: film %.1f s says '%s'", t, l.c_str());
            tolk::Speak(l, tolk::Priority::Ui, false);
        }
        g_last = e.lines;
    }
}

// ---- keys ---------------------------------------------------------------------------

void RepeatKey()
{
    if (hotkeys::ShiftHeldNow()) {
        g_enabled = !g_enabled;
        if (!g_enabled) g_queue.clear();
        // Back on: where the quests stand now is read afresh, silently - the
        // stages passed while it was off are not said all at once.
        if (g_enabled) for (auto& q : g_qstate) q.known = false;
        tolk::Speak(g_enabled
                        ? strings::T("Audiodeskrypcja włączona.", "Audio description on.")
                        : strings::T("Audiodeskrypcja wyłączona.", "Audio description off."),
                    tolk::Priority::Ui, true);
        return;
    }
    if (g_last.empty()) {
        tolk::Speak(strings::T("Nie było jeszcze opisu.", "No description yet."),
                    tolk::Priority::Ui, true);
        return;
    }
    std::string all;
    for (const auto& l : g_last) { if (!all.empty()) all += " "; all += l; }
    tolk::Speak(all, tolk::Priority::Ui, true);
}

} // namespace

void OnLoadingClosed(bool gameLoad)
{
    // Through a door: nothing changes - a scene that starts with the door (the
    // vault exit) is said once the new place is up (Tick waits out the load).
    if (!gameLoad) return;
    // A game was loaded (or started): where the quests stand is read afresh,
    // silently, once the load has settled; nothing from before it is said,
    // and everything may be described again (an earlier save replayed).
    for (auto& q : g_qstate) { q.known = false; q.running = false; }
    for (auto& e : g_entries) if (e.kind != Entry::Kind::Movie) e.played = false;
    g_seen_actors.clear();
    g_queue.clear();
    g_shots.clear();
    g_last.clear();
    g_last_cross = 0;
    g_dialog_was = false;
    g_settle_until = (GetTickCount() + kLoadSettleMs) | 1;   // 0 = no load seen yet
}

void ToggleMovies()
{
    g_movies = !g_movies;
    tolk::Speak(g_movies
                    ? strings::T("Audiodeskrypcja filmów włączona.", "Film audio description on.")
                    : strings::T("Audiodeskrypcja filmów wyłączona.", "Film audio description off."),
                tolk::Priority::System, true);
}

// The descriptions in the mod's language, English when there is none for it.
// Again when the language changes: a translation plugin over an English
// Fallout3.esm is only seen once the game has read its data, after Init.
std::string g_loaded_lang;

void LoadForLanguage()
{
    g_entries.clear();
    g_quests.clear();
    g_qstate.clear();
    g_queue.clear();
    g_seen_actors.clear();
    g_loaded_lang = strings::Language();
    std::wstring dir = config::DataDir();
    if (dir.empty()) return;
    const std::string& code = g_loaded_lang;
    if (!Load(dir + L"\\audiodesc_" + std::wstring(code.begin(), code.end()) + L".txt"))
        Load(dir + L"\\audiodesc_en.txt");
    // People are matched by the name the GAME shows. When the mod speaks
    // another language than the game, take the game's names from the twin
    // file (the two files list the people in the same order).
    const std::string& game = config::Get().game_language;
    bool gamePl = game == "pl";
    if ((game == "pl" || game == "en") && gamePl != strings::Polish())
        LoadAliases(dir + (gamePl ? L"\\audiodesc_pl.txt" : L"\\audiodesc_en.txt"));
}

void Tick(float)
{
    // Only between games (the main menu, where the language is settled), so a
    // scene half way through is never forgotten.
    if (strings::Language() != g_loaded_lang && !poll::IsGameplayActive()) {
        F3A_INFO("AudioDesc: the language is now '%s' - descriptions reloaded",
                 strings::Language().c_str());
        LoadForLanguage();
    }
    if (!g_enabled || !config::Get().audio_desc || g_entries.empty()) return;
    CheckMovies();                         // films play outside gameplay too
    if (!poll::IsGameplayActive() && !menu::IsOpen(menu::Id::Race)) return;
    // Nothing over a loading screen or a film (the "years later" cards): a
    // scene's description waits until the scene is on screen.
    if (menu::IsOpen(menu::Id::Loading) || game::CurrentMovie(nullptr, nullptr, nullptr)) return;
    CheckStages();
    CheckActors();
    SpeakQueued();
    DWORD now = GetTickCount();
    for (size_t i = 0; i < g_shots.size();) {
        if (now >= g_shots[i].at) {
            TakeShot(g_shots[i].tag);
            g_shots.erase(g_shots.begin() + i);
        } else {
            ++i;
        }
    }
}

void Init()
{
    g_enabled = config::Get().audio_desc;
    g_movies  = config::Get().intro_audio_desc;
    LoadForLanguage();
    hotkeys::Bind(config::Get().hotkeys.audio_desc_repeat, &RepeatKey);
    F3A_INFO("AudioDesc module ready (%s%s).", g_enabled ? "on" : "off",
             config::Get().audio_desc_capture ? ", capturing screenshots" : "");
}

void Shutdown()
{
    g_queue.clear();
    if (g_gdip) { Gdiplus::GdiplusShutdown(g_gdip); g_gdip = 0; }
}

} // namespace f3a::modules::audiodesc
