#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <windows.h>
#include <string>

namespace f3a::modules::barter {
namespace {

// Barter. Its two columns, the arrows and Enter are the container module's
// (the menus are built alike). Here: who the trade is with, and the deal - the
// screen shows the caps either side and, in the middle, how many caps the
// offer moves and which way; a sighted player watches that number while
// picking items. So it is said whenever it changes, and H (here, not the
// world status) says both sides' caps with it.
std::string g_deal;          // the deal last said ("" = none)
bool        g_opened = false;
int         g_settle = 0;    // ticks a new deal has to hold before it is said
std::string g_pending;

std::string DealText(const game::BarterInfo& b)
{
    if (b.amount.empty() || b.direction == 0) return {};
    return (b.direction > 0 ? strings::T("Dostajesz ", "You get ")
                            : strings::T("Płacisz ", "You pay ")) +
           b.amount + strings::T(" kapsli", " caps");
}

void OnOpen()
{
    g_deal.clear();
    g_pending.clear();
    g_opened = false;
    g_settle = 0;
}

void OnClose() { OnOpen(); }

void OnTick(float)
{
    game::BarterInfo b;
    if (!game::GetBarterState(&b)) return;
    if (!g_opened) {
        g_opened = true;
        std::string s = b.vendor.empty() ? std::string(strings::T("Handel", "Barter"))
                                         : strings::T("Handel: ", "Barter: ") + b.vendor;
        if (!b.player_caps.empty()) s += strings::T(". Twoje kapsle: ", ". Your caps: ") + b.player_caps;
        if (!b.vendor_caps.empty()) s += strings::T(", handlarza: ", ", the vendor's: ") + b.vendor_caps;
        s += strings::T(". Lewo, prawo zmienia kategorię, Shift ze strzałką albo [ zmienia stronę, "
                        "Enter dodaje do transakcji.",
                        ". Left and right change the category, Shift with an arrow or [ switches sides, "
                        "Enter adds to the deal.");
        F3A_INFO("Barter with '%s' (caps %s / %s)", b.vendor.c_str(),
                 b.player_caps.c_str(), b.vendor_caps.c_str());
        tolk::Speak(s, tolk::Priority::Ui, true);
        return;
    }
    // The amount is rewritten as items go in; say it once it has held a moment.
    std::string deal = DealText(b);
    if (deal == g_deal) { g_pending = deal; g_settle = 0; return; }
    if (deal != g_pending) { g_pending = deal; g_settle = 4; return; }
    if (g_settle > 0 && --g_settle > 0) return;
    g_deal = deal;
    tolk::Speak(deal.empty() ? std::string(strings::T("Bez transakcji.", "No deal."))
                             : deal,
                tolk::Priority::Ui, false);
}

} // namespace

// H in barter: both sides' caps and the deal.
bool SpeakSummary()
{
    if (menu::ActiveMenu() != menu::Id::Barter) return false;
    game::BarterInfo b;
    if (!game::GetBarterState(&b)) return false;
    std::string s = strings::T("Twoje kapsle: ", "Your caps: ") + b.player_caps +
                    strings::T(", handlarza: ", ", the vendor's: ") + b.vendor_caps;
    std::string deal = DealText(b);
    s += ". " + (deal.empty() ? std::string(strings::T("Bez transakcji", "No deal")) : deal);
    tolk::Speak(s, tolk::Priority::Ui, true);
    return true;
}

void Init()
{
    // Registered after the container module: both get the ticks for Barter,
    // the dispatcher gives them to the topmost handler - so this is chained
    // from there (see container.cpp), not registered for Barter itself.
    F3A_INFO("Barter module ready.");
}
void Shutdown() {}

void Tick() { OnTick(0.0f); }
void Open() { OnOpen(); }
void Close() { OnClose(); }

} // namespace f3a::modules::barter
