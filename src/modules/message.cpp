#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

namespace f3a::modules::message {
namespace {

void OnOpen()
{
    auto sel = game::GetActiveMenuSelectionText();
    if (sel && !sel->empty()) {
        tolk::Speak(*sel, tolk::Priority::System, true);
    }
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Message, &OnOpen);
    menu::RegisterMenu(menu::Id::LevelUp,
        []() {
            tolk::Speak(strings::Render(strings::Key::LevelUp),
                        tolk::Priority::System, true);
        });
    menu::RegisterMenu(menu::Id::Book, &OnOpen);
    F3A_INFO("Message module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::message
