#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <string>

namespace f3a::modules::repair {
namespace {

// Repair (your own, with a spare item) and a vendor's repair services. The
// list rows are read by the keyboard selection reader; here, what the screen
// puts beside them: the item's condition and stat now and after the repair,
// the gain, the cost, a refusal - said once it settles after the row, as a
// sighted player reads it - and on opening the skill (and caps) in the header.
std::string g_last;
std::string g_pending;
int         g_settle = 0;
bool        g_opened = false;

void OnOpen()  { g_last.clear(); g_pending.clear(); g_settle = 0; g_opened = false; }
void OnClose() { OnOpen(); }

void OnTick(float)
{
    if (!g_opened) {
        std::string head = game::GetRepairHeader();
        if (head.empty()) return;
        g_opened = true;
        tolk::Speak(head, tolk::Priority::Ui, false);
        return;
    }
    std::string d = game::GetRepairDetail();
    if (d == g_last) { g_pending = d; g_settle = 0; return; }
    if (d != g_pending) { g_pending = d; g_settle = 4; return; }   // ~0.1 s
    if (g_settle > 0 && --g_settle > 0) return;
    g_last = d;
    if (!d.empty()) tolk::Speak(d, tolk::Priority::Ui, false);    // after the row
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Repair, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::RepairServices, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Repair module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::repair
