#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <cstdio>

namespace f3a::modules::vats {
namespace {

int g_last_idx = -2;
int g_last_queue = -1;

void AnnounceSelection()
{
    int idx = game::GetVatsSelectedIndex();
    if (idx == g_last_idx) return;
    g_last_idx = idx;

    auto targets = game::GetVatsTargets();
    if (targets.empty()) {
        tolk::Speak(strings::Render(strings::Key::VatsNoTargets),
                    tolk::Priority::Combat, true);
        return;
    }
    if (idx < 0 || idx >= (int)targets.size()) return;
    const auto& t = targets[idx];

    char pct[8], cost[8];
    std::snprintf(pct,  sizeof(pct),  "%d", t.hit_chance);
    std::snprintf(cost, sizeof(cost), "%d", t.ap_cost);

    char label[160];
    std::snprintf(label, sizeof(label), "%s, %s",
                  t.actor_name.c_str(),
                  t.body_part.c_str());

    tolk::Speak(
        strings::RenderArgs(strings::Key::VatsTargetFmt, label, pct, cost),
        tolk::Priority::Combat, true);
}

void AnnounceQueue()
{
    int q = game::GetVatsQueueLength();
    int cap = game::GetVatsQueueCapacity();
    if (q == g_last_queue) return;
    g_last_queue = q;

    if (q >= cap && cap > 0) {
        tolk::Speak(strings::Render(strings::Key::VatsQueueFull),
                    tolk::Priority::Combat, false);
    }
}

void OnOpen()
{
    g_last_idx = -2;
    g_last_queue = -1;
    if (game::GetVatsTargets().empty()) {
        tolk::Speak(strings::Render(strings::Key::VatsNoTargets),
                    tolk::Priority::Combat, true);
    }
}

void OnClose() { g_last_idx = -2; g_last_queue = -1; }
void OnTick(float) { AnnounceSelection(); AnnounceQueue(); }

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::VATS, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("VATS module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::vats
