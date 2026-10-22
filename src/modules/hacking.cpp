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
#include <cstdlib>
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
std::vector<std::string> g_words;   // the candidate passwords
std::vector<std::string> g_list;    // what Up/Down browse: the words, then bracket pairs
size_t      g_nwords  = 0;          // how many of g_list are words
int         g_index   = -1;
// The words the answers so far leave possible: each wrong guess says how many
// letters were right in place ("Likeness=2"), and only a word with that many
// letters in common with the guess can still be the password.
std::vector<std::string> g_possible;
std::string g_last_guess;           // the word just entered, waiting for its answer
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
    static const char* const kPl[] = { "próba", "próby", "prób" };
    static const char* const kRu[] = { "попытка", "попытки", "попыток" };
    return strings::Counted(n, kPl, kRu, "attempt", "attempts");
}
void EndGuess() { g_guessing = false; g_gindex = -1; g_gstep = 0; g_gwait = 0; }

void Reset()
{
    g_key.clear(); g_pending.clear(); g_words.clear(); g_lines.clear();
    g_list.clear(); g_nwords = 0; g_possible.clear(); g_last_guess.clear();
    g_spoken_log.clear(); g_hl.clear();
    g_index = -1; g_line_index = -1; g_attempts = 0;
    g_log_count = 0; g_log_pending = 0; g_locked_said = false;
    EndGuess();
}

// Read newly-appended log entries once the count has settled.
// Letters in the same place.
int Likeness(const std::string& a, const std::string& b)
{
    int n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (a[i] == b[i]) ++n;
    return n;
}

// After a wrong guess the log says how many letters were right ("Likeness=2",
// in the game's own language): narrow the possible words down by it and say
// what is left - the deduction a sighted player does with pen and paper.
std::string LikenessHint(const std::string& answer)
{
    if (g_last_guess.empty()) return {};
    const std::string guess = g_last_guess;
    // The number after '=' or before '/' ("2/5"): the first digits there.
    int likeness = -1;
    for (size_t i = 0; i < answer.size(); ++i) {
        if (answer[i] < '0' || answer[i] > '9') continue;
        size_t j = i;
        while (j < answer.size() && answer[j] >= '0' && answer[j] <= '9') ++j;
        const bool marked = (i > 0 && answer[i - 1] == '=') || (j < answer.size() && answer[j] == '/') ||
                            (i > 1 && answer[i - 2] == '=' && answer[i - 1] == ' ');
        if (marked) { likeness = std::atoi(answer.substr(i, j - i).c_str()); break; }
        i = j;
    }
    if (likeness < 0) return {};
    g_last_guess.clear();
    std::vector<std::string> keep;
    for (const auto& w : g_possible)
        if (w != guess && Likeness(w, guess) == likeness) keep.push_back(w);
    g_possible.swap(keep);
    if (g_possible.empty()) return {};
    if (g_possible.size() == 1)
        return strings::T(". Hasło to ", ". The password is ") + g_possible.front();
    std::string s = strings::T(". Pasują jeszcze: ", ". Still possible: ");
    for (size_t i = 0; i < g_possible.size(); ++i) s += (i ? ", " : "") + g_possible[i];
    return s;
}

// The browse list from the screen: the words, then the bracket pairs.
void RefreshList(const game::HackingInfo& h)
{
    std::vector<std::string> list = h.words;
    list.insert(list.end(), h.brackets.begin(), h.brackets.end());
    if (list == g_list) return;
    g_list.swap(list);
    g_nwords = h.words.size();
    if (g_index >= static_cast<int>(g_list.size())) g_index = static_cast<int>(g_list.size()) - 1;
}

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
        if (!add.empty()) add += LikenessHint(add);
        if (!add.empty()) tolk::Speak(add, tolk::Priority::Ui, false);
    } else if (static_cast<int>(log.size()) > g_log_count) {
        g_log_pending = static_cast<int>(log.size());
    }
}

