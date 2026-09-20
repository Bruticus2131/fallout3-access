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
const void* g_last_box = nullptr;   // listbox of the last announcement

void OnTick(float)
{
    // Keyboard path first: it tracks the highlight across BOTH columns, so
    // switching sides with Left/Right (which never moves the mouse activeTile)
    // gets announced. We re-speak when the row text changes OR the focused
    // column (listbox) changes — the latter is exactly the Left/Right switch.
    auto sel = game::GetKeyboardSelection();
    if (sel && (!sel->label.empty() || !sel->value.empty())) {
        std::string text = sel->label;
        if (!sel->value.empty())
            text += (text.empty() ? "" : ", ") + sel->value;
        bool box_changed = sel->container != g_last_box;
        if (text == g_last && !box_changed) return;
        g_last     = text;
        g_last_box = sel->container;
        tolk::Speak(text, tolk::Priority::Ui, true);
        return;
    }

    // Fallback: the mouse/activeTile label (unchanged old behavior).
    auto m = game::GetActiveMenuSelectionText();
    if (!m || m->empty() || *m == g_last) return;
    g_last     = *m;
    g_last_box = nullptr;
    tolk::Speak(*m, tolk::Priority::Ui, true);
}

void OnOpen()  { g_last.clear(); g_last_box = nullptr; OnTick(0); }
void OnClose() { g_last.clear(); g_last_box = nullptr; }

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
