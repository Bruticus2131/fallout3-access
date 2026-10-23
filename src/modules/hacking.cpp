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
//   * read the lines the terminal types out before the board, as each shows
//     in full; Enter then finishes the typing at once, as a click does,
//   * read the candidate words; UP/DOWN browse them (+ spelled),
//   * ENTER guesses the chosen word: the menu's own highlight code is handed
//     the word's row and letter, and only once the game's highlight holds
//     that word - the text a click enters - is the click made, through the
//     menu itself (game::EnterHackingCandidate). Nothing is entered that the
//     game does not confirm. (Steering the game's cursor onto the word never
//     got it highlighted in play.)
//   * LEFT/RIGHT browse every readable line of the screen (header + attempt log),
//   * read the attempt log live (the response after a guess), entry by entry.
// While a tutorial popup is over the screen, its keys are its own.
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

// Attempt-log reading: the log as last said, and as last read (it has to hold
// still for a poll - rows land across several frames).
std::vector<std::string> g_spoken_log;
std::vector<std::string> g_log_seen;

// A guess just asked for: Enter again waits this long, so one press held a
// moment, or pressed twice, can't spend two attempts.
DWORD g_enter_quiet_until = 0;

std::string g_hl;              // last announced highlighted word

// The typed-out lines before the board, said once each (by slot).
std::vector<std::string> g_intro_said;
bool g_intro_seen = false;     // the screen has been open (to reset on close)

// When a tutorial popup was last over the screen.
DWORD g_tutorial_at = 0;

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
void Reset()
{
    g_key.clear(); g_pending.clear(); g_words.clear(); g_lines.clear();
    g_list.clear(); g_nwords = 0; g_possible.clear(); g_last_guess.clear();
    g_spoken_log.clear(); g_hl.clear();
    g_index = -1; g_line_index = -1; g_attempts = 0;
    g_log_seen.clear(); g_locked_said = false;
    g_enter_quiet_until = 0;
}

// A typed-out line worth saying: not a row of the memory dump ("0xF4E0 ..."),
// not the bare ">" prompt, with a letter in it. The prompt in front is dropped.
std::string IntroLine(const std::string& raw)
{
    size_t b = raw.find_first_not_of("> \t");
    if (b == std::string::npos) return {};
    std::string s = raw.substr(b);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return {};
    for (unsigned char c : s)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80) return s;
    return {};
}

// Before the board: each line the terminal types out, once it shows in full,
// in order. One line a tick, so a burst (the typing finished at once) is
// queued in order rather than run together.
void ReadIntro(const std::vector<std::pair<int, std::string>>& lines)
{
    for (const auto& l : lines) {
        if (l.first < 0) continue;
        if (g_intro_said.size() <= static_cast<size_t>(l.first)) g_intro_said.resize(l.first + 1);
        if (g_intro_said[l.first] == l.second) continue;
        g_intro_said[l.first] = l.second;
        const std::string s = IntroLine(l.second);
        if (s.empty()) continue;
        tolk::Speak(s, tolk::Priority::Ui, false);
        return;
    }
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

// What the log adds, oldest first: the entries of `cur` after the longest run
// that ends `old` and begins `cur`. The log keeps only so many rows: once it
// is full the oldest drop off the top as new ones come in, and their COUNT
// stays the same - the second and third answers went unsaid that way.
std::vector<std::string> LogAdded(const std::vector<std::string>& old,
                                  const std::vector<std::string>& cur)
{
    for (size_t n = (std::min)(old.size(), cur.size()); n > 0; --n)
        if (std::equal(old.end() - n, old.end(), cur.begin()))
            return std::vector<std::string>(cur.begin() + n, cur.end());
    return cur;
}

// A log row as said: the ">" the terminal prints in front dropped.
std::string LogText(const std::string& e)
{
    size_t b = e.find_first_not_of("> \t");
    return b == std::string::npos ? std::string() : e.substr(b);
}

void PollLog()
{
    // Rows land across several frames: read once the log has held still for a
    // poll, then say what it gained since it was last said.
    std::vector<std::string> log = game::GetHackingLogEntries();
    if (log != g_log_seen) { g_log_seen = log; return; }
    if (log == g_spoken_log) return;
    std::string add;
    for (const auto& e : LogAdded(g_spoken_log, log)) {
        const std::string t = LogText(e);
        if (t.empty()) continue;
        if (!add.empty()) add += ", ";
        add += t;
    }
    g_spoken_log = log;
    if (!add.empty()) add += LikenessHint(add);
    if (!add.empty()) tolk::Speak(add, tolk::Priority::Ui, false);
}

// A tutorial popup is over the screen - or was a moment ago: the Enter that
// closes it is still held when the screen under it is read again, and must
// not also guess or skip.
bool TutorialUp()
{
    if (game::IsTutorialOpen()) { g_tutorial_at = GetTickCount(); return true; }
    return GetTickCount() - g_tutorial_at < 400;
}

} // namespace

