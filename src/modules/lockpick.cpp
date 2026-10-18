#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
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

// The bobby pin follows only the mouse with keyboard and mouse; Left/Right move
// it here (game::NudgeLockpick, on the main thread), Shift for fine steps.
// Per tick while held, so holding sweeps it and a tap nudges it.
constexpr int kPinStep     = 3;    // UI units per tick (~120 a second)
constexpr int kPinFineStep = 1;
bool g_said_help = false;

void PinKeys()
{
    const bool left  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
    const bool right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    if (left == right) return;                     // neither, or both
    const bool fine = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const int step = fine ? kPinFineStep : kPinStep;
    poll::RequestLockpickNudge(left ? -step : step);
}

void Tone(int freq, int ms)
{
    Beep(freq, ms);
}

void OnTick(float)
{
    if (!g_said_help) {
        g_said_help = true;
        tolk::Speak(strings::T("Wytrych: strzałki lewo, prawo przesuwają wytrych, z Shiftem powoli. "
                               "Dźwięk mówi, jak blisko jest właściwe miejsce.",
                               "Lockpick: left and right arrows move the pin, slowly with Shift. "
                               "The tone tells how close the right spot is."),
                    tolk::Priority::Ui, false);
    }
    PinKeys();

    auto s = game::GetLockpickState();
    // No real reading yet: an all-zero state announced "sweet spot" and beeped
    // (blocking this thread) every time a lock was opened. The lockpick cue
    // that does work lives in the world scanner's tick.
    if (!s.valid) return;
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

void OnOpen()
{
    g_last_delta = 9999.0f; g_announced_sweet = false; g_announced_brokensoon = false;
    g_said_help = false;
}
void OnClose() {}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::LockPick, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Lockpick module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::lockpick
