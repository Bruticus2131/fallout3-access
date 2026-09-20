#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <string>

namespace f3a::modules::message {
namespace {

// Read the full popup text (title + body + buttons), not just the menu name.
// The text tiles are sometimes populated a tick or two after the menu opens,
// so we keep trying for a short window until we get something, then latch.
bool        g_spoken = false;
std::string g_last;
int         g_retry  = 0;
constexpr int kRetryTicks = 12;   // ~1s at the poll rate

void TryAnnounce()
{
    std::string text = game::GetActiveMessageText();
    if (text.empty()) {
        // Fall back to whatever the focused tile says (covers Book pages and
        // any popup that isn't a Message menu).
        auto sel = game::GetActiveMenuSelectionText();
        if (sel) text = *sel;
    }
    if (text.empty() || text == g_last) return;
    g_last   = text;
    g_spoken = true;
    tolk::Speak(text, tolk::Priority::System, true);
}

void OnOpen()
{
    g_spoken = false;
    g_last.clear();
    g_retry = kRetryTicks;
    TryAnnounce();
}

void OnClose()
{
    g_spoken = false;
    g_last.clear();
    g_retry = 0;
}

void OnTick(float)
{
    if (g_spoken || g_retry <= 0) return;
    --g_retry;
    TryAnnounce();
}

// ---- Text-entry popup (character name, etc.) -------------------------------
//
// TextEditMenu: announce the prompt on open, then echo what's typed. We poll the
// `textedit_text` tile each tick (it's the only signal — the game owns the F/key
// input) and speak the delta: appended chars as they come (queued so fast typing
// isn't cut), the whole field on backspace/edit. Lets a blind player hear the
// name they're entering.
std::string g_te_last;
bool        g_te_open = false;

void OnTextEditOpen()
{
    g_te_open = true;
    auto txt  = game::GetTextEditText();
    g_te_last = txt ? *txt : std::string();
    // Minimal: no "enter name" prompt (the user doesn't want it). Just read any
    // existing text; an empty field opens silently and the letter-echo takes over.
    if (!g_te_last.empty())
        tolk::Speak(g_te_last, tolk::Priority::Ui, true);
}

void OnTextEditClose() { g_te_open = false; g_te_last.clear(); }

void OnTextEditTick(float)
{
    if (!g_te_open) return;
    auto txt = game::GetTextEditText();
    if (!txt) return;                       // no reliable read this tick
    const std::string cur = *txt;
    if (cur == g_te_last) return;           // unchanged (also swallows caret blink)
    g_te_last = cur;
    // Read the WHOLE text so far on every change → "z, zu, zuz, zuzi, zuzia".
    // interrupt=true so the latest state wins (no piled-up fragments); an empty
    // field stays silent (no "puste" spam).
    if (!cur.empty())
        tolk::Speak(cur, tolk::Priority::Ui, true);
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Message, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::LevelUp,
        []() {
            tolk::Speak(strings::Render(strings::Key::LevelUp),
                        tolk::Priority::System, true);
        });
    menu::RegisterMenu(menu::Id::Book, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::TextEdit, &OnTextEditOpen, &OnTextEditClose,
                       &OnTextEditTick);
    // Terminal text is polled directly in polling_loop (PollTerminal), not via
    // menu dispatch — the terminal isn't reliably detected as a menu "open".
    F3A_INFO("Message module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::message
