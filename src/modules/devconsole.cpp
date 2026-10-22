#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace f3a::modules::devconsole {
namespace {

// The console (~). A sighted player sees the line being typed with its caret
// and the lines the commands print; here that is said: the console opening
// and closing, each character typed or deleted, where the caret moved to, a
// line brought back from the history (Up / Down), what a command printed
// after Enter, and the page PageUp / PageDown scrolls into view. The game
// keeps its own keys - this only listens. Run from the main thread: the line
// being typed is read through the engine's own text lookup.

bool        g_open      = false;
const void* g_cursor    = nullptr;   // the last printed line seen
bool        g_have_line = false;
std::string g_line;                  // game code page, caret taken out
int         g_caret     = 0;
int         g_bottom    = -1;        // the view's bottom line
bool        g_hinted    = false;     // the keys said once a session
DWORD       g_last_tick = 0;
// PageUp / PageDown with nothing moving a moment later: the top or the end.
int         g_page_key  = 0;
DWORD       g_page_at   = 0;
int         g_page_from = -1;
bool        g_prior_was = false, g_next_was = false;
bool        g_enter_was = false;
DWORD       g_enter_at  = 0;         // Enter last pressed

// F11: the last state, for the dump (written on another thread).
std::mutex               g_dump_mx;
std::vector<std::string> g_dump;
DWORD                    g_dump_at = 0;

void Say(const std::string& s, bool interrupt = true)
{
    if (!s.empty()) tolk::Speak(s, tolk::Priority::Ui, interrupt);
}

std::string Text(const std::string& raw) { return game::GameTextToUtf8(raw); }

// A character as a screen reader names it when you type it.
std::string CharName(unsigned char c)
{
    switch (c) {
    case ' ':  return strings::T("spacja", "space");
    case '.':  return strings::T("kropka", "dot");
    case ',':  return strings::T("przecinek", "comma");
    case ';':  return strings::T("średnik", "semicolon");
    case ':':  return strings::T("dwukropek", "colon");
    case '"':  return strings::T("cudzysłów", "quote");
    case '\'': return strings::T("apostrof", "apostrophe");
    case '(':  return strings::T("nawias otwierający", "left paren");
    case ')':  return strings::T("nawias zamykający", "right paren");
    case '[':  return strings::T("nawias kwadratowy otwierający", "left bracket");
    case ']':  return strings::T("nawias kwadratowy zamykający", "right bracket");
    case '{':  return strings::T("klamra otwierająca", "left brace");
    case '}':  return strings::T("klamra zamykająca", "right brace");
    case '_':  return strings::T("podkreślnik", "underscore");
    case '-':  return strings::T("minus", "dash");
    case '+':  return strings::T("plus", "plus");
    case '=':  return strings::T("równa się", "equals");
    case '*':  return strings::T("gwiazdka", "star");
    case '/':  return strings::T("ukośnik", "slash");
    case '\\': return strings::T("ukośnik wsteczny", "backslash");
    case '!':  return strings::T("wykrzyknik", "exclamation");
    case '?':  return strings::T("pytajnik", "question");
    case '#':  return strings::T("krzyżyk", "hash");
    case '&':  return strings::T("ampersand", "ampersand");
    case '%':  return strings::T("procent", "percent");
    case '<':  return strings::T("mniejsze", "less");
    case '>':  return strings::T("większe", "greater");
    case '~':  return strings::T("tylda", "tilde");
    case '`':  return strings::T("grawis", "grave");
    case '@':  return strings::T("małpa", "at");
    case '$':  return strings::T("dolar", "dollar");
    case '^':  return strings::T("daszek", "caret");
    case '|':  return strings::T("kreska pionowa", "bar");
    default:   return Text(std::string(1, static_cast<char>(c)));
    }
}

std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& e : v) {
        if (e.empty()) continue;
        if (!s.empty()) s += ' ';
        s += e;
    }
    return s;
}

std::string NoSpaces(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
}

// What changed in the line being typed, as a screen reader says it in an
// edit box.
void DescribeEdit(const std::string& was, int wc, const std::string& now, int nc)
{
    if (now == was) {                                         // the caret moved
        Say(nc < static_cast<int>(now.size()) ? CharName(now[nc])
                                              : strings::T("koniec", "end"));
        return;
    }
    const int dw = static_cast<int>(now.size()) - static_cast<int>(was.size());
    if (dw > 0 && wc >= 0 && wc <= static_cast<int>(was.size()) && nc == wc + dw &&
        now.compare(0, wc, was, 0, wc) == 0 &&
        now.compare(nc, std::string::npos, was, wc, std::string::npos) == 0) {
        const std::string typed = now.substr(wc, dw);
        Say(dw == 1 ? CharName(typed[0]) : Text(typed));
        return;
    }
    if (dw == -1) {
        // Backspace takes the one before the caret, Delete the one at it.
        if (wc >= 1 && nc == wc - 1 && now == std::string(was).erase(wc - 1, 1)) {
            Say(CharName(was[wc - 1]));
            return;
        }
        if (wc < static_cast<int>(was.size()) && nc == wc && now == std::string(was).erase(wc, 1)) {
            Say(CharName(was[wc]));
            return;
        }
    }
    // Anything else: a line from the history (Up / Down), or the line emptied.
    Say(now.empty() ? strings::T("pusto", "blank") : Text(now));
}

