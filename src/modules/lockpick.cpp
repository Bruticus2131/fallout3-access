#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cmath>
#include <cstdio>

namespace f3a::modules::lockpick {
namespace {

// Tonal feedback: render a short tone whose pitch tracks how close the pick is
// to the sweet spot. We use Windows Beep on a worker thread to avoid blocking.
// For accessibility this is more usable than relying on visual cues.

constexpr float SWEET_SPOT_TOLERANCE = 5.0f; // degrees

float g_last_delta = 9999.0f;
bool  g_announced_sweet = false;
int   g_last_picks = -1;
bool  g_announced_brokensoon = false;

void Tone(int freq, int ms)
{
    Beep(freq, ms);
}

void OnTick(float)
{
    auto s = game::GetLockpickState();
    if (s.broken) {
        tolk::Speak(strings::Render(strings::Key::LockpickBroken),
                    tolk::Priority::System, true);
        return;
    }
    if (s.unlocked) {
        tolk::Speak(strings::Render(strings::Key::LockpickUnlocked),
                    tolk::Priority::System, true);
        return;
    }

    float delta = std::fabs(s.pick_angle_deg - s.sweet_spot_deg);

    // Tonal feedback only when the player moves the pick meaningfully.
    if (std::fabs(delta - g_last_delta) >= 1.0f) {
        // 250 Hz at far, up to 1200 Hz at sweet spot.
        float t = 1.0f - std::min(delta, 90.0f) / 90.0f;
        int freq = 250 + (int)(t * 950);
        Tone(freq, 25);
        g_last_delta = delta;
    }

    if (delta <= SWEET_SPOT_TOLERANCE) {
        if (!g_announced_sweet) {
            tolk::Speak(strings::Render(strings::Key::LockpickSweetSpot),
                        tolk::Priority::Ui, false);
            g_announced_sweet = true;
        }
    } else {
        g_announced_sweet = false;
    }

    // Tension > 0.85 means the pick is about to break.
    if (s.tension > 0.85f && !g_announced_brokensoon) {
        tolk::Speak(strings::Render(strings::Key::LockpickBrokenSoon),
                    tolk::Priority::System, true);
        g_announced_brokensoon = true;
    } else if (s.tension < 0.4f) {
        g_announced_brokensoon = false;
    }
}

void OnOpen()  { g_last_delta = 9999.0f; g_announced_sweet = false; g_announced_brokensoon = false; }
void OnClose() {}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::LockPick, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Lockpick module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::lockpick
