// AutoWalk — walks the player to a target picked with the object scanner.
//
// State machine (mirrors the architecture of the author's earlier mod):
//   AW_IDLE -> AW_WALKING -> (arrive | stuck | cancel) -> AW_IDLE
//
// Movement: we DON'T rotate the player to steer (writing rotZ fights the
// engine's own movement controller and makes the player orbit/spin). Instead
// we drive with the four movement keys (WASD) relative to whatever way the
// player faces — decomposing the heading-to-goal into forward/back + strafe —
// so the player slides toward the waypoint without ever turning. Trick
// borrowed from the "Better Autowalk" FOSE plugin. The player hears distance
// callouts and a clear stop reason.

#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/navmesh.h"
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
#include <vector>

namespace f3a::modules::autowalk {
namespace {

enum class State { Idle, Walking, Avoiding };

State        g_state = State::Idle;
game::Vec3   g_target_pos{};
std::string  g_target_name;

// Navmesh path (waypoints from player to target). When we have one we steer
// to the current waypoint instead of straight at the target — that's what
// routes around walls. Empty = fall back to straight-line + bug avoidance.
std::vector<game::Vec3> g_waypoints;
size_t       g_wp_index = 0;
bool         g_have_path = false;
constexpr float kWaypointRadius = 96.0f;   // advance when this close (~1.5 m)

// Stuck detection: compare position over a sliding window.
game::Vec3   g_last_probe_pos{};
float        g_probe_timer   = 0.0f;
constexpr float kProbeEvery  = 1.5f;   // seconds
constexpr float kMinProgress = 12.0f;  // game units per probe window

// Obstacle avoidance ("bug" algorithm): when blocked heading straight at the
// goal, veer off-axis to slide along the obstacle, then re-aim at the goal.
// We don't have raycasts, so we just TRY a side; if still stuck, widen the
// angle and alternate sides; after enough failures, give up.
float g_avoid_timer   = 0.0f;
int   g_avoid_side    = 1;     // +1 right, -1 left (alternates)
float g_avoid_angle   = 50.0f; // degrees off the goal heading
int   g_avoid_tries   = 0;
constexpr float kAvoidDuration = 1.6f;  // seconds spent veering per attempt
constexpr int   kAvoidMaxTries = 5;     // give up after this many

float YawToDegrees(const game::Vec3& from, const game::Vec3& to)
{
    float dx = to.x - from.x, dy = to.y - from.y;
    float deg = std::atan2(dx, dy) * 57.2957795f;   // 0 = north, matches game
    return deg;
}

// Distance callouts.
float        g_callout_timer = 0.0f;
constexpr float kCalloutEvery = 2.5f;

// Arrival radius (~1.7 m).
constexpr float kArriveDist = 110.0f;

// Different-floor guard: a straight-line driver can't climb stairs, so if the
// target is well above/below us, stop and tell the player to use the beacon.
constexpr float kFloorDelta = 160.0f;

// Movement keys as hardware scancodes (set 1). Default Fallout 3 bindings:
// W = forward, S = back, A = strafe left, D = strafe right. DirectInput sees
// scancode injection. (If the player rebinds movement this would need to read
// their bindings — defaults cover the vast majority.)
constexpr WORD kScanW = 0x11, kScanA = 0x1E, kScanS = 0x1F, kScanD = 0x20;

struct MoveKeys { bool w = false, a = false, s = false, d = false; };
MoveKeys g_keys;   // currently-held movement keys

void SendScan(WORD scan, bool down)
{
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wScan   = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

// Apply a desired key set, sending only the edges that changed (no key spam).
void ApplyKeys(MoveKeys want)
{
    if (want.w != g_keys.w) SendScan(kScanW, want.w);
    if (want.a != g_keys.a) SendScan(kScanA, want.a);
    if (want.s != g_keys.s) SendScan(kScanS, want.s);
    if (want.d != g_keys.d) SendScan(kScanD, want.d);
    g_keys = want;
}

void ReleaseAllKeys() { ApplyKeys(MoveKeys{}); }

// Drive toward a world point by strafing — never rotates the player. Decompose
// the heading-to-goal (relative to current facing) into an 8-way WASD combo;
// the bearing feedback loop corrects the residual angle as we move.
void DriveToward(const game::Vec3& target)
{
    auto pp = game::GetPlayerPosition();
    auto br = game::ComputeBearing(pp, game::GetPlayerYaw(), target);
    float rel = br.relative_yaw;        // -180..180, 0 = ahead, + = right
    MoveKeys k;
    k.w = (rel > -67.5f  && rel <  67.5f);
    k.s = (rel >  112.5f || rel < -112.5f);
    k.d = (rel >   22.5f && rel <  157.5f);
    k.a = (rel <  -22.5f && rel > -157.5f);
    ApplyKeys(k);
}

float DistanceToTarget()
{
    auto pp = game::GetPlayerPosition();
    float dx = g_target_pos.x - pp.x;
    float dy = g_target_pos.y - pp.y;
    return std::sqrt(dx * dx + dy * dy);
}

// The point we currently steer toward: the active waypoint if we have a
// navmesh path, otherwise the final target.
game::Vec3 CurrentGoal()
{
    if (g_have_path && g_wp_index < g_waypoints.size())
        return g_waypoints[g_wp_index];
    return g_target_pos;
}

// Advance past any waypoints we've already reached. Returns true if the path
// is exhausted (only the final target remains).
void AdvanceWaypoints()
{
    if (!g_have_path) return;
    auto pp = game::GetPlayerPosition();
    while (g_wp_index < g_waypoints.size()) {
        const auto& w = g_waypoints[g_wp_index];
        float dx = w.x - pp.x, dy = w.y - pp.y;
        if (std::sqrt(dx * dx + dy * dy) > kWaypointRadius) break;
        ++g_wp_index;
    }
}

void StopWalking(const char* reason_utf8)
{
    if (g_state == State::Idle) return;
    ReleaseAllKeys();
    g_state = State::Idle;
    if (reason_utf8 && *reason_utf8) {
        tolk::Speak(reason_utf8, tolk::Priority::System, true);
    }
}

// Steer along the goal heading offset by the current avoidance angle/side.
void SteerAvoiding()
{
    auto pp = game::GetPlayerPosition();
    float goalDeg  = YawToDegrees(pp, CurrentGoal());
    float veerDeg  = goalDeg + g_avoid_side * g_avoid_angle;
    float rad      = veerDeg * 0.01745329252f;
    game::Vec3 p{ pp.x + std::sin(rad) * 300.0f,
                  pp.y + std::cos(rad) * 300.0f, pp.z };
    DriveToward(p);
}

void EnterAvoiding()
{
    g_state       = State::Avoiding;
    g_avoid_timer = 0.0f;
    g_avoid_side  = -g_avoid_side;                  // alternate side each time
    g_avoid_angle = 50.0f + 20.0f * g_avoid_tries;  // widen with each failure
    if (g_avoid_angle > 110.0f) g_avoid_angle = 110.0f;
    ++g_avoid_tries;
    g_last_probe_pos = game::GetPlayerPosition();
    g_probe_timer    = 0.0f;
}

} // namespace

bool IsWalking() { return g_state != State::Idle; }

void StartTo(const game::Vec3& pos, const std::string& name)
{
    g_target_pos  = pos;
    g_target_name = name;

    g_last_probe_pos = game::GetPlayerPosition();
    g_probe_timer    = 0.0f;
    g_callout_timer  = 0.0f;
    g_avoid_tries    = 0;
    g_avoid_side     = 1;

    // Try to compute a real path around walls. If it fails (no navmesh,
    // endpoints off-mesh), g_have_path stays false and we walk straight with
    // bug-avoidance like before.
    g_waypoints.clear();
    g_wp_index  = 0;
    g_have_path = navmesh::BuildPath(game::GetPlayerPosition(), pos,
                                     g_waypoints) && !g_waypoints.empty();

    g_state = State::Walking;
    DriveToward(CurrentGoal());

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Idę do: %s, %s%s",
                  name.c_str(),
                  strings::FormatDistance(DistanceToTarget()).c_str(),
                  g_have_path ? ", trasa wyznaczona" : "");
    tolk::Speak(buf, tolk::Priority::System, true);
}

void Stop()
{
    StopWalking("Zatrzymano.");
}

float ProbeMoved()
{
    auto pp = game::GetPlayerPosition();
    float mx = pp.x - g_last_probe_pos.x;
    float my = pp.y - g_last_probe_pos.y;
    g_last_probe_pos = pp;
    return std::sqrt(mx * mx + my * my);
}

void Tick(float dt)
{
    if (g_state == State::Idle) return;

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

    // Different floor → a straight-line walk can't get there. Only bail when
    // we DON'T have a navmesh path; a real path handles stairs/height itself.
    if (!g_have_path) {
        float dz = g_target_pos.z - game::GetPlayerPosition().z;
        if (std::fabs(dz) > kFloorDelta) {
            StopWalking(dz > 0 ? "Cel jest wyżej. Użyj naprowadzania i schodów."
                               : "Cel jest niżej. Użyj naprowadzania i schodów.");
            return;
        }
    }

    if (g_state == State::Walking) {
        // Follow the path: advance through reached waypoints, steer at the
        // current one (or the target if we have no path).
        AdvanceWaypoints();
        DriveToward(CurrentGoal());

        g_probe_timer += dt;
        if (g_probe_timer >= kProbeEvery) {
            g_probe_timer = 0.0f;
            if (ProbeMoved() < kMinProgress) {
                // Blocked — try to go around instead of giving up.
                tolk::Speak("Przeszkoda, próbuję obejść.",
                            tolk::Priority::Background, false);
                EnterAvoiding();
            } else {
                g_avoid_tries = 0;   // making progress; reset failure count
            }
        }
    } else { // Avoiding
        SteerAvoiding();
        g_avoid_timer += dt;
        g_probe_timer  += dt;

        // Check progress mid-veer; if we're moving freely, resume straight.
        if (g_probe_timer >= kProbeEvery) {
            g_probe_timer = 0.0f;
            float moved = ProbeMoved();
            if (moved < kMinProgress) {
                if (g_avoid_tries >= kAvoidMaxTries) {
                    StopWalking("Nie mogę obejść przeszkody. "
                                "Spróbuj naprowadzania ręcznego.");
                    return;
                }
                EnterAvoiding();   // try the other side / wider angle
                return;
            }
        }
        if (g_avoid_timer >= kAvoidDuration) {
            g_state = State::Walking;   // re-aim at the goal
            g_probe_timer = 0.0f;
            g_last_probe_pos = game::GetPlayerPosition();
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
    // Never leave a movement key stuck down.
    ReleaseAllKeys();
    g_state = State::Idle;
}

} // namespace f3a::modules::autowalk
