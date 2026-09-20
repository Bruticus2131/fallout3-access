#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

namespace f3a::modules::hacking {
namespace {

// Accessible terminal hacking. The minigame is MOUSE-ONLY (hover a word, click),
// so a blind player can't select. So we:
//   * read the candidate words; UP/DOWN browse them (+ spelled),
//   * ENTER guesses the chosen word — we MOVE the game cursor onto it (mouse
//     injection) and left-click, exactly like a sighted click. The click only
//     fires once the ENGINE confirms the cursor is on that word (the grid tile's
//     user trait, game::GetHackingHighlightedWord) — geometry alone can land a
//     character off and waste an attempt on the wrong word.
//   * LEFT/RIGHT browse every readable line of the screen (header + attempt log),
//   * read the attempt log live (the response after a guess), entry by entry.
std::vector<std::string> g_words;
int         g_index   = -1;
std::string g_key;             // announced grid state (the word set)
std::string g_pending;
DWORD       g_since   = 0;
int         g_attempts = 0;    // last announced attempts remaining
bool        g_locked_said = false;

std::vector<std::string> g_lines;   // current screen text lines (left/right)
int         g_line_index = -1;

// Attempt-log reading: entries arrive across several frames, so let the COUNT
// settle, then diff by content against what we've already spoken.
std::vector<std::string> g_spoken_log;
int  g_log_count   = 0;
int  g_log_pending = 0;        // non-zero = settle in progress

// Mouse-driven guess state.
bool  g_guessing = false;
int   g_gindex   = -1;         // word being guessed
float g_gtx = 0, g_gty = 0;
int   g_gbudget  = 0;
int   g_scan     = 0;          // sideways search step (0 = not searching)
int   g_scan_off = 0;          // current sideways offset, px

// Does this build actually fill the highlighted-word trait? Latched on the first
// non-empty read; until then the guess falls back to pure geometry.
bool        g_verify_ok = false;
std::string g_hl;              // last announced highlighted word

constexpr int kScanSteps = 20;   // ±60 px sideways search for the word
constexpr int kScanPx    = 6;

bool g_up = false, g_down = false, g_left = false, g_right = false, g_enter = false;

bool KeyEdge(int vk, bool* prev)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !*prev;
    *prev = down;
    return edge;
}
void SeedKeyEdges()
{
    g_up    = (GetAsyncKeyState(VK_UP)     & 0x8000) != 0;
    g_down  = (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0;
    g_left  = (GetAsyncKeyState(VK_LEFT)   & 0x8000) != 0;
    g_right = (GetAsyncKeyState(VK_RIGHT)  & 0x8000) != 0;
    g_enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
}
std::string Spell(const std::string& w)
{
    std::string s;
    for (char c : w) { if (!s.empty()) s += ' '; s += c; }
    return s;
}
std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (auto& e : v) { if (!s.empty()) s += " "; s += e; }
    return s;
}
// Polish plural: 1 proba / 2-4 proby / 5+ prob.
std::string Attempts(int n)
{
    if (n == 1)             return "1 proba";
    if (n >= 2 && n <= 4)   return std::to_string(n) + " proby";
    return std::to_string(n) + " prob";
}
void MouseMoveRel(long dx, long dy)
{
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = dx; in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE; SendInput(1, &in, sizeof(in));
}
void MouseClick()
{
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}
void EndGuess() { g_guessing = false; g_gindex = -1; g_scan = 0; g_scan_off = 0; }

void Reset()
{
    g_key.clear(); g_pending.clear(); g_words.clear(); g_lines.clear();
    g_spoken_log.clear(); g_hl.clear();
    g_index = -1; g_line_index = -1; g_attempts = 0;
    g_log_count = 0; g_log_pending = 0; g_locked_said = false;
    EndGuess();
}

// Read newly-appended log entries once the count has settled.
void PollLog()
{
    std::vector<std::string> log = game::GetHackingLogEntries();
    if (g_log_pending > 0) {
        if (static_cast<int>(log.size()) > g_log_pending) {
            g_log_pending = static_cast<int>(log.size());   // still arriving
            return;
        }
        int grew = static_cast<int>(log.size()) - g_log_count;
        std::string add;
        for (auto& e : log) {
            if (std::find(g_spoken_log.begin(), g_spoken_log.end(), e) != g_spoken_log.end())
                continue;
            if (!add.empty()) add += ", ";
            add += e;
            g_spoken_log.push_back(e);
        }
        // A repeat of earlier text (a second "Wpis odrzucony") is invisible to
        // the content diff — fall back to reading the newest `grew` rows.
        if (add.empty() && grew > 0) {
            for (size_t i = log.size() - grew; i < log.size(); ++i) {
                if (!add.empty()) add += ", ";
                add += log[i];
            }
        }
        g_log_count   = static_cast<int>(log.size());
        g_log_pending = 0;
        if (!add.empty()) tolk::Speak(add, tolk::Priority::Ui, false);
    } else if (static_cast<int>(log.size()) > g_log_count) {
        g_log_pending = static_cast<int>(log.size());
    }
}

// Drive the game cursor onto the word being guessed and click it. Returns when
// the guess is resolved (clicked or given up).
void TickGuess()
{
    if (--g_gbudget <= 0) {
        EndGuess();
        tolk::Speak("Nie moge trafic w slowo.", tolk::Priority::System, true);
        return;
    }
    const std::string want = (g_gindex >= 0 && g_gindex < static_cast<int>(g_words.size()))
                             ? g_words[g_gindex] : std::string();
    if (want.empty()) { EndGuess(); return; }

    std::string hl = game::GetHackingHighlightedWord();
    if (!hl.empty()) g_verify_ok = true;

    // The engine says the cursor is on our word — click it.
    if (g_verify_ok && hl == want) { MouseClick(); EndGuess(); return; }

    float cx, cy;
    if (!game::GetHackingCursor(&cx, &cy)) { EndGuess(); return; }
    float dx = g_gtx - cx, dy = g_gty - cy;

    if (std::fabs(dx) < 18.0f && std::fabs(dy) < 14.0f) {
        // On the computed spot. Without engine verification this is as good as
        // it gets — click (the old behaviour). With it, the cursor is on the row
        // but on a neighbouring character, so sweep sideways until the engine
        // reports our word.
        if (!g_verify_ok) { MouseClick(); EndGuess(); return; }
        if (g_scan > kScanSteps) {
            EndGuess();
            tolk::Speak("Nie moge trafic w slowo.", tolk::Priority::System, true);
            return;
        }
        if (g_scan == 0) g_scan = 1;
        int desired = ((g_scan + 1) / 2) * kScanPx * ((g_scan % 2) ? 1 : -1);
        MouseMoveRel(desired - g_scan_off, 0);
        g_scan_off = desired;
        ++g_scan;
        return;
    }

    long mx = static_cast<long>(dx); if (mx > 110) mx = 110; if (mx < -110) mx = -110;
    long my = static_cast<long>(dy); if (my > 110) my = 110; if (my < -110) my = -110;
    if (mx > -2 && mx < 2) mx = (dx > 0 ? 2 : -2);
    if (my > -2 && my < 2) my = (dy > 0 ? 2 : -2);
    MouseMoveRel(mx, my);
}

} // namespace