// Enter: the console prints the command itself first - not read back, the
// player has just typed it. Returns how many of the new lines are that echo.
size_t EchoLines(const std::vector<std::string>& fresh, const std::string& command)
{
    const std::string want = NoSpaces(Text(command));
    std::string acc;
    for (size_t i = 0; i < fresh.size(); ++i) {
        std::string l = NoSpaces(fresh[i]);
        if (i == 0 && !l.empty() && l[0] == '>') l.erase(0, 1);
        acc += l;
        if (acc == want) return i + 1;
        if (acc.size() > want.size() || want.compare(0, acc.size(), acc) != 0) break;
    }
    return 0;
}

void Snapshot(int count, int bottom, int shown)
{
    std::vector<std::string> d;
    char head[160];
    std::snprintf(head, sizeof(head), "  open=%d lines=%d bottom=%d shown=%d caret=%d",
                  g_open ? 1 : 0, count, bottom, shown, g_caret);
    d.push_back(head);
    d.push_back("  input: '" + Text(g_line) + "'");
    for (const auto& l : game::ConsoleLines(std::max(0, count - 20), count))
        d.push_back("  line: " + l);
    std::lock_guard<std::mutex> lk(g_dump_mx);
    g_dump.swap(d);
}

} // namespace

void Init() { F3A_INFO("Console module ready."); }
void Shutdown() {}

void MainThreadTick()
{
    const DWORD now = GetTickCount();
    if (now - g_last_tick < 25) return;
    g_last_tick = now;

    const bool open = game::ConsoleOpen();
    if (open != g_open) {
        g_open = open;
        g_cursor = nullptr;
        g_have_line = false;
        g_bottom = -1;
        g_page_key = 0;
        if (open) game::ConsoleNewLines(&g_cursor);   // what was there before: not news
        F3A_INFO("Console %s.", open ? "opened" : "closed");
        if (config::IsEnabled()) {
            if (open) {
                std::string s = strings::T("Konsola.", "Console.");
                if (!g_hinted) {
                    g_hinted = true;
                    s += strings::T(" Wpisz polecenie i naciśnij Enter. Góra i dół: poprzednie "
                                    "polecenia, PageUp i PageDown: przewijanie wyników.",
                                    " Type a command and press Enter. Up and Down: earlier "
                                    "commands, PageUp and PageDown: scroll the output.");
                }
                Say(s);
            } else {
                Say(strings::T("Konsola zamknięta.", "Console closed."));
            }
        }
        int c = 0, b = 0, v = 0;
        if (game::ConsoleView(&c, &b, &v)) Snapshot(c, b, v);
        return;
    }
    if (!open) return;

    // The keys whose effect is read below, before what they did is looked at.
    const bool prior = (GetAsyncKeyState(VK_PRIOR)  & 0x8000) != 0;
    const bool next  = (GetAsyncKeyState(VK_NEXT)   & 0x8000) != 0;
    const bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    if ((prior && !g_prior_was) || (next && !g_next_was)) {
        g_page_key = prior ? VK_PRIOR : VK_NEXT;
        g_page_at = now;
        g_page_from = g_bottom;
    }
    if (enter && !g_enter_was) g_enter_at = now;
    g_prior_was = prior;
    g_next_was = next;
    g_enter_was = enter;

    std::string line;
    int caret = 0;
    const bool have = game::GetConsoleInput(&line, &caret);
    std::vector<std::string> fresh = game::ConsoleNewLines(&g_cursor);

    // The line emptied as lines came out, or right after Enter: a command was
    // entered (Down past the newest command empties it too, quietly).
    const bool entered = have && g_have_line && line.empty() && !g_line.empty() &&
                         (!fresh.empty() || now - g_enter_at < 500);
    if (entered) {
        const size_t echo = EchoLines(fresh, g_line);
        fresh.erase(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(echo));
    }
    const bool speak = config::IsEnabled();
    if (have) {
        if (g_have_line && !entered && (line != g_line || caret != g_caret) && speak)
            DescribeEdit(g_line, g_caret, line, caret);
        g_have_line = true;
        g_line  = line;
        g_caret = caret;
    }
    if (speak) {
        if (!fresh.empty())  Say(Join(fresh), entered);
        else if (entered)    Say(strings::T("Wykonano.", "Done."));
    }

    // PageUp / PageDown: the console scrolls itself; the page that came into
    // view is read top to bottom.
    int count = 0, bottom = 0, shown = 0;
    if (game::ConsoleView(&count, &bottom, &shown)) {
        if (g_page_key && g_bottom >= 0 && bottom != g_bottom && bottom > 0 &&
            fresh.empty() && !entered) {
            const int from = std::max(0, bottom - shown);
            char head[96];
            std::snprintf(head, sizeof(head),
                          strings::T("Wiersze %d do %d z %d. ", "Lines %d to %d of %d. "),
                          from + 1, bottom, count);
            if (speak) Say(head + Join(game::ConsoleLines(from, bottom)));
            g_page_key = 0;
        } else if (g_page_key && now - g_page_at > 300) {
            if (bottom == g_page_from && speak)
                Say(g_page_key == VK_PRIOR ? strings::T("Początek.", "Top.")
                                           : strings::T("Koniec.", "Bottom."));
            g_page_key = 0;
        }
        g_bottom = bottom;
        if (now - g_dump_at > 500 || !fresh.empty()) {
            g_dump_at = now;
            Snapshot(count, bottom, shown);
        }
    }
}

std::vector<std::string> DumpLines()
{
    std::vector<std::string> d;
    d.push_back("===== Console =====");
    {
        std::lock_guard<std::mutex> lk(g_dump_mx);
        d.insert(d.end(), g_dump.begin(), g_dump.end());
    }
    if (d.size() == 1) d.push_back("  (not opened this session)");
    return d;
}

} // namespace f3a::modules::devconsole
