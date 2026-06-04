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
    F3A_INFO("Message module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::message