void Tick(float)
{
    auto h = game::GetHackingInfo();
    if (!h.active) {
        if (!g_key.empty() || g_locked_said) Reset();
        return;
    }
    if (game::IsPauseMenuOpen()) return;   // pause menu over hacking — ignore input

    // Locked out: nothing left to guess, just read the lock-out screen once.
    if (h.locked && h.words.empty()) {
        if (!g_locked_said) {
            g_locked_said = true;
            std::string s = Join(game::GetHackingTextLines());
            tolk::Speak(s.empty() ? "Terminal zablokowany." : s, tolk::Priority::Ui, true);
        }
        return;
    }

    // The grid identity is the WORD SET (not the attempt count) — so a guess
    // announces its result instead of replaying the whole intro line.
    std::string key;
    for (auto& w : h.words) key += w + ",";
    if (key != g_key) {
        if (key != g_pending) { g_pending = key; g_since = GetTickCount(); return; }
        if (GetTickCount() - g_since < 800) return;   // let the type-out settle
        g_key = key; g_words = h.words; g_index = -1;
        g_attempts = h.attempts;
        int L = g_words.empty() ? 0 : static_cast<int>(g_words.front().size());
        char buf[224];
        std::snprintf(buf, sizeof(buf),
            "Hakowanie. %s. %d slow po %d liter. "
            "Gora dol wybiera slowo, Enter zgaduje, lewo prawo czyta ekran.",
            Attempts(h.attempts).c_str(), static_cast<int>(g_words.size()), L);
        tolk::Speak(buf, tolk::Priority::Ui, true);
        SeedKeyEdges();
        g_spoken_log = game::GetHackingLogEntries();   // don't replay an old log
        g_log_count = static_cast<int>(g_spoken_log.size());
        g_log_pending = 0;
        g_line_index = -1;
        g_hl.clear();
        return;
    }

    // ---- Mouse-driven guess in progress ----
    if (g_guessing) { TickGuess(); return; }   // no other input while guessing

    // ---- Live: the attempt log (guess responses) ----
    PollLog();

    // Attempts spent — short announce (the log carries the reason).
    if (h.attempts != g_attempts) {
        g_attempts = h.attempts;
        tolk::Speak(Attempts(h.attempts) + " pozostalo", tolk::Priority::Ui, false);
    }

    // The engine tells us which word the cursor is on, so the player can also
    // sweep the grid with their own mouse and hear what they're pointing at.
    if (!h.highlighted.empty()) {
        g_verify_ok = true;
        if (h.highlighted != g_hl) {
            g_hl = h.highlighted;
            tolk::Speak(g_hl + ", " + Spell(g_hl), tolk::Priority::Ui, true);
        }
    } else {
        g_hl.clear();
    }

    // ---- Keys ----
    bool up    = KeyEdge(VK_UP,     &g_up);
    bool down  = KeyEdge(VK_DOWN,   &g_down);
    bool left  = KeyEdge(VK_LEFT,   &g_left);
    bool right = KeyEdge(VK_RIGHT,  &g_right);
    bool enter = KeyEdge(VK_RETURN, &g_enter);

    if ((up || down) && !g_words.empty()) {
        int n = static_cast<int>(g_words.size());
        if (g_index < 0) g_index = up ? n - 1 : 0;
        else             g_index = (g_index + (up ? -1 : 1) + n) % n;
        char idx[24];
        std::snprintf(idx, sizeof(idx), ", %d z %d", g_index + 1, n);
        tolk::Speak(g_words[g_index] + ", " + Spell(g_words[g_index]) + idx,
                    tolk::Priority::Ui, true);
    }
    if (left || right) {
        g_lines = game::GetHackingTextLines();
        if (!g_lines.empty()) {
            int n = static_cast<int>(g_lines.size());
            if (g_line_index < 0) g_line_index = right ? 0 : n - 1;
            else                  g_line_index = (g_line_index + (right ? 1 : -1) + n) % n;
            tolk::Speak(g_lines[g_line_index], tolk::Priority::Ui, true);
        }
    }
    if (enter && g_index >= 0 && g_index < static_cast<int>(g_words.size())) {
        float tx, ty;
        if (game::GetHackWordTarget(g_index, &tx, &ty)) {
            g_gtx = tx; g_gty = ty; g_gbudget = 150;
            g_gindex = g_index; g_guessing = true;
            g_scan = 0; g_scan_off = 0;
            tolk::Speak("Zgaduje: " + g_words[g_index], tolk::Priority::Ui, true);
        }
    }
}

void Init() { F3A_INFO("Hacking module ready."); }
void Shutdown() {}

} // namespace f3a::modules::hacking
