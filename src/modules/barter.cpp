#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <cstdio>
#include <string>

namespace f3a::modules::barter {
namespace {

std::string g_last_selection;
int         g_last_net = INT32_MIN;

void AnnounceSelection()
{
    auto bs = game::GetBarterState();
    if (!bs.selected) return;

    char vbuf[16];  std::snprintf(vbuf, sizeof(vbuf), "%d", bs.selected->value_caps);
    char wbuf[16];  std::snprintf(wbuf, sizeof(wbuf), "%.1f", bs.selected->weight);

    char extra[96];
    std::snprintf(extra, sizeof(extra), "%s, %s",
                  strings::RenderArgs(strings::Key::ItemWeightFmt, wbuf).c_str(),
                  strings::RenderArgs(strings::Key::ItemValueFmt, vbuf).c_str());

    std::string line = strings::RenderArgs(strings::Key::SelectionFmt,
                                           bs.selected->name.c_str(), extra);
    if (line == g_last_selection) return;
    g_last_selection = line;
    tolk::Speak(line, tolk::Priority::Ui, true);
}

void AnnounceTotal()
{
    auto bs = game::GetBarterState();
    int net = bs.vendor_total_value - bs.player_total_value;
    if (net == g_last_net) return;
    g_last_net = net;

    char a[16], b[16], c[16];
    std::snprintf(a, sizeof(a), "%d", bs.player_total_value);
    std::snprintf(b, sizeof(b), "%d", bs.vendor_total_value);
    std::snprintf(c, sizeof(c), "%+d", net);

    tolk::Speak(
        strings::RenderArgs(strings::Key::BarterTotalFmt, a, b, c),
        tolk::Priority::Ui, false);

    const auto& cfg = config::Get();
    if (net < 0 && -net >= cfg.barter_warn_loss_caps) {
        char loss[16]; std::snprintf(loss, sizeof(loss), "%d", -net);
        tolk::Speak(strings::RenderArgs(strings::Key::BarterLossWarn, loss),
                    tolk::Priority::System, false);
    }
}

void OnOpen()  { g_last_selection.clear(); g_last_net = INT32_MIN; }
void OnClose() { g_last_selection.clear(); g_last_net = INT32_MIN; }
void OnTick(float) { AnnounceSelection(); AnnounceTotal(); }

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Barter, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Repair, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::RepairServices, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Barter module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::barter
