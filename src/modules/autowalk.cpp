// AutoWalk — walks the player to a target picked with the object scanner.
//
// State machine (mirrors the architecture of the author's earlier mod):
//   AW_IDLE -> AW_WALKING -> (arrive | stuck | cancel) -> AW_IDLE
//
// Movement: we hold the game's forward key via SendInput (hardware scancode,
// so DirectInput sees it) and steer by writing the player's yaw toward the
// target every tick. No pathfinding — straight line with stuck detection;
// the player hears distance callouts and a clear stop reason.

#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>

namespace f3a::modules::autowalk {
namespace {

enum class State { Idle, Walking };

State        g_state = State::Idle;
game::Vec3   g_target_pos{};
std::string  g_target_name;

// Stuck detection: compare position over a sliding window.
game::Vec3   g_last_probe_pos{};
float        g_probe_timer   = 0.0f;
constexpr float kProbeEvery  = 1.5f;   // seconds
constexpr float kMinProgress = 12.0f;  // game units per probe window

// Distance callouts.
float        g_callout_timer = 0.0f;
constexpr float kCalloutEvery = 2.5f;

// Arrival radius (~1.7 m).
constexpr float kArriveDist = 110.0f;

// Different-floor guard: a straight-line driver can't climb stairs, so if the
// target is well above/below us, stop and tell the player to use the beacon.
constexpr float kFloorDelta = 160.0f;

// Gentle steering: snapping yaw straight at the target each tick makes the
// player orbit the goal (the heading error swings 180° as you pass abeam).
// Turn toward the desired heading by at most this many degrees per tick, and
// don't bother correcting within a small dead zone.
constexpr float kMaxTurnDeg  = 18.0f;
constexpr float kDeadZoneDeg = 4.0f;

void SteerToward(const game::Vec3& target)
{
    auto pp = game::GetPlayerPosition();
    auto br = game::ComputeBearing(pp, game::GetPlayerYaw(), target);
    float rel = br.relative_yaw;            // -180..180, 0 = ahead
    if (std::fabs(rel) <= kDeadZoneDeg) return;
    float step = rel;
    if (step >  kMaxTurnDeg) step =  kMaxTurnDeg;
    if (step < -kMaxTurnDeg) step = -kMaxTurnDeg;
    float new_yaw_deg = game::GetPlayerYaw() + step;
    // Convert back to a world point ahead at the new heading and reuse
    // SetPlayerYawTo, so all yaw-convention math stays in one place.
    float rad = new_yaw_deg * 0.01745329252f;
    game::Vec3 ahead{ pp.x + std::sin(rad) * 100.0f,
                      pp.y + std::cos(rad) * 100.0f, pp.z };
    game::SetPlayerYawTo(ahead);
}

// Forward key as a hardware scancode (W = 0x11). DirectInput sees scancode
// injection; configurable later if the user rebinds movement.
constexpr WORD kForwardScan = 0x11;

void SendForward(bool down)
{
    INPUT in{};
    in.type           = INPUT_KEYBOARD;
    in.ki.wScan       = kForwardScan;
    in.ki.dwFlags     = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

float DistanceToTarget()
{
    auto pp = game::GetPlayerPosition();
    float dx = g_target_pos.x - pp.x;
    float dy = g_target_pos.y - pp.y;
    return std::sqrt(dx * dx + dy * dy);
}

void StopWalking(const char* reason_utf8)
{
    if (g_state == State::Idle) return;
    SendForward(false);
    g_state = State::Idle;
    if (reason_utf8 && *reason_utf8) {
        tolk::Speak(reason_utf8, tolk::Priority::System, true);
    }
}

} // namespace

bool IsWalking() { return g_state == State::Walking; }

void StartTo(const game::Vec3& pos, const std::string& name)
{
    g_target_pos  = pos;
    g_target_name = name;

    game::SetPlayerYawTo(g_target_pos);

    g_last_probe_pos = game::GetPlayerPosition();
    g_probe_timer    = 0.0f;
    g_callout_timer  = 0.0f;

    g_state = State::Walking;
    SendForward(true);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Idę do: %s, %s",
                  name.c_str(),
                  strings::FormatDistance(DistanceToTarget()).c_str());
    tolk::Speak(buf, tolk::Priority::System, true);
}

void Stop()
{
    StopWalking("Zatrzymano.");
}

void Tick(float dt)
{
    if (g_state != State::Walking) return;

    // Any menu (other than the HUD) opening cancels the walk — so does
    // leaving gameplay (load, main menu).
    if (!poll::IsGameplayActive()) { StopWalking(nullptr); return; }
    auto active = menu::ActiveMenu();
    if (active != menu::Id::None && active != menu::Id::HUDMain) {
        StopWalking(nullptr);
        return;
    }

    float dist = DistanceToTarget();
    if (dist <= kArriveDist) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Dotarłeś: %s", g_target_name.c_str());
        StopWalking(buf);
        return;
    }

    // Different floor → a straight-line walk can't get there. Bail out and
    // point the player at the beacon instead.
    float dz = g_target_pos.z - game::GetPlayerPosition().z;
    if (std::fabs(dz) > kFloorDelta) {
        StopWalking(dz > 0 ? "Cel jest wyżej. Użyj naprowadzania i schodów."
                           : "Cel jest niżej. Użyj naprowadzania i schodów.");
        return;
    }

    // Steer toward the target gradually (avoids orbiting the goal).
    SteerToward(g_target_pos);

    // Stuck probe.
    g_probe_timer += dt;
    if (g_probe_timer >= kProbeEvery) {
        auto pp = game::GetPlayerPosition();
        float mx = pp.x - g_last_probe_pos.x;
        float my = pp.y - g_last_probe_pos.y;
        float moved = std::sqrt(mx * mx + my * my);
        g_last_probe_pos = pp;
        g_probe_timer    = 0.0f;
        if (moved < kMinProgress) {
            StopWalking("Przeszkoda. Zatrzymano.");
            return;
        }
    }

    // Periodic distance callout.
    g_callout_timer += dt;
    if (g_callout_timer >= kCalloutEvery) {
        g_callout_timer = 0.0f;
        tolk::Speak(strings::FormatDistance(dist),
                    tolk::Priority::Background, false);
    }
}

void Init()
{
    F3A_INFO("AutoWalk module ready.");
}

void Shutdown()
{
    // Never leave the forward key stuck down.
    if (g_state == State::Walking) SendForward(false);
    g_state = State::Idle;
}

} // namespace f3a::modules::autowalk
