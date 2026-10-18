#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"
#include "f3a/strings.h"

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

namespace f3a::modules::hacking {
namespace {

// Accessible terminal hacking. With keyboard and mouse the minigame is mouse
// only (hover a word, click), so a blind player can't select. So we:
//   * read the candidate words; UP/DOWN browse them (+ spelled),
//   * ENTER guesses the chosen word: the GAME's cursor is put on the word's
//     middle letter, and once the game's own highlight holds that word - the
//     text a click enters - the click is made through the menu itself
//     (game::ClickHackingSelection). Geometry alone can land a letter off and
//     waste an attempt on the wrong word, so nothing is entered before the
//     game confirms it; off by a letter, the cursor steps sideways.
//     (It used to shove the real mouse and send the button down and up in one
//     batch - the game never saw that click - steering by the blinking text
//     cursor, which does not follow the mouse at all.)
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

// Guess state.
bool  g_guessing = false;
int   g_gindex   = -1;         // word being guessed
float g_gtx = 0, g_gty = 0;    // its middle letter, UI units
float g_gcw = 0;               // one letter's width there
int   g_gstep    = 0;          // cursor placings so far: the middle letter, then ±1, ±2...
int   g_gwait    = 0;          // ticks for the game to move its cursor before checking

std::string g_hl;              // last announced highlighted word

constexpr float kHackCharWidth = 17.0f;   // the grid's letter width (0x0063FF10 divides by 17)
constexpr int   kSettleTicks   = 3;

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
// Polish plural: 1 próba / 2-4 próby / 5+ prób. English: 1 attempt / N attempts.
std::string Attempts(int n)
{
    if (!strings::Polish())
        return n == 1 ? std::string("1 attempt") : std::to_string(n) + " attempts";
    if (n == 1)             return "1 próba";
    if (n >= 2 && n <= 4)   return std::to_string(n) + " próby";
    return std::to_string(n) + " prób";
}
void EndGuess() { g_guessing = false; g_gindex = -1; g_gstep = 0; g_gwait = 0; }

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

// Put the game's cursor on the word being guessed and enter it once the game
// highlights it. One step per call; done when entered or given up.
void TickGuess()
{
    const std::string want = (g_gindex >= 0 && g_gindex < static_cast<int>(g_words.size()))
                             ? g_words[g_gindex] : std::string();
    if (want.empty()) { EndGuess(); return; }
    if (g_gwait > 0) { --g_gwait; return; }

    if (g_gstep > 0) {               // the last placing has had time to land
        const std::string sel = game::GetHackingSelection();
        const std::string hl  = game::GetHackingHighlightedWord();
        F3A_INFO("Hacking guess: want %s, try %d, selection '%s', highlight '%s'",
                 want.c_str(), g_gstep, sel.c_str(), hl.c_str());
        if (sel == want || hl == want) {
            poll::RequestHackGuess();
            g_hl = want;             // on it already; not read out again
            EndGuess();
            return;
        }
    }
    // The middle letter, then a letter to the right, to the left, two right...
    if (g_gstep > static_cast<int>(want.size()) + 1) {
        EndGuess();
        tolk::Speak(strings::T("Nie mogę trafić w słowo.", "Can't reach the word."),
                    tolk::Priority::System, true);
        return;
    }
    int k = (g_gstep + 1) / 2;
    if (g_gstep % 2 == 0) k = -k;
    poll::RequestHackCursorTo(g_gtx + k * g_gcw, g_gty);
    ++g_gstep;
    g_gwait = kSettleTicks;
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
            tolk::Speak(s.empty() ? std::string(strings::T("Terminal zablokowany.", "Terminal locked."))
                                  : s,
                        tolk::Priority::Ui, true);
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
        char buf[256];
        std::snprintf(buf, sizeof(buf), strings::T(
            "Hakowanie. %s. %d słów po %d liter. "
            "Góra, dół wybiera słowo, Enter zgaduje, lewo, prawo czyta ekran.",
            "Hacking. %s. %d words, %d letters each. "
            "Up and down choose a word, Enter guesses, left and right read the screen."),
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
        tolk::Speak(Attempts(h.attempts) + strings::T(" zostało", " left"),
                    tolk::Priority::Ui, false);
    }

    // The engine tells us which word the cursor is on, so the player can also
    // sweep the grid with their own mouse and hear what they're pointing at.
    if (!h.highlighted.empty()) {
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
        std::snprintf(idx, sizeof(idx), strings::T(", %d z %d", ", %d of %d"), g_index + 1, n);
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
        float tx, ty, cw = 0.0f;
        if (game::GetHackWordTarget(g_index, &tx, &ty, &cw)) {
            g_gtx = tx; g_gty = ty;
            g_gcw = cw > 1.0f ? cw : kHackCharWidth;
            g_gindex = g_index; g_guessing = true;
            g_gstep = 0; g_gwait = 0;
            tolk::Speak(strings::T("Zgaduję: ", "Guessing: ") + g_words[g_index],
                        tolk::Priority::Ui, true);
        }
    } else if (enter) {
        tolk::Speak(strings::T("Najpierw wybierz słowo strzałkami góra, dół.",
                               "First choose a word with up and down."),
                    tolk::Priority::Ui, true);
    }
}

void Init() { F3A_INFO("Hacking module ready."); }
void Shutdown() {}

} // namespace f3a::modules::hacking
