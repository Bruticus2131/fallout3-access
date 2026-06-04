// Beacon guidance — the player walks themselves (W), the mod steers by ear.
//
// Why not drive the player automatically? Auto-walk orbits the target (the
// heading error blows up near the goal) and dies in corridors (straight line
// hits walls). A blind player avoids obstacles far better than a naive
// straight-line driver — so we keep the human in control of movement and only
// provide continuous directional feedback, the way audio games do:
//
//   * on course (heading within a few degrees)   -> short high beep
//   * off course                                  -> spoken clock direction
//   * every few seconds                           -> spoken distance
//   * target on another floor (|dz| large)        -> "above"/"below" hint
//   * arrived                                      -> announce + stop
//
// No engine control = no spinning, works indoors.

#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cmath>
#include <thread>
#include <string>

namespace f3a::modules::guide {
namespace {

bool        g_active = false;
game::Vec3  g_target{};
std::string g_name;

float g_cue_timer  = 0.0f;   // beep / direction cue cadence
float g_dist_timer = 0.0f;   // spoken distance cadence
std::string g_last_clock;    // avoid repeating the same clock cue

constexpr float kArriveDist   = 140.0f;  // ~2 m
constexpr float kOnCourseDeg  = 12.0f;
constexpr float kFloorDelta   = 160.0f;  // |dz| above this = different floor
constexpr float kCueEvery     = 0.55f;
constexpr float kDistEvery    = 3.0f;

void BeepAsync(int freq, int ms)
{
    std::thread([freq, ms] { Beep(freq, ms); }).detach();
}

bool GameplayAndHud()
{
    if (!poll::IsGameplayActive()) return false;
    auto m = menu::ActiveMenu();
    return m == menu::Id::None || m == menu::Id::HUDMain;
}

} // namespace

bool IsActive() { return g_active; }

void StartTo(const game::Vec3& pos, const std::string& name)
{
    g_target    = pos;
    g_name      = name;
    g_active    = true;
    g_cue_timer = 0.0f;
    g_dist_timer = 0.0f;
    g_last_clock.clear();

    auto pp = game::GetPlayerPosition();
    float dx = pos.x - pp.x, dy = pos.y - pp.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    tolk::Speak("Prowadzę do: " + name + ", " +
                strings::FormatDistance(dist),
                tolk::Priority::System, true);
}

void Stop()
{
    if (!g_active) return;
    g_active = false;
    tolk::Speak("Naprowadzanie wyłączone.", tolk::Priority::System, true);
}

void Tick(float dt)
{
    if (!g_active) return;
    if (!GameplayAndHud()) { g_active = false; return; }

    auto pp   = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    float dx  = g_target.x - pp.x;
    float dy  = g_target.y - pp.y;
    float dz  = g_target.z - pp.z;
    float dist = std::sqrt(dx * dx + dy * dy);

    if (dist <= kArriveDist) {
        g_active = false;
        tolk::Speak("Dotarłeś: " + g_name, tolk::Priority::System, true);
        return;
    }

    // Relative heading: -180..180, 0 = dead ahead.
    auto br = game::ComputeBearing(pp, yaw, g_target);
    float rel = br.relative_yaw;

    g_cue_timer += dt;
    if (g_cue_timer >= kCueEvery) {
        g_cue_timer = 0.0f;
        if (std::fabs(rel) <= kOnCourseDeg) {
            // On course: bright confirming beep. Pitch a touch higher the
            // straighter you are.
            int freq = 1500 + (int)((kOnCourseDeg - std::fabs(rel)) * 20);
            BeepAsync(freq, 30);
            g_last_clock.clear();
        } else {
            // Off course: speak the clock direction, but only when it
            // changes, so small wobble doesn't chatter.
            std::string clock = strings::ClockDirection(rel);
            if (clock != g_last_clock) {
                g_last_clock = clock;
                tolk::Speak(clock, tolk::Priority::Ui, true);
            } else {
                // Same direction still needed: a low nudge beep.
                BeepAsync(rel < 0 ? 500 : 700, 25);
            }
        }
    }

    g_dist_timer += dt;
    if (g_dist_timer >= kDistEvery) {
        g_dist_timer = 0.0f;
        std::string msg = strings::FormatDistance(dist);
        if (std::fabs(dz) > kFloorDelta) {
            msg += dz > 0 ? ", cel wyżej — poszukaj schodów"
                          : ", cel niżej — poszukaj schodów";
        }
        tolk::Speak(msg, tolk::Priority::Background, false);
    }
}

void Init()     { F3A_INFO("Guide (beacon) module ready."); }
void Shutdown() { g_active = false; }

} // namespace f3a::modules::guide
