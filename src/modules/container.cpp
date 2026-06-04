#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <string>

namespace f3a::modules::container {
namespace {

std::string g_last;

void OnTick(float)
{
    auto sel = game::GetActiveMenuSelectionText();
    if (!sel || sel->empty() || *sel == g_last) return;
    g_last = *sel;
    tolk::Speak(*sel, tolk::Priority::Ui, true);
}

void OnOpen()  { g_last.clear(); OnTick(0); }
void OnClose() { g_last.clear(); }

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Container, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Quantity,  &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::SleepWait, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Container module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::container