// Put the game's cursor on the word being guessed and enter it once the game
// highlights it. One step per call; done when entered or given up.
void TickGuess()
{
    const std::string want = (g_gindex >= 0 && g_gindex < static_cast<int>(g_list.size()))
                             ? g_list[g_gindex] : std::string();
    const bool bracket = g_gindex >= static_cast<int>(g_nwords);
    if (want.empty()) { EndGuess(); return; }
    if (g_gwait > 0) { --g_gwait; return; }

    if (g_gstep > 0) {               // the last placing has had time to land
        const std::string sel = game::GetHackingSelection();
        const std::string hl  = game::GetHackingHighlightedWord();
        F3A_INFO("Hacking guess: want %s, try %d, selection '%s', highlight '%s'",
                 want.c_str(), g_gstep, sel.c_str(), hl.c_str());
        if (sel == want || (!bracket && hl == want)) {
            poll::RequestHackGuess();
            if (!bracket) { g_hl = want; g_last_guess = want; }   // its answer narrows the rest
            EndGuess();
            return;
        }
    }
    // The middle letter, then a letter to the right, to the left, two right...
    // A pair is taken on its opening bracket only: a letter either way at most.
    if (g_gstep > (bracket ? 2 : static_cast<int>(want.size()) + 1)) {
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
        // A bracket pair took a wrong word out: the same grid, one word fewer.
        // Said as that, not as a new terminal.
        bool removed = !g_words.empty() && h.words.size() < g_words.size();
        for (const auto& w : h.words)
            if (std::find(g_words.begin(), g_words.end(), w) == g_words.end()) removed = false;
        if (removed) {
            std::string gone;
            for (const auto& w : g_words)
                if (std::find(h.words.begin(), h.words.end(), w) == h.words.end()) {
                    gone += (gone.empty() ? "" : ", ") + w;
                    g_possible.erase(std::remove(g_possible.begin(), g_possible.end(), w),
                                     g_possible.end());
                }
            g_key = key; g_words = h.words;
            RefreshList(h);
            tolk::Speak(strings::T("Usunięto: ", "Removed: ") + gone, tolk::Priority::Ui, false);
            return;
        }
        g_key = key; g_words = h.words; g_index = -1;
        g_possible = h.words; g_last_guess.clear();
        g_list.clear(); RefreshList(h);
        g_attempts = h.attempts;
        int L = g_words.empty() ? 0 : static_cast<int>(g_words.front().size());
        char buf[256];
        std::snprintf(buf, sizeof(buf), strings::T(
            "Hakowanie. %s. %d słów po %d liter, par nawiasów: %d. "
            "Góra, dół wybiera słowo, Enter zgaduje, lewo, prawo czyta ekran.",
            "Hacking. %s. %d words, %d letters each, bracket pairs: %d. "
            "Up and down choose a word, Enter guesses, left and right read the screen."),
            Attempts(h.attempts).c_str(), static_cast<int>(g_words.size()), L,
            static_cast<int>(h.brackets.size()));
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
    RefreshList(h);                            // a used bracket pair is gone

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

    if ((up || down) && !g_list.empty()) {
        int n = static_cast<int>(g_list.size());
        if (g_index < 0) g_index = up ? n - 1 : 0;
        else             g_index = (g_index + (up ? -1 : 1) + n) % n;
        char idx[24];
        std::snprintf(idx, sizeof(idx), strings::T(", %d z %d", ", %d of %d"), g_index + 1, n);
        const std::string& it = g_list[g_index];
        if (g_index >= static_cast<int>(g_nwords)) {
            tolk::Speak(strings::T("Para nawiasów: ", "Bracket pair: ") + Spell(it) + idx,
                        tolk::Priority::Ui, true);
        } else {
            const bool ruled_out = !g_possible.empty() &&
                std::find(g_possible.begin(), g_possible.end(), it) == g_possible.end();
            tolk::Speak(it + ", " + Spell(it) +
                            (ruled_out ? strings::T(", odpada", ", ruled out") : "") + idx,
                        tolk::Priority::Ui, true);
        }
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
    if (enter && g_index >= 0 && g_index < static_cast<int>(g_list.size())) {
        float tx, ty, cw = 0.0f;
        if (game::GetHackWordTarget(g_index, &tx, &ty, &cw)) {
            g_gtx = tx; g_gty = ty;
            g_gcw = cw > 1.0f ? cw : kHackCharWidth;
            g_gindex = g_index; g_guessing = true;
            g_gstep = 0; g_gwait = 0;
            tolk::Speak((g_index >= static_cast<int>(g_nwords)
                             ? strings::T("Nawiasy: ", "Brackets: ")
                             : strings::T("Zgaduję: ", "Guessing: ")) + g_list[g_index],
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
