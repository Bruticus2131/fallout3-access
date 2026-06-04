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
#include "f3a/audio_beacon.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <cmath>
#include <string>

namespace f3a::modules::guide {
namespace {

bool        g_active = false;
game::Vec3  g_target{};
std::string g_name;

float g_ping_timer = 0.0f;   // positional beacon cadence
float g_dist_timer = 0.0f;   // spoken distance cadence

// No-progress ("blocked") detection: a straight-line beacon points through
// walls, so in corridors you can walk into a wall while the target stays the
// same distance away. We watch distance over a window and warn when it isn't
// shrinking, so the player knows to look for a way around (scanner → door).
float g_prog_timer = 0.0f;
float g_prog_dist  = 0.0f;
bool  g_warned_blocked = false;
constexpr float kProgEvery    = 2.5f;
constexpr float kProgMin      = 60.0f;   // must close at least this per window

constexpr float kArriveDist   = 140.0f;  // ~2 m
constexpr float kFloorDelta   = 160.0f;  // |dz| above this = different floor
constexpr float kDistEvery    = 4.0f;
constexpr float kRangeUnits   = 2500.0f; // distance mapped to 0..1 over this

// Beacon ping interval shortens as you close in (classic sonar): far apart,
// near rapid-fire.
constexpr float kPingFar      = 0.90f;
constexpr float kPingNear     = 0.18f;

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
    g_target     = pos;
    g_name       = name;
    g_active     = true;
    g_ping_timer = 0.0f;
    g_dist_timer = 0.0f;
    g_prog_timer = 0.0f;
    g_warned_blocked = false;

    auto pp = game::GetPlayerPosition();
    float dx = pos.x - pp.x, dy = pos.y - pp.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    g_prog_dist = dist;
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

    // Positional beacon ping: panned by heading, pitched front/back, quieter
    // with distance. The interval shortens as you approach. You steer by
    // turning until the ping is centred and high-pitched, then walk in.
    float dist01 = dist / kRangeUnits;
    if (dist01 > 1.0f) dist01 = 1.0f;
    float interval = kPingFar - (kPingFar - kPingNear) * (1.0f - dist01);

    g_ping_timer += dt;
    if (g_ping_timer >= interval) {
        g_ping_timer = 0.0f;
        audio::Ping(rel, dist01);
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

    // No-progress detector: are we actually getting closer?
    g_prog_timer += dt;
    if (g_prog_timer >= kProgEvery) {
        g_prog_timer = 0.0f;
        float closed = g_prog_dist - dist;     // positive = got closer
        g_prog_dist = dist;
        if (closed < kProgMin) {
            if (!g_warned_blocked) {
                g_warned_blocked = true;
                tolk::Speak("Brak postępu — przeszkoda. Poszukaj obejścia "
                            "skanerem, na przykład drzwi.",
                            tolk::Priority::System, true);
            }
        } else {
            g_warned_blocked = false;          // moving again; re-arm warning
        }
    }
}

void Init()
{
    audio::Init();
    F3A_INFO("Guide (beacon) module ready.");
}

void Shutdown()
{
    g_active = false;
    audio::Shutdown();
}

} // namespace f3a::modules::guide