void Tick(float)
{
    // Before the board: the lines being typed out. Enter finishes the typing.
    int stage = -1;
    const auto intro = game::GetHackingIntro(&stage);
    if (stage < 0) {
        if (g_intro_seen) { g_intro_seen = false; g_intro_said.clear(); }
    } else {
        if (!g_intro_seen) SeedKeyEdges();   // a key still held from before: not a press
        g_intro_seen = true;
        if (stage <= 1) {
            const bool enter = KeyEdge(VK_RETURN, &g_enter);
            if (game::IsPauseMenuOpen() || TutorialUp()) return;
            ReadIntro(intro);
            if (enter) poll::RequestHackSkipIntro();
            return;
        }
    }

    auto h = game::GetHackingInfo();
    if (!h.active) {
        if (!g_key.empty() || g_locked_said) Reset();
        return;
    }
    if (game::IsPauseMenuOpen()) return;   // pause menu over hacking — ignore input
    // A tutorial popup over the board: the keys are its own (Enter closes it,
    // and must not also guess).
    if (TutorialUp()) { SeedKeyEdges(); return; }

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
        tolk::Speak(buf, tolk::Priority::Ui, false);   // after the typed-out lines
        SeedKeyEdges();
        g_spoken_log = game::GetHackingLogEntries();   // don't replay an old log
        g_log_seen = g_spoken_log;
        g_line_index = -1;
        g_hl.clear();
        return;
    }

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
    // Right after a guess the highlight is the guess's own, then the mouse's
    // again: not news.
    if (GetTickCount() < g_enter_quiet_until) {
        if (!h.highlighted.empty()) g_hl = h.highlighted;
    } else if (!h.highlighted.empty()) {
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
        // The header lines, then the log in the order it was written.
        const auto log = game::GetHackingLogEntries();
        g_lines.clear();
        for (const auto& l : game::GetHackingTextLines())
            if (std::find(log.begin(), log.end(), l) == log.end()) g_lines.push_back(l);
        for (const auto& e : log)
            if (!LogText(e).empty()) g_lines.push_back(LogText(e));
        if (!g_lines.empty()) {
            int n = static_cast<int>(g_lines.size());
            if (g_line_index < 0) g_line_index = right ? 0 : n - 1;
            else                  g_line_index = (g_line_index + (right ? 1 : -1) + n) % n;
            tolk::Speak(g_lines[g_line_index], tolk::Priority::Ui, true);
        }
    }
    if (enter && GetTickCount() < g_enter_quiet_until) {
        // the last guess is still being entered and answered
    } else if (enter && g_index >= 0 && g_index < static_cast<int>(g_list.size())) {
        const std::string& want = g_list[g_index];
        const bool bracket = g_index >= static_cast<int>(g_nwords);
        g_enter_quiet_until = GetTickCount() + 1200;
        poll::RequestHackEnter(g_index, want);
        // The game then highlights it: not said a second time. A word's
        // answer narrows the rest down.
        if (!bracket) { g_hl = want; g_last_guess = want; }
        tolk::Speak((bracket ? strings::T("Nawiasy: ", "Brackets: ")
                             : strings::T("Zgaduję: ", "Guessing: ")) + want,
                    tolk::Priority::Ui, true);
    } else if (enter) {
        tolk::Speak(strings::T("Najpierw wybierz słowo strzałkami góra, dół.",
                               "First choose a word with up and down."),
                    tolk::Priority::Ui, true);
    }
}

void Init() { F3A_INFO("Hacking module ready."); }
void Shutdown() {}

} // namespace f3a::modules::hacking
