#include "f3a/modules.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"
#include "f3a/strings.h"

#include <string>
#include <cstdio>

namespace f3a::modules::quantity {
namespace {

// The "Ile?" prompt that appears when you drop, buy, sell or move a STACK of
// items. Blind it used to be a dead end: you could start dropping seven suits
// with no idea which number was selected.
//
// We do NOT drive this menu, on purpose: clicking the stepper tiles fought the
// game's own handling of Left/Right (the cursor jumped around). Ok - ours or
// the player's A - used to crash the game on a drop; that was the engine
// losing the selected item behind the prompt, fixed in engine_hooks.cpp.
//
// The menu carries its own shortcut hints — `A)` on Ok and `E)` on Cancel — so
// the accessible answer is to READ those out and let the player press them. That
// is how the rest of the mod works: the game acts, we narrate.
//
// Tiles (from a live dump): QM_AmountChosen holds the chosen number as text,
// QM_AmountMeter is the slider, QM_OKButton / QM_CancelButton finish.
bool        g_open = false;
std::string g_last_said;

// FOSE's kMenuType_Quantity, spelled out to avoid pulling in game headers.
constexpr uint32_t kQuantityMenu = 0x3F8;

std::string Describe(int amount, int maximum)
{
    char buf[64];
    // Only give a ratio when the maximum is trustworthy — an earlier version
    // read the wrong slider trait and announced nonsense like "7 z 5".
    if (maximum > amount) std::snprintf(buf, sizeof(buf), strings::T("%d z %d", "%d of %d"),
                                        amount, maximum);
    else                  std::snprintf(buf, sizeof(buf), "%d", amount);
    return buf;
}

} // namespace

void Tick(float)
{
    if (menu::ActiveMenu() != menu::Id::Quantity) {
        if (g_open) { g_open = false; g_last_said.clear(); }
        return;
    }

    // Read the sampled state, never the live menu — see poll::QuantityState.
    int amount = 0, maximum = 0;
    if (!poll::QuantityState(&amount, &maximum)) return;

    if (!g_open) {
        g_open = true;
        g_last_said = Describe(amount, maximum);
        F3A_INFO("Quantity: open amount=%d max=%d", amount, maximum);
        tolk::Speak(strings::T("Ile? ", "How many? ") + g_last_said +
                    strings::T(". Strzałkami ustaw ilość, A zatwierdza, E anuluje.",
                               ". Arrows set the amount, A confirms, E cancels."),
                    tolk::Priority::Ui, true);
        return;
    }

    std::string now = Describe(amount, maximum);
    if (now == g_last_said) return;
    g_last_said = now;
    tolk::Speak(now, tolk::Priority::Ui, true);
}

void Init() { F3A_INFO("Quantity module ready."); }
void Shutdown() {}

} // namespace f3a::modules::quantity
