// AutoWalk — walks the player to a target picked with the object scanner.
//
//   Idle -> Walking -> (arrive | stuck | cancel) -> Idle
//
// Movement = how a sighted player actually moves: TURN toward the goal with
// mouse-look (the engine's own turn path — writing player->rotZ directly fights
// the movement controller and makes the player spin), and hold FORWARD (W) once
// roughly facing it. Forward-only can't orbit a target the way pure strafing
// can. We follow navmesh waypoints when a path was found, else a straight line.
// The player hears distance callouts and a clear stop reason.

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

enum class State { Idle, Walking };

State        g_state = State::Idle;
game::Vec3   g_target_pos{};
std::string  g_target_name;

// Navmesh path (waypoints from player to target). When present we steer to the
// current waypoint instead of straight at the target — that routes around
// walls. Empty = straight-line.
std::vector<game::Vec3> g_waypoints;
size_t       g_wp_index   = 0;
bool         g_have_path  = false;
constexpr float kWaypointRadius = 96.0f;   // advance when this close (~1.5 m)

// Stuck detection: compare position over a sliding window.
game::Vec3   g_last_probe_pos{};
float        g_probe_timer  = 0.0f;
constexpr float kProbeEvery  = 2.0f;   // seconds
constexpr float kMinProgress = 16.0f;  // game units per probe window

// Distance callouts.
float        g_callout_timer = 0.0f;
constexpr float kCalloutEvery = 2.5f;

constexpr float kArriveDist = 55.0f;   // ~0.85 m — right up against the target

// Different-floor guard: with no navmesh path we can't climb stairs, so if the
// target is well above/below us, stop and tell the player to use the beacon.
constexpr float kFloorDelta = 160.0f;

// --- Turning via mouse-look ------------------------------------------------
// Relative bearing (deg, + = goal is to the right) -> horizontal mouse motion.
// CRUCIAL for third person: mouse-look there spins the CAMERA, not the body.
// The body only turns when it MOVES (and FO3 moves you in the camera direction
// in BOTH views, with rotZ snapping to the movement heading). So we must keep
// walking forward WHILE turning — then rotZ tracks the camera and this feedback
// loop converges. Gating forward on "already facing" deadlocked: body never
// moved, rotZ never updated, camera just spun around it forever.
//
// SMOOTH turn: small mouse step per tick so the view eases toward the target
// instead of whipping. Proportional (slows as it lines up) with a low cap, so a
// big turn is spread over many frames — gentle, not a jerk. Gain is INI-tunable
// (Voice/AutoWalkTurnGain); the low cap is what keeps it smooth regardless.
constexpr long  kTurnMax  = 32;      // max mouse counts per tick (low = smooth)
constexpr float kTurnDead = 6.0f;    // deg — don't fidget inside this

void TurnMouse(float relDeg)
{
    if (std::fabs(relDeg) <= kTurnDead) return;
    long gain = config::Get().autowalk_turn_gain;
    if (gain < 1) gain = 1;
    long dx = (long)(relDeg * (float)gain);
    if (dx >  kTurnMax) dx =  kTurnMax;
    if (dx < -kTurnMax) dx = -kTurnMax;
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = dx;             // relative, + = turn right
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// --- Forward key (W = scancode 0x11) ---------------------------------------
constexpr WORD kScanW = 0x11;
bool g_forward_down = false;

void Forward(bool down)
{
    if (down == g_forward_down) return;   // only send edges
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wScan   = kScanW;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
    g_forward_down = down;
}

float DistanceToTarget()
{
    auto pp = game::GetPlayerPosition();
    float dx = g_target_pos.x - pp.x;
    float dy = g_target_pos.y - pp.y;
    return std::sqrt(dx * dx + dy * dy);
}

// The point we currently steer toward: the active waypoint if we have a path,
// otherwise the final target.
game::Vec3 CurrentGoal()
{
    if (g_have_path && g_wp_index < g_waypoints.size())
        return g_waypoints[g_wp_index];
    return g_target_pos;
}

// Advance past waypoints we've already reached.
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

// Turn toward the goal AND keep walking forward (see TurnMouse note: forward
// motion is what makes the body — and rotZ — actually rotate). Returns |error|.
float Steer(const game::Vec3& goal)
{
    auto  pp  = game::GetPlayerPosition();
    auto  br  = game::ComputeBearing(pp, game::GetPlayerYaw(), goal);
    float rel = br.relative_yaw;          // -180..180, 0 = ahead, + = right
    TurnMouse(rel);
    Forward(true);
    return std::fabs(rel);
}

void StopWalking(const char* reason_utf8)
{
    if (g_state == State::Idle) return;
    Forward(false);
    g_state = State::Idle;
    if (reason_utf8 && *reason_utf8)
        tolk::Speak(reason_utf8, tolk::Priority::System, true);
}

float ProbeMoved()
{
    auto pp = game::GetPlayerPosition();
    float mx = pp.x - g_last_probe_pos.x;
    float my = pp.y - g_last_probe_pos.y;
    g_last_probe_pos = pp;
    return std::sqrt(mx * mx + my * my);
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

    // Try to compute a real path around walls. If it fails (no navmesh,
    // endpoints off-mesh), g_have_path stays false and we walk straight.
    g_waypoints.clear();
    g_wp_index  = 0;
    g_have_path = navmesh::BuildPath(game::GetPlayerPosition(), pos,
                                     g_waypoints) && !g_waypoints.empty();

    g_state = State::Walking;
    Steer(CurrentGoal());

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

void Tick(float dt)
{
    if (g_state == State::Idle) return;

    // Any menu (other than the HUD) or leaving gameplay cancels the walk.
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

    // Different floor → a straight-line walk can't get there. Only bail when we
    // have no navmesh path; a real path handles stairs/height itself.
    if (!g_have_path) {
        float dz = g_target_pos.z - game::GetPlayerPosition().z;
        if (std::fabs(dz) > kFloorDelta) {
            StopWalking(dz > 0 ? "Cel jest wyżej. Użyj naprowadzania i schodów."
                               : "Cel jest niżej. Użyj naprowadzania i schodów.");
            return;
        }
    }

    AdvanceWaypoints();
    Steer(CurrentGoal());

    // Stuck detection: if we've been holding forward but barely moved, give up
    // cleanly rather than grinding against geometry.
    g_probe_timer += dt;
    if (g_probe_timer >= kProbeEvery) {
        g_probe_timer = 0.0f;
        if (g_forward_down && ProbeMoved() < kMinProgress) {
            StopWalking("Utknąłem. Spróbuj naprowadzania ręcznego.");
            return;
        }
        if (!g_forward_down) g_last_probe_pos = game::GetPlayerPosition();
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
    Forward(false);   // never leave the forward key stuck down
    g_state = State::Idle;
}

} // namespace f3a::modules::autowalk
