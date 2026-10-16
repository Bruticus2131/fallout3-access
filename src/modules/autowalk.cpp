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
#include "f3a/player_mover.h"
#include "f3a/menu_dispatch.h"
#include "f3a/hotkeys.h"
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
const void*  g_target_refr = nullptr;   // destination ref (for Alt+Home teleport)
uint32_t     g_target_form = 0;
// Quest mode: re-read the live objective marker each tick instead of walking to
// the position it had when the key was pressed. Objectives advance mid-route
// (and DLC quests can hijack the tracked slot), so a cached marker walks you to
// the wrong place. The beacon already worked this way.
bool         g_follow_quest = false;

// Navmesh path (waypoints from player to target). When present we steer to the
// current waypoint instead of straight at the target — that routes around
// walls. Empty = straight-line.
std::vector<game::Vec3> g_waypoints;
size_t       g_wp_index   = 0;
bool         g_have_path  = false;
// Places this walk must route round: locked doors met on the way.
std::vector<game::Vec3> g_avoid;
// Lost-the-path check: how far off the route counts as lost, and how often a
// new route may be worked out (a rebuild every tick would be its own problem).
constexpr float kLostDist       = 450.0f;
bool g_lost_said = false;   // "lost the route" already said in this episode
constexpr float kRebuildEvery   = 3.0f;
float        g_rebuild_cooldown = 0.0f;
constexpr float kWaypointRadius = 96.0f;   // advance when this close (~1.5 m)

// The engine's pathfinder is the PRIMARY route, as in the FNV mod; ours (the
// navmesh A*) is worked out too, but only walked when the engine has nothing.
game::Vec3 g_route_goal{};          // where the last engine request is to lead
bool   g_route_engine   = false;    // the route being walked is the engine's
bool   g_route_partial  = false;    // ...and it ends short of its goal
float  g_leg_start_dist = 0.0f;     // distance to the goal when that leg began
int    g_route_legs     = 0;        // engine legs asked for on this walk
float  g_engine_wait    = 0.0f;     // s left to stand and wait for its answer
bool   g_engine_given_up = false;   // a short leg gained nothing: ours from here
bool   g_engine_said_none = false;  // its last answer was "no route"
bool   g_cross_engine   = false;    // cross mode: the game's route to the TARGET is awaited
game::Vec3 g_probe_pos{};           // where the player stood at the last stall probe
constexpr float kEngineWaitSecs  = 1.5f;
constexpr int   kMaxRouteLegs    = 100;     // the FNV mod's cap
constexpr float kPartialResidual = 256.0f;  // route ends this far short = partial
constexpr float kLegMinGain      = 500.0f;  // a leg must gain this to ask for another
// While we have no path, keep retrying BuildPath as we move — once we near the
// stairs / a connected mesh region the path (which handles height) is found.
float        g_path_retry_timer = 0.0f;
bool         g_floor_warned     = false;   // announced the "other floor" hint once
constexpr float kPathRetryEvery = 2.0f;    // seconds between path retries

// Stuck detection: track PROGRESS TOWARD THE TARGET over a sliding window, not
// raw movement — otherwise circling in place (moving, but not getting closer)
// reads as "not stuck" and the walker spins forever near an unreachable marker.
float        g_last_probe_dist = 0.0f;  // distance-to-target at the last probe
float        g_probe_timer  = 0.0f;
constexpr float kProbeEvery  = 2.0f;   // seconds
constexpr float kMinProgress = 16.0f;  // must get this many units CLOSER per window

// No distance callouts during a walk: the player asked for a quiet walk, and
// the beacon/scanner say how far on request.
float        g_close_timer   = 0.0f;   // seconds spent within kNearArrive

// Door-opening: when blocked, the obstacle is usually a closed door (e.g. the
// Overseer's office). Open the nearest one with the native Activate and resume.
// Track the last door tried so that, if we're STILL stuck against it after a
// grace period, we give up instead of toggling it shut again.
uint32_t     g_last_door     = 0;
float        g_door_cooldown = 0.0f;
// The door we just opened: hold still while it swings, then check it really
// opened before walking on.
float        g_door_hold     = 0.0f;
const void*  g_door_refr     = nullptr;
uint32_t     g_door_form     = 0;
std::string  g_door_name;
int          g_door_checks   = 0;        // state checks left before giving up
constexpr float kDoorHoldSecs  = 0.9f;   // stand still while the door swings
constexpr float kDoorGraceSecs = 2.0f;   // then don't count the doorway as stuck
constexpr float kDoorReach   = 300.0f;   // ~4.7 m — a door we've walked up to
// Stopping the walk at a door is an interruption, so it needs a tighter test
// than merely noticing one nearby: close, and roughly in the direction we are
// actually walking.
constexpr float kDoorStopReach = 200.0f;  // ~3 m
constexpr float kDoorStopCone  = 55.0f;   // degrees either side of the heading

// --- Cross-worldspace routing ----------------------------------------------
// When the target sits in a DIFFERENT coordinate frame (another cell/worldspace)
// we can't walk straight at its coordinates — we travel the LOAD DOORS that
// connect the spaces. We steer to a chosen load door, activate it to pass
// through, wait for the cell to load, then re-evaluate on the far side and
// repeat. Preference goes to a door that leads directly into the target's space;
// otherwise the nearest load door that doesn't just send us back where we came
// from. If no connecting door is reachable, or we get wedged, we teleport to the
// target as a last resort — so the player ALWAYS arrives.
bool         g_cross            = false;          // routing to a door to change space
const void*  g_cross_door_refr  = nullptr;        // door we're walking to
uint32_t     g_cross_door_form  = 0;
game::Vec3   g_cross_door_pos{};
std::string  g_cross_door_name;
// Transition tracking: after activating a door (or teleporting) we wait for the
// player's CELL to actually change, then let it settle a beat before reading the
// new position. g_cross_wait counts DOWN as an overall timeout for the change to
// happen (a locked/failed door never changes the cell → fall back); it only
// ticks while gameplay is active, so a load screen doesn't burn it. >0 means "a
// transition is in progress".
float        g_cross_wait       = 0.0f;
float        g_cross_settle     = 0.0f;           // settle timer after the cell changed
const void*  g_cross_prev_cell  = nullptr;        // cell at activation (detect the change)
int          g_hop_count        = 0;              // load doors traversed this trip
int          g_cross_stuck      = 0;              // stalls while approaching a door
bool         g_cross_teleported = false;          // already used the teleport fallback
const void*  g_cross_from_cell  = nullptr;        // cell we left (skip the door back)
// Doors taken on this walk. They are DEPRIORITISED, not banned: a route that
// has to back out of a dead end still needs them, and in a vault that happens.
std::vector<uint32_t> g_cross_used_doors;
bool         g_target_indoors   = false;          // is the destination inside?
constexpr int   kMaxHops        = 12;             // give up chaining doors past this
constexpr float kCrossTimeout   = 8.0f;           // s of active play w/o a cell change → fallback
constexpr float kCrossSettle    = 0.8f;           // s to settle after the cell changes
constexpr float kCrossDoorReach = 160.0f;         // close enough to use a load door
constexpr int   kCrossMaxStuck  = 4;              // stalls before the teleport fallback
constexpr int   kCrossScan      = 6000;           // radius to hunt for a load door

constexpr float kArriveDist = 90.0f;   // ~1.4 m — close enough to activate
// If we stall this close to the target, treat it as arrival rather than an
// obstacle: the exact origin is often unreachable (inside furniture, behind a
// counter, off-navmesh), so "stuck right next to it" means we're effectively there.
constexpr float kNearArrive = 175.0f;  // ~2.7 m

// Floor-independent arrival backstop. The strict test also demands the same
// floor, and a target whose Z is wrong — quest markers especially — keeps that
// false for good, so a walk could circle its destination forever without ever
// announcing that it got there. Standing this close for this long IS arriving,
// whatever the height difference claims.
constexpr float kCloseArriveSecs = 2.5f;

// Different-floor guard: with no navmesh path we can't climb stairs, so if the
// target is well above/below us, stop and tell the player to use the beacon.
constexpr float kFloorDelta = 160.0f;

// No-reference cross-space test: with no target reference to compare cells with,
// a target this far away that the navmesh cannot path to is treated as being in
// another location rather than walked at blindly.
constexpr float kNoPathFarDist = 4000.0f;

// How near the end of a navmesh path has to land for that path to count as
// "this target is reachable on foot from here", whatever the cell records say.
// Generous on purpose: the last waypoint sits on walkable ground, and a target
// standing on a staircase, a platform or inside furniture is legitimately some
// way off the mesh — the overseer's office is 775 units above the floor the
// path ends on.
constexpr float kPathEndsNearTarget = 1000.0f;

// --- Turning via mouse-look ------------------------------------------------
// Relative bearing (deg, + = goal is to the right) -> horizontal mouse motion.
// CRUCIAL for third person: mouse-look there spins the CAMERA, not the body.
// The body only turns when it MOVES (and FO3 moves you in the camera direction
// in BOTH views, with rotZ snapping to the movement heading). So we must keep
// walking forward WHILE turning — then rotZ tracks the camera and this feedback
// loop converges. Gating forward on "already facing" deadlocked: body never
// moved, rotZ never updated, camera just spun around it forever.
//
// Turn rate. A low cap looked smoother but turned too slowly to track sharp
// corrections (the walker wandered / missed targets), so keep it fast enough to
// steer reliably — the "whip" is only visual and the player can't see it.
// Proportional, so it still eases as it lines up. Gain is INI-tunable
// (Voice/AutoWalkTurnGain).
constexpr long  kTurnMax  = 220;     // max mouse counts per tick
constexpr float kTurnDead = 4.0f;    // deg — don't turn inside this

// --- The key-injection gate ------------------------------------------------
//
// Under native walking NOTHING here presses a key or moves the mouse. Every
// "it said Zatrzymano and kept walking" bug this mod has had came from the same
// shape: an injected key outliving the walk that sent it — a key-up swallowed
// while a menu had focus, a recovery branch that pressed W without telling the
// mover, a stop that cleared one drive but not the other. A walk a blind player
// cannot stop is worse than a walk that steers badly: you walk into the enemy,
// into the radiation, off the walkway, with no way to break off.
//
// So the fix is not a better stop, it is having nothing to stop. The engine
// mover is driven by RESTATING a goal every tick, so not restating it is
// already a full stop — no state is left anywhere that could keep the player
// moving. That is how the FNV mod gets a reliable stop, and it is what the
// player asked for here: no keys at all, unless [Voice] NativeWalk is 0.
//
// Releases are never gated: if a run with NativeWalk = 0 left a key down, we
// still want to let go of it.
bool KeysAllowed() { return !config::Get().native_walk; }

// Native walking asked for, but the movement hook is not in place (a different
// game build). Refuse to walk rather than quietly fall back to keys — that
// silent fallback is precisely the thing being removed.
DWORD g_no_mover_said = 0;
void WarnNoMover()
{
    DWORD now = GetTickCount();
    if (g_no_mover_said && now - g_no_mover_said < 5000) return;
    g_no_mover_said = now;
    F3A_INFO("autowalk: NativeWalk on but the mover hook is not installed — "
             "refusing to walk (set [Voice] NativeWalk = 0 for the old key walk).");
    tolk::Speak("Auto-chodzenie niedostępne na tej wersji gry. "
                "Ustaw NativeWalk na 0 w pliku INI, żeby chodzić klawiszami.",
                tolk::Priority::System, true);
}

void TurnMouse(float relDeg)
{
    if (!KeysAllowed()) return;      // native walking turns the body itself
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

// --- Movement keys (scancodes: W=0x11, A=0x1E, S=0x1F, D=0x20) --------------
constexpr WORD kScanW = 0x11;
constexpr WORD kScanA = 0x1E;
constexpr WORD kScanS = 0x1F;
constexpr WORD kScanD = 0x20;
bool g_forward_down = false;
int  g_strafe_down = 0;   // 0 = none, -1 = left (A), +1 = right (D)

void SendScan(WORD scan, bool down)
{
    // Belt and braces for the gate above: whatever path reaches here, a PRESS
    // is impossible under native walking, so no caller added later can quietly
    // reintroduce a key that outlives its walk. Releases always go through.
    if (down && !KeysAllowed()) return;
    hotkeys::SuppressKey(scan);   // ours, not the player's — see SuppressKey
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wScan   = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

// Hold any combination of the four move keys (edges only). This lets us drive
// the player by STRAFE — sliding toward the goal in 8 directions WITHOUT turning
// the camera (mouse-free) — instead of mouse-look. g_forward_down/g_strafe_down
// mirror the W and A/D state for the stuck probe and clean release.
bool g_kw = false, g_ka = false, g_ks = false, g_kd = false;

void Move(bool w, bool a, bool s, bool d)
{
    // Gate here as well as in SendScan, so the bookkeeping stays honest:
    // g_kw..g_kd must keep matching the game, or the next edge sends a key-up
    // for a key that was never pressed.
    if (!KeysAllowed()) { w = a = s = d = false; }
    if (w != g_kw) { SendScan(kScanW, w); g_kw = w; }
    if (a != g_ka) { SendScan(kScanA, a); g_ka = a; }
    if (s != g_ks) { SendScan(kScanS, s); g_ks = s; }
    if (d != g_kd) { SendScan(kScanD, d); g_kd = d; }
    g_forward_down = (w || a || s || d);   // "trying to move" for the stuck probe
    g_strafe_down  = a ? -1 : (d ? 1 : 0);
}

void Forward(bool down) { Move(down, false, false, false); }

// Jump (Space, scancode 0x39) — a classic unstick move: hop off a ledge/lip or
// over low rubble when strafing alone can't free us (e.g. wedged high on a rock
// at the start of a valid path). Held briefly then released = one hop.
constexpr WORD kScanJump = 0x39;
bool  g_kjump      = false;
float g_jump_timer = 0.0f;
void SetJump(bool down) { if (down != g_kjump) { SendScan(kScanJump, down); g_kjump = down; } }

// Let go of every movement key, whether or not we think it is down. Move() only
// sends EDGES, so the moment our bookkeeping and the game disagree — a key-up
// swallowed while a menu had focus, a walk cut short between the press and the
// release — the game is left holding W and the player walks on with nothing
// driving them. On the way out of a walk, two redundant messages are cheap
// insurance against that.
void ReleaseKeysHard()
{
    SendScan(kScanW, false);
    SendScan(kScanA, false);
    SendScan(kScanS, false);
    SendScan(kScanD, false);
    SendScan(kScanJump, false);
    g_kw = g_ka = g_ks = g_kd = false;
    g_kjump        = false;
    g_forward_down = false;
    g_strafe_down  = 0;
}
void TriggerJump()
{
    if (!KeysAllowed()) return;   // Space is an injected key like any other
    if (!g_kjump) { SetJump(true); g_jump_timer = 0.30f; }
}

// --- Stuck recovery: when forward progress stalls, strafe sideways to slip
// around whatever is there. It "feels" the obstacle by trial, since there is no
// path to follow when the navmesh gives none.
//
// It used to flip sides on EVERY stall, and that is why a walk toward something
// just past a crate turned into pacing: a step left, a step right, back to the
// same spot, forever — a log of it shows the distance going 253, 258, 354 while
// the mod kept announcing the same container. Going round an obstacle means
// COMMITTING to one side long enough to clear it. So the side is kept, and each
// further stall on that side strafes for longer; only after several failures is
// the other side tried, with the clock reset.
float g_recover_timer = 0.0f;   // seconds of strafing left; 0 = normal steering
int   g_recover_dir   = 1;      // side being tried
int   g_recover_tries = 0;      // consecutive attempts on THIS side
int   g_stuck_count   = 0;      // consecutive stalls without progress
constexpr float kRecoverSecs = 1.2f;
constexpr int   kSideTries   = 3;   // attempts before trying the other side
// Orbit detection: the closest we have ever been this walk, and how many
// probes in a row have failed to beat it.
float g_best_dist = 1e9f;
int   g_noimprove = 0;
constexpr float kOrbitRadius = 400.0f;  // only call it an orbit when already near
constexpr int   kOrbitProbes = 4;       // probes without improvement before stopping
constexpr int   kMaxStuck    = 6;   // give up after this many failed sidesteps
                                    // (open terrain w/o navmesh needs more tries)

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

// Ask the game's own pathfinder for a route (it routes NPCs: doors, stairs,
// the links between navmeshes). False when it can't be asked — no reference to
// route to, switched off, or given up on for this walk.
bool RequestEngineRouteTo(const void* refr, uint32_t form, const game::Vec3& goal,
                          float radius)
{
    if (!refr || !form || g_engine_given_up || !config::Get().engine_path) return false;
    g_route_goal = goal;
    mover::RequestEnginePath(refr, form, radius);
    return true;
}

// The route for what the walk is doing now: the door it is heading for in
// cross mode, the target otherwise.
bool RequestEngineRoute()
{
    if (g_cross)
        return g_cross_door_refr &&
               RequestEngineRouteTo(g_cross_door_refr, g_cross_door_form,
                                    g_cross_door_pos, kCrossDoorReach * 0.5f);
    return RequestEngineRouteTo(g_target_refr, g_target_form, g_target_pos, kArriveDist);
}

float DistanceToRouteGoal()
{
    auto pp = game::GetPlayerPosition();
    float dx = g_route_goal.x - pp.x, dy = g_route_goal.y - pp.y;
    return std::sqrt(dx * dx + dy * dy);
}

// A route that runs through somewhere this walk has learned to avoid (a locked
// door) is no route.
//
// A reroute starts AT the locked door, so its first points always lie near it.
// When the player already stands in a door's zone, only points clearly nearer
// the door than the player count — a route going through it, not one leaving.
bool RouteHitsAvoided(const std::vector<game::Vec3>& wp)
{
    const float kZone2 = 120.0f * 120.0f;
    auto pp = game::GetPlayerPosition();
    for (const auto& a : g_avoid) {
        float pdx = pp.x - a.x, pdy = pp.y - a.y;
        float pd2 = pdx * pdx + pdy * pdy;
        for (const auto& p : wp) {
            float dx = p.x - a.x, dy = p.y - a.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < kZone2 && std::fabs(p.z - a.z) < 160.0f &&
                (pd2 >= kZone2 || d2 < pd2 * 0.5f))
                return true;
        }
    }
    return false;
}

// Start a freshly adopted route at the leg the player is on now, not at its
// first point: the answer takes a moment, and turning back to where the
// player stood when asking is a step backwards. Only the route's first stretch
// is searched, so a route that doubles back past the player is not cut short.
void SnapToRoute()
{
    g_wp_index = 0;
    if (g_waypoints.size() < 2) return;
    auto pp = game::GetPlayerPosition();
    float best = 1e30f, walked = 0.0f;
    size_t bestI = 1;
    for (size_t i = 1; i < g_waypoints.size() && walked <= 800.0f; ++i) {
        const auto& a = g_waypoints[i - 1];
        const auto& b = g_waypoints[i];
        float sx = b.x - a.x, sy = b.y - a.y;
        float len2 = sx * sx + sy * sy;
        float t = len2 > 1.0f ? ((pp.x - a.x) * sx + (pp.y - a.y) * sy) / len2 : 1.0f;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        float lx = a.x + t * sx - pp.x, ly = a.y + t * sy - pp.y;
        float d2 = lx * lx + ly * ly;
        if (d2 < best) { best = d2; bestI = i; }
        walked += std::sqrt(len2);
    }
    g_wp_index = bestI;
}

// Take the game's route once it is ready.
void AdoptEngineRoute()
{
    int st = mover::EnginePathState();
    if (st != 2 && st != 3) return;
    std::vector<game::Vec3> wp;
    mover::TakeEnginePath(&wp);
    g_engine_wait = 0.0f;                // answered, one way or the other
    if (st == 3 || wp.empty()) {
        g_engine_said_none = true;
        F3A_INFO("autowalk: no engine route - %s (%u waypoint(s))",
                 g_have_path ? "walking our own" : "none of ours either",
                 (unsigned)g_waypoints.size());
        return;
    }
    if (RouteHitsAvoided(wp)) {
        F3A_INFO("autowalk: engine route goes through an avoided door - ignored");
        return;
    }
    // Partial: it ends well short of where it was asked to go. The engine
    // routes only as far as the navmesh it has loaded; the next leg is asked
    // for when this one is walked (see Tick).
    const auto& end = wp.back();
    float rx = end.x - g_route_goal.x, ry = end.y - g_route_goal.y;
    float residual = std::sqrt(rx * rx + ry * ry);
    bool partial = residual > kPartialResidual;
    F3A_INFO("autowalk: engine route adopted, %u waypoint(s)%s (ours had %u)",
             (unsigned)wp.size(), partial ? ", PARTIAL" : "", (unsigned)g_waypoints.size());
    if (partial) F3A_INFO("autowalk: it ends %.0f units short of its goal", residual);
    g_waypoints     = std::move(wp);
    g_have_path     = true;
    g_route_engine  = true;
    g_route_partial = partial;
    g_leg_start_dist = DistanceToRouteGoal();
    SnapToRoute();
    g_last_probe_dist = DistanceToTarget();
}

// Before walking a route of the engine's, the player waits for it: drop what
// was being followed and ask. Used for the door in cross mode, where our own
// navmesh route (to the target) means nothing.
void RouteToDoor()
{
    g_waypoints.clear();
    g_wp_index      = 0;
    g_have_path     = false;
    g_route_engine  = false;
    g_route_partial = false;
    if (RequestEngineRoute()) g_engine_wait = kEngineWaitSecs;
}

// Work the route out again from where the player stands now, steering round
// everything in g_avoid. Says `why` (if any) so the player knows the walk has
// not simply wandered off. Returns whether a route was found.
bool RebuildPath(const char* why)
{
    g_rebuild_cooldown = kRebuildEvery;
    std::vector<game::Vec3> wp;
    bool ok = navmesh::BuildPath(game::GetPlayerPosition(), g_target_pos, wp,
                                 &g_avoid) && !wp.empty();
    // The avoid list steers the search, but a route that still runs through
    // a locked door is no route round it.
    bool hits = ok && !g_avoid.empty() && RouteHitsAvoided(wp);
    if (hits) ok = false;
    F3A_INFO("autowalk: rebuild (%s) -> %s, %u waypoint(s), %u avoided",
             why ? why : "silent",
             ok ? "OK" : hits ? "only through an avoided door" : "no path",
             (unsigned)wp.size(), (unsigned)g_avoid.size());
    if (ok) {
        g_waypoints = std::move(wp);
        g_wp_index  = 0;
        g_have_path = true;
        g_route_engine  = false;      // ours, until the engine's answer replaces it
        g_route_partial = false;
    }
    RequestEngineRoute();
    if (why && *why) tolk::Speak(why, tolk::Priority::Ui, false);
    return ok;
}

// Advance past waypoints we've already reached — or already passed: the
// player projected onto the leg being walked (previous waypoint -> this one)
// is at its end, and not far off to the side of it. The look-ahead steering
// below rounds a bend without touching its corner, so "within reach of the
// waypoint" alone would leave the route pointing back at a corner already
// behind the player (the FNV mod advances the same way).
void AdvanceWaypoints()
{
    if (!g_have_path) return;
    auto pp = game::GetPlayerPosition();
    for (int guard = 0; guard < 16 && g_wp_index < g_waypoints.size(); ++guard) {
        const auto& w = g_waypoints[g_wp_index];
        float dx = w.x - pp.x, dy = w.y - pp.y;
        bool reached = std::sqrt(dx * dx + dy * dy) <= kWaypointRadius;
        bool passed  = false;
        if (!reached && g_wp_index > 0) {
            const auto& a = g_waypoints[g_wp_index - 1];
            float sx = w.x - a.x, sy = w.y - a.y;
            float len2 = sx * sx + sy * sy;
            if (len2 > 1.0f) {
                float t = ((pp.x - a.x) * sx + (pp.y - a.y) * sy) / len2;
                float lx = a.x + t * sx - pp.x, ly = a.y + t * sy - pp.y;
                passed = t >= 0.92f && lx * lx + ly * ly <= 4.0f * kWaypointRadius * kWaypointRadius;
            }
        }
        if (!reached && !passed) break;
        ++g_wp_index;
    }
}

// The point to steer at, the FNV mod's "corridor-hugging" way: the player is
// projected onto the leg being walked and the aim is taken a little further
// along the route, so a bend is rounded smoothly instead of walking corner to
// corner — but never round a corner sharper than 60°, which would cut its
// inside into a wall. `corner` gets the real next waypoint, for braking.
constexpr float kLookAhead      = 60.0f;   // 1.5x the turning radius (160 u/s / 4 rad/s)
constexpr float kSharpCornerCos = 0.5f;    // cos 60°

game::Vec3 LookAheadGoal(game::Vec3* corner)
{
    game::Vec3 goal = CurrentGoal();
    if (corner) *corner = goal;
    if (!g_have_path || g_wp_index == 0 || g_wp_index >= g_waypoints.size()) return goal;
    auto pp = game::GetPlayerPosition();
    const auto& a = g_waypoints[g_wp_index - 1];
    const auto& b = g_waypoints[g_wp_index];
    float abx = b.x - a.x, aby = b.y - a.y;
    float len2 = abx * abx + aby * aby;
    float t = len2 > 1.0f ? ((pp.x - a.x) * abx + (pp.y - a.y) * aby) / len2 : 1.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float curX = a.x + abx * t, curY = a.y + aby * t;
    size_t next = g_wp_index;
    float nx = b.x, ny = b.y;
    float remaining = kLookAhead;
    for (int guard = 0; guard < 64; ++guard) {
        float dx = nx - curX, dy = ny - curY;
        float seg = std::sqrt(dx * dx + dy * dy);
        bool last = next + 1 >= g_waypoints.size();
        if (remaining <= seg || last) {
            float f = seg > 1.0f ? remaining / seg : 1.0f;
            if (f > 1.0f) f = 1.0f;
            return { curX + dx * f, curY + dy * f, g_waypoints[next].z };
        }
        const auto& on = g_waypoints[next + 1];
        float ox = on.x - nx, oy = on.y - ny;
        float olen = std::sqrt(ox * ox + oy * oy);
        if (seg > 1.0f && olen > 1.0f && (dx * ox + dy * oy) / (seg * olen) < kSharpCornerCos)
            return g_waypoints[next];               // stop at a sharp corner
        remaining -= seg;
        curX = nx; curY = ny;
        ++next;
        nx = on.x; ny = on.y;
    }
    return goal;
}

// Steer toward the goal. FIRST person: mouse-free STRAFE — slide toward the goal
// with an 8-way W/A/S/D combo, never turning the camera (movement is camera-
// relative and rotZ = camera in first person, so the combo stays correct as we
// slide). THIRD person: the camera orbits independently of movement, so fall
// back to mouse-turn + forward. Returns |relative bearing|.
float Steer(const game::Vec3& goal, const game::Vec3* corner = nullptr);

// Sidestep an obstacle WITHOUT dropping back to key injection.
//
// The recovery used to strafe by holding A or D, and that is why the walk went
// quiet mid-step: forward motion is driven by the engine's own movement update,
// which brings the walk animation and with it the game's real footstep sounds,
// while a key-held strafe moves the player with none of that. You heard the
// footsteps stop while the distance kept changing — two different mechanisms
// taking turns. Steering to a point out to the side keeps one mechanism in
// charge of everything.
void SidestepNative(int dir, const game::Vec3& goal, bool backOut)
{
    auto pp = game::GetPlayerPosition();
    float dx = goal.x - pp.x, dy = goal.y - pp.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) { dx = 0.0f; dy = 1.0f; len = 1.0f; }
    dx /= len; dy /= len;
    // Perpendicular to the line to the target, plus a lead along it: forward
    // normally, BACKWARD when we are wedged and need to get out first.
    const float kSide = 220.0f;
    const float lead  = backOut ? -140.0f : 90.0f;
    game::Vec3 aside{ pp.x + (-dy * dir) * kSide + dx * lead,
                      pp.y + ( dx * dir) * kSide + dy * lead,
                      pp.z };
    Steer(aside);
}

float Steer(const game::Vec3& goal, const game::Vec3* corner)
{
    auto  pp  = game::GetPlayerPosition();
    float rel = game::ComputeBearing(pp, game::GetPlayerYaw(), goal).relative_yaw;

    // Preferred: drive the engine's movement update directly (see player_mover).
    // No keys, exact speed, and the turn slows the walk instead of skidding
    // through it. Falls through to the key-holding path when the hook isn't in
    // place or [Voice] NativeWalk is off, so walking always works somehow.
    if (config::Get().native_walk) {
        if (mover::Available()) {
            Move(false, false, false, false);      // make sure no key is stuck down
            if (corner)
                mover::SetGoalWithCorner(goal.x, goal.y, /*run*/true, corner->x, corner->y);
            else
                mover::SetGoal(goal.x, goal.y, /*run*/true);
            return std::fabs(rel);
        }
        WarnNoMover();      // say so, and steer nowhere rather than press keys
        return std::fabs(rel);
    }
    mover::Clear();

    if (game::IsThirdPerson()) {
        TurnMouse(rel);
        Forward(true);
        return std::fabs(rel);
    }

    // Decompose the bearing into an 8-way key combo (no turning).
    float a = std::fabs(rel);
    bool w = a < 67.5f;                    // goal ahead-ish
    bool s = a > 112.5f;                   // goal behind-ish
    bool right = rel >  22.5f && rel <  157.5f;
    bool left  = rel < -22.5f && rel > -157.5f;
    Move(w, left, s, right);
    return a;
}

// Starting a walk drops any aim lock: both steer the player's heading, and an
// aim that keeps yanking the view back at a target is not something you can
// walk away from.
void DropAimLock() { poll::StopAimTrack(); }

// On arrival, turn the view onto the target, as the FNV mod does: the middle
// of its body, measured from the camera, held for a moment so the engine's
// own view update does not undo it. The crosshair is then on it for E.
constexpr int kLookAtFrames = 30;

void LookAtTarget()
{
    if (!g_target_refr || !g_target_form) return;
    game::Vec3 at{};
    if (!game::GetRefPosition(g_target_refr, g_target_form, &at)) return;   // gone
    game::Vec3 body{};
    if (game::GetAimPointFor(g_target_refr, &body, nullptr)) at = body;
    poll::RequestAimTrack(g_target_refr, g_target_form, at, 0.0f, kLookAtFrames);
}

void StopWalking(const char* reason_utf8);   // defined below

// The walk is over at the target: say so and face it.
void Arrive(const char* say)
{
    StopWalking(say);
    LookAtTarget();
}

// Steer along the route: at the look-ahead point, braking for the real corner.
float SteerRoute()
{
    game::Vec3 corner{};
    game::Vec3 goal = LookAheadGoal(&corner);
    return Steer(goal, &corner);
}

void StopWalking(const char* reason_utf8)
{
    if (g_state == State::Idle) return;
    {
        // Logged because "it walked past the target and kept going" cannot be
        // diagnosed from the outside: we need to see whether arrival was
        // detected at all, and from how far out.
        float d  = DistanceToTarget();
        float dz = g_target_pos.z - game::GetPlayerPosition().z;
        F3A_INFO("autowalk stop: %s | dist=%.0f dz=%.0f path=%d wp=%u/%u",
                 reason_utf8 ? reason_utf8 : "(cicho)", d, dz,
                 g_have_path ? 1 : 0, (unsigned)g_wp_index,
                 (unsigned)g_waypoints.size());
    }
    // Measured, not assumed, and measured BEFORE we stop anything: the first
    // version of this line read mover::Active() after mover::Clear() had
    // already cleared it, so it reported 0 every single time and told us
    // nothing about the state we were trying to diagnose.
    F3A_INFO("autowalk stop state (before stopping): keys_allowed=%d W_down=%d "
             "mover_active=%d",
             (int)KeysAllowed(),
             (GetAsyncKeyState('W') & 0x8000) ? 1 : 0,
             (int)mover::Active());
    mover::Clear();       // stop driving the engine's mover
    mover::CancelEnginePath();
    ReleaseKeysHard();    // and let go of every movement key, edge or no edge
    g_jump_timer    = 0.0f;
    g_close_timer   = 0.0f;
    g_recover_timer = 0.0f;
    g_recover_tries = 0;
    g_stuck_count   = 0;
    g_door_hold     = 0.0f;
    g_best_dist = 1e9f;
    g_noimprove = 0;
    g_cross         = false;
    g_cross_wait    = 0.0f;
    g_cross_door_refr = nullptr;
    g_cross_used_doors.clear();   // a new walk gets a clean map of the building
    g_engine_wait    = 0.0f;
    g_route_engine   = false;
    g_route_partial  = false;
    g_cross_engine   = false;
    g_state = State::Idle;
    if (reason_utf8 && *reason_utf8)
        tolk::Speak(reason_utf8, tolk::Priority::System, true);
}


// When blocked, a closed door is the usual culprit. Open it ourselves — the
// same native Activate the End key uses, on the game's thread — hold still
// while it swings, check it really opened, and walk on.
//
// Handing the door to the player instead (the previous behaviour) stranded a
// blind player in the vault: the walk stopped "facing" a door it had not
// actually turned to, the interaction key hit nothing, and there was no way
// forward. What made opening it ourselves go wrong before is now ruled out
// before we touch anything:
//   * a LOCKED door is never activated (that would open the lockpick screen
//     mid-walk) — the walk stops and says which lock it is;
//   * a LOAD door is never activated (that would carry the player off to
//     another place) — the walk goes round it like any obstacle;
//   * a door that is already OPEN, or whose state can't be read, is not
//     toggled — activating it would shut it;
//   * the walk stands still while the door swings instead of shoving into it,
//     and stops with a clear sentence if the door did not open.
// Each door is tried once per walk. Returns true if it took the matter in hand.
bool TryOpenBlockingDoor()
{
    auto  pp  = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto ents = game::ScanNearby((int)kDoorReach, 24, /*actors_only*/false, false);   // 24: a cluttered room must not crowd the door out
    const game::WorldEntity* best = nullptr;
    float bestd = kDoorStopReach * kDoorStopReach;
    for (const auto& e : ents) {
        if (e.kind != game::WorldEntity::Kind::Door || !e.refr || !e.form_id)
            continue;
        if (e.load_door != 0) continue;     // a way somewhere else, not a door to open
        if (e.open_state == 1) continue;    // already open: not what blocks us
        // It has to be IN FRONT of us, not merely nearby.
        auto br = game::ComputeBearing(pp, yaw, e.position);
        if (std::fabs(br.relative_yaw) > kDoorStopCone) continue;
        float dx = e.position.x - pp.x, dy = e.position.y - pp.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestd) { bestd = d2; best = &e; }
    }
    if (!best) return false;
    // Only once per door per walk: if the SAME door blocks us again it is not
    // the problem, so fall through to the ordinary stuck handling.
    if (best->form_id == g_last_door) return false;
    g_last_door = best->form_id;
    float dist = std::sqrt(bestd);

    if (best->locked) {
        // A locked door on the route is not the end of the route: the navmesh
        // runs through doorways whether or not they open, and often there is
        // another way round (the reactor level on the way to Jonas). Close the
        // doorway off and look for one before giving up.
        g_avoid.push_back(best->position);
        if (!g_cross && RebuildPath(nullptr)) {
            F3A_INFO("autowalk: locked door '%s' (%08X) — routing round it",
                     best->name.c_str(), (unsigned)best->form_id);
            tolk::Speak("Drzwi zamknięte na zamek. Szukam innej drogi.",
                        tolk::Priority::Ui, false);
            g_last_probe_dist = DistanceToTarget();
            g_stuck_count = 0;
            return true;
        }
        F3A_INFO("autowalk: locked door '%s' (%08X) lock=%d, %.0f units — stopping",
                 best->name.c_str(), (unsigned)best->form_id, best->lock_level, dist);
        std::string what = "Zamknięte drzwi na drodze: " + best->name + ", ";
        int L = best->lock_level;
        if (L >= 250 || L < 0) what += "potrzebny klucz.";
        else what += std::string("zamek ") +
                     ((L < 25) ? "bardzo łatwy" : (L < 50) ? "łatwy" :
                      (L < 75) ? "średni" : (L < 100) ? "trudny" : "bardzo trudny") + ".";
        const void* r = best->refr; uint32_t f = best->form_id;
        StopWalking(what.c_str());
        game::Vec3 at{};
        if (game::GetRefPosition(r, f, &at)) poll::RequestFacePoint(at);
        return true;
    }
    if (best->open_state != 0) {
        // Can't tell whether it is open: don't risk shutting it. Stuck handling
        // goes round instead, and logs this so a pattern would show.
        F3A_INFO("autowalk: door '%s' (%08X) state unknown — not touching it",
                 best->name.c_str(), (unsigned)best->form_id);
        return false;
    }

    F3A_INFO("autowalk: opening the door '%s' (%08X), %.0f units",
             best->name.c_str(), (unsigned)best->form_id, dist);
    game::QueueActivate(best->refr, best->form_id);
    // Stop ONCE, here; the hold below only waits. (The mover's stop is a full
    // brake with an idle animation — not something to repeat every tick.)
    mover::Clear();
    Move(false, false, false, false);
    g_door_refr = best->refr;
    g_door_form = best->form_id;
    g_door_name = best->name;
    g_door_hold = kDoorHoldSecs;
    g_door_checks = 2;
    g_door_cooldown = 2.0f * kDoorHoldSecs + kDoorGraceSecs;
    tolk::Speak("Otwieram drzwi.", tolk::Priority::Background, false);
    return true;
}

// Called every tick while a door we opened is swinging: stand still, then
// check it opened. Returns true while the walk must not move.
bool DoorHoldTick(float dt)
{
    if (g_door_hold <= 0.0f) return false;
    g_door_hold -= dt;   // standing still: nothing steers until it is done
    if (g_door_hold > 0.0f) return true;

    int st = game::DoorOpenState(g_door_refr, g_door_form);
    F3A_INFO("autowalk: door '%s' after opening: state=%d (1=open 0=closed -1=?)",
             g_door_name.c_str(), st);
    if (st == 0 && --g_door_checks > 0) {
        g_door_hold = kDoorHoldSecs;          // a slow door: look once more
        return true;
    }
    if (st == 0) {
        std::string what = "Drzwi się nie otworzyły: " + g_door_name + ".";
        const void* r = g_door_refr; uint32_t f = g_door_form;
        StopWalking(what.c_str());
        game::Vec3 at{};
        if (game::GetRefPosition(r, f, &at)) poll::RequestFacePoint(at);
        return true;
    }
    g_last_probe_dist = DistanceToTarget();   // fresh progress base past the door
    return false;
}

// When forward progress stalls, identify WHAT is most likely blocking us and say
// it — so the player isn't just told "obstacle" but "a raider is in the way" or
// "this is terrain". No raycasts, so we scan a forward CONE and name the nearest
// solid reference in it. Static world geometry (walls, rocks, cliffs) isn't a
// scannable reference, so "nothing solid in the cone" == a terrain obstacle.
// Items/notes/quest markers don't physically block, so they're skipped.
std::string DescribeObstacle()
{
    auto  pp   = game::GetPlayerPosition();
    float yaw  = game::GetPlayerYaw();
    auto  ents = game::ScanNearby(320, 12, /*actors_only*/false, false);
    const game::WorldEntity* best = nullptr;
    float bestd = 1e9f;
    for (const auto& e : ents) {
        if (!e.refr) continue;
        if (e.kind == game::WorldEntity::Kind::Item ||
            e.kind == game::WorldEntity::Kind::Note ||
            e.kind == game::WorldEntity::Kind::Quest)
            continue;                                   // these don't block a walk
        auto br = game::ComputeBearing(pp, yaw, e.position);
        if (std::fabs(br.relative_yaw) > 55.0f) continue;   // not roughly ahead
        if (br.distance > 300.0f)              continue;    // too far to be it
        if (br.distance < bestd) { bestd = br.distance; best = &e; }
    }
    if (!best)
        return "Przeszkoda terenowa — ściana lub skała. Omijam.";
    switch (best->kind) {
        case game::WorldEntity::Kind::Actor:
            return (best->hostile ? "Wróg na drodze: " : "Ktoś na drodze: ")
                   + best->name + ". Omijam.";
        case game::WorldEntity::Kind::Door:
            // A door still in the cone here is one we already handed to the
            // player (we stop at each door only once per walk), so if we are
            // still stuck on it, it is locked or blocked rather than just shut.
            return "Drzwi zablokowane lub zamknięte: " + best->name + ".";
        case game::WorldEntity::Kind::Container:
            return "Blokuje pojemnik: " + best->name + ". Omijam.";
        default:
            return "Przeszkoda: " + best->name + ". Omijam.";
    }
}

// Choose the next load door to head for while crossing worldspaces. Doors are
// ranked in two categories:
//   2 — leads straight into the target's space (the door we ultimately want)
//   1 — leads OUT to an exterior worldspace (progress toward the open world,
//       where a distant target building lives)
// A door that leads into some OTHER interior (a random building — Craterside,
// a house) is NEVER chosen: that was the "walks into the wrong shop" bug. We
// also skip the door back to where we came from, and doors that lead into the
// space we're already standing in (no progress). Higher category wins, then the
// nearest. Returns false when nothing qualifies — the caller then teleports,
// which is the reliable way to reach a target whose entrance isn't nearby (e.g.
// an item deep inside a building we haven't walked up to yet).
bool PickCrossDoor()
{
    auto pp   = game::GetPlayerPosition();
    // Many entries: in a furnished vault the 48 nearest things were all
    // chairs and lockers, and the one door that led on was not among them —
    // so the walk set off through a door back the way it came.
    auto ents = game::ScanNearby(kCrossScan, 400, /*actors_only*/false, false);
    const void* hereCell = game::GetPlayerCell();
    const bool  indoors  = game::PlayerIsInInterior();
    const game::WorldEntity* best = nullptr;
    int   bestCat = 0;
    float bestd   = 0.0f;
    for (const auto& e : ents) {
        if (e.kind != game::WorldEntity::Kind::Door || !e.refr || !e.form_id)
            continue;
        if (!game::IsLoadDoor(e.refr)) continue;
        // Don't go back the way we came, and don't loop into our current space.
        if (g_cross_from_cell &&
            game::DoorLeadsToCellSpace(e.refr, g_cross_from_cell)) continue;
        if (hereCell && game::DoorLeadsToCellSpace(e.refr, hereCell)) continue;

        // Ranking, best first. The old rule was "a door into another interior
        // is never a good pick", which is true in the wasteland and wrong
        // underground: Vault 101 is a chain of interiors, so in the prologue
        // EVERY usable door was rejected, the pick came back empty, and the
        // walk gave up a few rooms from the overseer's office.
        //
        // Doors already used this walk rank last rather than being banned, so a
        // dead end can be backed out of instead of ending the route.
        bool used = false;
        for (uint32_t id : g_cross_used_doors) if (id == e.form_id) { used = true; break; }

        int cat;
        if (game::DoorLeadsToTargetSpace(e.refr, g_target_refr))    cat = 4;
        else if (game::DoorLeadsToExterior(e.refr))
            cat = g_target_indoors ? 2 : 3;   // outside only helps if that is where we are going
        else if (indoors)                                           cat = 2;
        else continue;   // outdoors, a door further inside is not the way on
        if (used) cat = 1;
        // A locked door opens only with its key or a lockpick screen — the
        // last resort, after every door that simply opens.
        if (e.locked) cat = 0;

        float dx = e.position.x - pp.x, dy = e.position.y - pp.y;
        float d2 = dx * dx + dy * dy;
        bool better = !best || cat > bestCat || (cat == bestCat && d2 < bestd);
        if (better) { best = &e; bestCat = cat; bestd = d2; }
    }
    if (!best) {
        F3A_INFO("Autowalk: no usable door here (indoors=%d, target indoors=%d, "
                 "%d door(s) already used)", (int)indoors, (int)g_target_indoors,
                 (int)g_cross_used_doors.size());
        return false;
    }
    if (g_cross_used_doors.size() < 32) g_cross_used_doors.push_back(best->form_id);
    F3A_INFO("Autowalk: next door '%s' (rank %d)", best->name.c_str(), bestCat);
    // The orbit check measures the way to THIS door: the closest approach to
    // the previous one (often a few units) made every probe here look like no
    // improvement, and a walk of several doors stopped after ~8 s.
    g_best_dist = 1e9f;
    g_noimprove = 0;
    g_cross_door_refr = best->refr;
    g_cross_door_form = best->form_id;
    g_cross_door_pos  = best->position;
    g_cross_door_name = best->name;
    return true;
}

// Last resort when we can't route by doors (none reachable, wedged, or too many
// hops): teleport straight to the target reference (cell-aware native MoveTo, on
// the main thread). We stay in cross mode and wait out the load; the far-side
// check then either resumes normal walking (same space now) or, if teleport
// somehow didn't land us there, stops with a clear message.
// Out of options on foot. This used to teleport with the native MoveTo, but that
// call CRASHES the game (proven twice in play), and teleporting into a quest
// marker also skipped everything on the way there. So we stop honestly and say
// what to do instead: the world map's fast travel is the real way to cross the
// wasteland.
void CrossFallbackTeleport()
{
    mover::Clear();
    Move(false, false, false, false);
    // Indoors there IS no fast travel: an interior has no world map and no
    // marker to travel to, so sending the player to the map was advice they
    // could not act on — in the Vault 101 prologue, where the target is simply
    // a few doors away, it was the worst possible thing to say. Point at the
    // doors instead, which is what the route actually needs.
    if (game::PlayerIsInInterior()) {
        StopWalking("Nie mogę dojść pieszo. Cel jest w innej części tej lokacji — "
                    "poszukaj drzwi klawiszem X i przejdź przez nie.");
        return;
    }
    StopWalking("Nie mogę dojść pieszo. Otwórz mapę świata i użyj szybkiej podróży.");
}

// The load door the game's route runs through: the one nearest the point
// where that route leaves this area (its last point here).
bool PickDoorNear(const game::Vec3& at)
{
    auto ents = game::ScanNearby(kCrossScan, 400, /*actors_only*/false, false);
    const game::WorldEntity* best = nullptr;
    float bestd = 300.0f * 300.0f;
    for (const auto& e : ents) {
        if (e.kind != game::WorldEntity::Kind::Door || !e.refr || !e.form_id) continue;
        if (!game::IsLoadDoor(e.refr)) continue;
        float dx = e.position.x - at.x, dy = e.position.y - at.y;
        float d2 = dx * dx + dy * dy;
        if (!(std::fabs(e.position.z - at.z) <= 250.0f)) continue;
        if (d2 < bestd) { bestd = d2; best = &e; }
    }
    if (!best) return false;
    if (g_cross_used_doors.size() < 32) g_cross_used_doors.push_back(best->form_id);
    F3A_INFO("Autowalk: the game's route leaves through '%s' (%.0f units from its end)",
             best->name.c_str(), std::sqrt(bestd));
    g_cross_door_refr = best->refr;
    g_cross_door_form = best->form_id;
    g_cross_door_pos  = best->position;
    g_cross_door_name = best->name;
    g_best_dist = 1e9f;
    g_noimprove = 0;
    return true;
}

// Our own door ranking (PickCrossDoor) and the game's route to that door.
// False when there is no door at all (the walk has then been stopped).
bool CrossPickDoorOurselves()
{
    if (!PickCrossDoor()) { CrossFallbackTeleport(); return false; }
    RouteToDoor();
    return true;
}

// The next door to go through. The game's own pathfinder is asked first —
// for the TARGET itself: it routes across doors as it does for NPCs, and the
// door its route leads through is the one to take (TickCross picks it up
// when the answer comes). Our own ranking only when it has nothing.
bool CrossNextHop()
{
    g_cross_door_refr = nullptr;
    g_waypoints.clear(); g_wp_index = 0; g_have_path = false;
    g_route_engine = false; g_route_partial = false;
    if (g_target_refr && g_target_form &&
        RequestEngineRouteTo(g_target_refr, g_target_form, g_target_pos, kArriveDist)) {
        g_cross_engine = true;
        g_engine_wait  = kEngineWaitSecs;
        return true;
    }
    return CrossPickDoorOurselves();
}

// Begin (or restart) cross-worldspace routing: remember the cell we're leaving,
// then pick the first door. Teleports immediately if nothing connects.
void BeginCross()
{
    g_cross            = true;
    g_cross_from_cell  = game::GetPlayerCell();
    g_target_indoors   = g_target_refr ? game::RefIsInInterior(g_target_refr)
                                       : game::PlayerIsInInterior();
    g_cross_door_refr  = nullptr;
    g_cross_stuck      = 0;
    g_probe_timer      = 0.0f;
    if (!CrossNextHop()) return;
    if (!g_cross_door_refr) return;          // the game's answer names the door
    g_last_probe_dist = std::sqrt(
        (g_cross_door_pos.x - game::GetPlayerPosition().x) *
        (g_cross_door_pos.x - game::GetPlayerPosition().x) +
        (g_cross_door_pos.y - game::GetPlayerPosition().y) *
        (g_cross_door_pos.y - game::GetPlayerPosition().y));
    tolk::Speak("Prowadzę do przejścia: " + g_cross_door_name,
                tolk::Priority::Background, false);
}

// Cross-worldspace tick: walk to the chosen door, activate it, wait for the
// load, then re-evaluate. Runs instead of the normal steering while g_cross.
void TickCross(float dt)
{
    // A door activation / teleport is in progress: wait for the cell to change,
    // then settle, then evaluate the far side. (During the actual load screen
    // gameplay is inactive and this whole function is skipped, so these timers
    // only advance on real in-cell frames.)
    if (g_cross_wait > 0.0f) {
        Move(false, false, false, false);
        bool changed = game::GetPlayerCell() != g_cross_prev_cell;
        if (!changed) {
            // Cell hasn't changed yet. Either the load simply hasn't happened,
            // or the door did nothing (locked/blocked). Time out → fall back.
            g_cross_settle = 0.0f;
            g_cross_wait  -= dt;
            if (g_cross_wait <= 0.0f) {
                if (g_cross_teleported) {
                    StopWalking("Nie udało się dotrzeć do celu. Spróbuj ręcznie.");
                    return;
                }
                CrossFallbackTeleport();
            }
            return;
        }
        // Cell changed — let it settle a beat so position/scan reads are valid.
        g_cross_settle += dt;
        if (g_cross_settle < kCrossSettle) return;
        g_cross_wait = 0.0f;

        // Far side: are we finally in the target's coordinate frame? With a
        // reference we compare spaces; without one (quest markers often have no
        // reference) a successful navmesh path is the evidence that the target is
        // reachable from here.
        bool arrived_in_space = g_target_refr
            ? game::TargetSharesPlayerSpace(g_target_refr)
            : navmesh::BuildPath(game::GetPlayerPosition(), g_target_pos,
                                 g_waypoints) && !g_waypoints.empty();
        if (arrived_in_space) {
            g_cross = false;
            g_best_dist = 1e9f;
            g_noimprove = 0;
            g_waypoints.clear(); g_wp_index = 0;
            g_have_path = navmesh::BuildPath(game::GetPlayerPosition(),
                          g_target_pos, g_waypoints) && !g_waypoints.empty();
            g_route_engine  = false;
            g_route_partial = false;
            if (RequestEngineRoute()) g_engine_wait = kEngineWaitSecs;   // engine first
            g_last_probe_dist = DistanceToTarget();
            g_probe_timer = 0.0f;
            g_stuck_count = 0;
            tolk::Speak("Jesteś w tej samej lokacji co cel. Idę dalej.",
                        tolk::Priority::Background, false);
            return;
        }
        // Still elsewhere. If the teleport fallback already ran and even that
        // didn't land us in the target's space, give up cleanly.
        if (g_cross_teleported) {
            StopWalking("Nie udało się dotrzeć do celu. Spróbuj ręcznie.");
            return;
        }
        // Otherwise chain to the next door (bounded). Mark the cell we JUST left
        // (prev_cell) as "back", so PickCrossDoor won't send us straight back
        // through the door we arrived by.
        g_cross_from_cell = g_cross_prev_cell;
        if (++g_hop_count > kMaxHops) {
            CrossFallbackTeleport();
            return;
        }
        if (!CrossNextHop()) return;
        g_cross_stuck = 0;
        g_probe_timer = 0.0f;
        if (!g_cross_door_refr) return;      // the game's answer names the door
        g_last_probe_dist = std::sqrt(
            (g_cross_door_pos.x - game::GetPlayerPosition().x) *
            (g_cross_door_pos.x - game::GetPlayerPosition().x) +
            (g_cross_door_pos.y - game::GetPlayerPosition().y) *
            (g_cross_door_pos.y - game::GetPlayerPosition().y));
        tolk::Speak("Kolejne przejście: " + g_cross_door_name,
                    tolk::Priority::Background, false);
        return;
    }

    if (!g_cross_door_refr && g_cross_engine) {
        // The game has answered (or not in time) about a route to the TARGET.
        g_cross_engine = false;
        const bool engineRoute = g_route_engine && g_have_path && !g_waypoints.empty();
        if (engineRoute && !mover::EnginePathLeadsElsewhere() && !g_route_partial) {
            // It reaches the target without leaving this area: the cell
            // records disagreed, the geometry does not. Just walk it.
            F3A_INFO("autowalk: the game's route reaches the target here - walking it");
            g_cross = false;
            return;
        }
        if (engineRoute && mover::EnginePathLeadsElsewhere() &&
            PickDoorNear(g_waypoints.back())) {
            // Walk the game's route; at its end is the door it goes through.
            g_cross_stuck = 0;
            g_probe_timer = 0.0f;
            auto pp0 = game::GetPlayerPosition();
            float ddx = g_cross_door_pos.x - pp0.x, ddy = g_cross_door_pos.y - pp0.y;
            g_last_probe_dist = std::sqrt(ddx * ddx + ddy * ddy);
            tolk::Speak("Prowadzę do przejścia: " + g_cross_door_name,
                        tolk::Priority::Background, false);
        } else {
            F3A_INFO("autowalk: no route of the game's through a door - choosing one ourselves");
            if (!CrossPickDoorOurselves()) return;
            tolk::Speak("Prowadzę do przejścia: " + g_cross_door_name,
                        tolk::Priority::Background, false);
            return;
        }
    }
    if (!g_cross_door_refr) {                 // lost the door → repick or teleport
        if (!PickCrossDoor()) { CrossFallbackTeleport(); return; }
        RouteToDoor();
        return;
    }
    AdoptEngineRoute();
    g_engine_said_none = false;     // a door route's "none" means nothing for the target
    AdvanceWaypoints();

    auto  pp = game::GetPlayerPosition();
    float dx = g_cross_door_pos.x - pp.x, dy = g_cross_door_pos.y - pp.y;
    float dd = std::sqrt(dx * dx + dy * dy);

    if (dd <= kCrossDoorReach) {              // at the door → go through it
        game::QueueActivate(g_cross_door_refr, g_cross_door_form);
        mover::CancelEnginePath();
        g_waypoints.clear(); g_wp_index = 0; g_have_path = false;
        g_route_engine = false; g_route_partial = false;
        g_last_door       = g_cross_door_form;
        g_cross_door_refr = nullptr;
        g_cross_prev_cell = game::GetPlayerCell();
        g_cross_wait      = kCrossTimeout;    // wait for the cell to change
        g_cross_settle    = 0.0f;
        Move(false, false, false, false);
        tolk::Speak("Przechodzę: " + g_cross_door_name,
                    tolk::Priority::Background, false);
        return;
    }

    // Recovery strafe (same idea as the main loop) or steer at the door.
    if (g_recover_timer > 0.0f) {
        if (config::Get().native_walk)
            SidestepNative(g_recover_dir, g_cross_door_pos, /*backOut*/false);
        else
            Move(true, g_recover_dir < 0, false, g_recover_dir > 0);
        g_recover_timer -= dt;
        if (g_recover_timer <= 0.0f) g_last_probe_dist = dd;
    } else if (g_route_engine && g_have_path && g_wp_index < g_waypoints.size()) {
        SteerRoute();                         // the engine's way to the door
    } else {
        Steer(g_cross_door_pos);              // no route: straight at it
    }

    // A door we opened on the way is still swinging: not a stall.
    if (g_door_cooldown > 0.0f) {
        g_door_cooldown -= dt;
        if (g_door_cooldown <= 0.0f) g_last_probe_dist = dd;
        g_probe_timer = 0.0f;
        return;
    }

    // Progress probe toward the door: no progress → try a plain closed door in
    // the way, else count a stall; enough stalls → teleport fallback.
    g_probe_timer += dt;
    if (g_probe_timer >= kProbeEvery) {
        g_probe_timer = 0.0f;
        float progress = g_last_probe_dist - dd;
        g_last_probe_dist = dd;
        // On the engine's route the way to the door can lead away from it
        // first; there a stall means NOT MOVING, as in the FNV mod.
        auto ppNow = game::GetPlayerPosition();
        float mvx = ppNow.x - g_probe_pos.x, mvy = ppNow.y - g_probe_pos.y;
        g_probe_pos = ppNow;
        if (g_route_engine && g_have_path && g_wp_index < g_waypoints.size())
            progress = std::sqrt(mvx * mvx + mvy * mvy);
        // Circling: near the target but no longer getting nearer. The existing
        // backstop only covers 175 units for 2.5 s, so an orbit at any wider
        // radius went on forever. Alex's walker calls this "arrival at the
        // closest navmesh-reachable point", and that is the honest answer — the
        // walk has got as close as it is going to get, and saying so beats
        // pacing around a marker.
        if (dd < g_best_dist - kMinProgress) {
            g_best_dist = dd;
            g_noimprove = 0;
        } else if (g_best_dist < kOrbitRadius) {
            if (++g_noimprove >= kOrbitProbes) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "Bliżej nie dojdę: %s.",
                              g_target_name.c_str());
                StopWalking(buf);
                return;
            }
        }

        if (progress < kMinProgress && g_recover_timer <= 0.0f) {
            if (TryOpenBlockingDoor()) return;
            if (++g_cross_stuck >= kCrossMaxStuck) { CrossFallbackTeleport(); return; }
            // Same side again, for longer, until this side has had its chances.
            if (++g_recover_tries > kSideTries) {
                g_recover_dir   = -g_recover_dir;
                g_recover_tries = 1;
            }
            g_recover_timer = kRecoverSecs * (float)g_recover_tries;
            TriggerJump();
            // Logged, not spoken. Naming every crate and doorframe it squeezes
            // past turned a walk into a running commentary, and the player
            // cannot act on any of it — the mod is already going round.
            F3A_DEBUG("autowalk: %s", DescribeObstacle().c_str());
        } else if (progress >= kMinProgress) {
            g_cross_stuck   = 0;
            g_recover_tries = 0;   // it is getting through; stop escalating
        }
    }
}

} // namespace

bool IsWalking() { return g_state != State::Idle; }

// The current destination's ref/form/name — for Alt+Home to teleport to WHERE
// YOU'RE WALKING (not the scanner selection, which drifts as DLC auto-tracks
// other quests). Null/empty when idle.
const void*        TargetRefr()   { return g_state != State::Idle ? g_target_refr : nullptr; }
uint32_t           TargetFormId() { return g_state != State::Idle ? g_target_form : 0; }
const std::string& TargetName()   { return g_target_name; }

std::string DiagString()
{
    if (g_state == State::Idle) return std::string("autowalk: idle");
    auto pp = game::GetPlayerPosition();
    float dist = DistanceToTarget();
    float dz   = g_target_pos.z - pp.z;
    // Live re-attempt: does the navmesh find a path from here to the target NOW?
    std::vector<game::Vec3> wp;
    bool ok = navmesh::BuildPath(pp, g_target_pos, wp);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "autowalk: '%s' player=(%.0f,%.0f,%.0f) target=(%.0f,%.0f,%.0f) "
        "dist2D=%.0f dz=%.0f have_path=%d wp=%d/%d stuck=%d recover=%.1f "
        "| rebuild=%s wps=%d",
        g_target_name.c_str(), pp.x, pp.y, pp.z,
        g_target_pos.x, g_target_pos.y, g_target_pos.z,
        dist, dz, (int)g_have_path, (int)g_wp_index, (int)g_waypoints.size(),
        g_stuck_count, g_recover_timer,
        ok ? "OK" : "FAIL", (int)wp.size());

    // Cross-worldspace diagnostics: is the target in the player's frame, is the
    // routing engaged, and WHERE do player/target actually live? This is what
    // tells us if the "wrong door" loop is old-DLL behaviour or a real miss.
    char cross[640];
    std::snprintf(cross, sizeof(cross),
        "\n  cross=%d hop=%d cross_stuck=%d cross_wait=%.1f tp_fallback=%d "
        "door='%s'\n  target_refr=%p shares_space=%d\n  player_at: %s\n  target_at: %s",
        (int)g_cross, g_hop_count, g_cross_stuck, g_cross_wait,
        (int)g_cross_teleported, g_cross_door_name.c_str(),
        g_target_refr, (int)game::TargetSharesPlayerSpace(g_target_refr),
        game::GetCurrentLocationName().c_str(),
        game::GetRefSpaceLabel(g_target_refr).c_str());
    return std::string(buf) + cross;
}

namespace { bool TryTravelLeg(); }   // defined below, used by StartTo

void StartTo(const game::Vec3& pos, const std::string& name,
             const void* refr, uint32_t form_id)
{
    // Native walking with no hook = nothing may drive the player. Refuse out
    // loud instead of falling back to held keys: that fallback is what produced
    // a walk which carried on after "Zatrzymano".
    if (config::Get().native_walk && !mover::Available()) { WarnNoMover(); return; }
    DropAimLock();
    g_target_pos  = pos;
    g_target_name = name;
    g_target_refr = refr;
    g_target_form = form_id;

    g_last_probe_dist = DistanceToTarget();
    g_probe_timer    = 0.0f;
    g_recover_timer  = 0.0f;
    g_recover_tries  = 0;
    g_stuck_count    = 0;
    g_best_dist = 1e9f;
    g_noimprove = 0;
    g_recover_dir    = 1;
    g_last_door      = 0;
    g_door_cooldown  = 0.0f;
    g_lost_said      = false;
    g_door_hold      = 0.0f;
    g_door_refr      = nullptr;

    // Reset cross-worldspace routing state for the new trip.
    g_cross            = false;
    g_cross_door_refr  = nullptr;
    g_cross_wait       = 0.0f;
    g_cross_settle     = 0.0f;
    g_cross_prev_cell  = nullptr;
    g_hop_count        = 0;
    g_cross_stuck      = 0;
    g_cross_teleported = false;
    g_cross_from_cell  = nullptr;

    g_route_engine     = false;
    g_route_partial    = false;
    g_route_legs       = 0;
    g_engine_wait      = 0.0f;
    g_engine_given_up  = false;
    g_engine_said_none = false;
    g_cross_engine     = false;
    g_probe_pos        = game::GetPlayerPosition();

    g_path_retry_timer = 0.0f;
    g_floor_warned     = false;
    g_follow_quest     = false;   // a plain target; StartToQuest re-sets this
    g_state            = State::Walking;

    bool shares = game::TargetSharesPlayerSpace(refr);
    F3A_INFO("autowalk StartTo '%s' refr=%p shares_space=%d cross=%d | player_at:%s | target_at:%s",
             name.c_str(), refr, (int)shares, (int)(refr && !shares),
             game::GetCurrentLocationName().c_str(),
             game::GetRefSpaceLabel(refr).c_str());
    // Ask the engine, once per walk, whether it already has a path for us.
    mover::ProbePath();

    // A "travel leg" (hop to the nearest known marker, walk the rest) used to run
    // here. It relied on the native MoveTo, which CRASHES the game — so the hop
    // is disabled until fast travel can be driven through the map menu instead.
    // TryTravelLeg is kept for that rework; it is not called.

    // Try a real path first: whether the target is even reachable on foot is the
    // evidence we need below when there's no reference to test the space with.
    g_avoid.clear();
    g_rebuild_cooldown = 0.0f;
    g_waypoints.clear();
    g_wp_index  = 0;
    g_have_path = navmesh::BuildPath(game::GetPlayerPosition(), pos,
                                     g_waypoints) && !g_waypoints.empty();

    // Target in ANOTHER coordinate frame (another cell/worldspace)? We can't walk
    // straight at its coordinates — route through the load doors that connect us.
    // This is the "led me to Moriarty's saloon" case: the Galaxy News Radio
    // marker lives in the Wasteland while the player stands in Megaton, so its
    // coordinates mean something else entirely here and walking "toward" them
    // heads off across town.
    //
    // With a reference we can compare spaces exactly. QUEST MARKERS OFTEN HAVE
    // NONE, and the old check bailed out in that case and walked the raw
    // coordinates anyway — which is precisely how the Moriarty walk happened. So
    // when there's no reference, treat "no navmesh path AND far away" as the same
    // situation: the navmesh here cannot reach it, so it is not here.
    bool other_space = refr ? !shares
                            : (!g_have_path && DistanceToTarget() > kNoPathFarDist);

    // A REAL PATH OUTRANKS THE CELL COMPARISON.
    //
    // Comparing cells was only ever a proxy for the real question, "can I walk
    // there?", and in the vault it gives the wrong answer: the overseer's
    // office is recorded in "Krypta 101 - przedsionek" while the player stands
    // in "Krypta 101", so the two do not share a cell — and the walk refused,
    // told the player to go and find a door, and threw away the 125-waypoint
    // path the navmesh had JUST BUILT across both of them. Interior cells of
    // one building share a navmesh; the records disagree, the geometry does not.
    //
    // So when the mesh produces a path that ENDS NEAR THE TARGET, that path is
    // the evidence and the cell records are overruled. The end-of-path test is
    // what keeps the old Moriarty bug away: a target in another worldspace has
    // coordinates that mean something else here, so any path toward them ends
    // nowhere near, and we still route by doors.
    if (other_space && g_have_path && !g_waypoints.empty()) {
        const auto& end = g_waypoints.back();
        float ex = pos.x - end.x, ey = pos.y - end.y, ez = pos.z - end.z;
        float gap = std::sqrt(ex * ex + ey * ey + ez * ez);
        F3A_INFO("autowalk: cells disagree, but the mesh has a %u-waypoint "
                 "path ending %.0f units from the target.",
                 (unsigned)g_waypoints.size(), gap);
        if (gap <= kPathEndsNearTarget) {
            other_space = false;
            F3A_INFO("autowalk: walking it — a path this close is a path there.");
        }
    }

    if (other_space) {
        tolk::Speak("Cel w innej lokacji: " + name +
                    ". Prowadzę przez przejścia.",
                    tolk::Priority::System, true);
        BeginCross();
        return;
    }

    // The engine's route first: stand for a moment (Tick) while it answers,
    // and walk ours only if it has none. Without a reference to route to, ours
    // straight away.
    if (RequestEngineRoute()) {
        g_engine_wait = kEngineWaitSecs;
        g_route_legs  = 1;
    } else {
        SteerRoute();
    }

    tolk::Speak("Idę do: " + name, tolk::Priority::System, true);
}

namespace {
// A destination on the far side of the wasteland is not something a sighted
// player walks to — they fast-travel to the nearest map marker they already know
// and walk the last stretch. Do the same: if the target is far and a DISCOVERED
// marker sits much closer to it, travel there and let the normal walk take over
// (the cross-worldspace state machine below waits out the load and resumes).
// False when there's nothing worth travelling to — the caller then walks/routes
// as before. Needs the player to have opened the map at least once, since that's
// where the marker list comes from.
constexpr float kFarTravelDist    = 6000.0f;   // below this, just walk
constexpr float kTravelMinSaving  = 2000.0f;   // a leg must save at least this

bool TryTravelLeg()
{
    const auto& markers = game::CachedMapMarkers();
    if (markers.empty()) return false;

    auto pp = game::GetPlayerPosition();
    float pdx = g_target_pos.x - pp.x, pdy = g_target_pos.y - pp.y;
    float playerToTarget = std::sqrt(pdx * pdx + pdy * pdy);
    if (playerToTarget < kFarTravelDist) return false;

    const game::MapMarker* best = nullptr;
    float bestDist = playerToTarget - kTravelMinSaving;   // must beat this
    for (const auto& m : markers) {
        if (!m.discovered || !m.refr) continue;
        float dx = g_target_pos.x - m.pos.x, dy = g_target_pos.y - m.pos.y;
        float d  = std::sqrt(dx * dx + dy * dy);
        if (d < bestDist) { bestDist = d; best = &m; }
    }
    if (!best) return false;

    F3A_INFO("autowalk: travel leg to '%s' (%.0f from target, was %.0f)",
             best->name.c_str(), bestDist, playerToTarget);
    tolk::Speak("Szybka podróż do: " + best->name + ", dalej pieszo.",
                tolk::Priority::System, true);

    Move(false, false, false, false);
    poll::RequestTeleport(best->refr, best->form_id);
    // Reuse the cross-space machine to wait out the load and re-evaluate on the
    // far side: it either resumes the plain walk (same space as the target now)
    // or picks the load door that gets us in.
    g_cross            = true;
    g_cross_teleported = false;      // a planned leg, not the give-up teleport
    g_cross_from_cell  = game::GetPlayerCell();
    g_cross_prev_cell  = game::GetPlayerCell();
    g_cross_door_refr  = nullptr;
    g_cross_wait       = kCrossTimeout;
    g_cross_settle     = 0.0f;
    return true;
}
} // namespace

// The live quest marker as a walk target. False when the tracked objective has
// no map marker (nothing to walk to).
bool StartToQuest()
{
    auto qt = game::GetCurrentQuestTarget();
    if (!qt.valid) return false;
    if (qt.position.x == 0.0f && qt.position.y == 0.0f && qt.position.z == 0.0f)
        return false;
    std::string label = qt.name;
    std::string q = game::GetTrackedQuestName();
    if (!q.empty()) label = q + ": " + qt.name;
    StartTo(qt.position, label, qt.refr, qt.refid);
    g_follow_quest = true;          // StartTo cleared it
    return true;
}

void Stop()
{
    // The player pressing the stop key means STOP, even when our own state
    // machine already believes the walk is over — and especially then, because
    // "the mod says it stopped and I am still walking" is exactly the case
    // where the two disagree. StopWalking returns early when it thinks it is
    // idle, so the engine-side stop goes first and unconditionally.
    mover::Clear();
    ReleaseKeysHard();
    StopWalking("Zatrzymano.");
}

namespace {
// Switch to the objective's current marker if it has moved meaningfully. Runs a
// full StartTo so the new target gets fresh cross-worldspace routing — the new
// marker may well be in another cell. Returns true if we retargeted (the caller
// should end the tick; the state was just rebuilt).
bool FollowQuestRetarget()
{
    if (!g_follow_quest) return false;
    auto qt = game::GetCurrentQuestTarget();
    if (!qt.valid) return false;
    if (qt.position.x == 0.0f && qt.position.y == 0.0f && qt.position.z == 0.0f)
        return false;
    float dx = qt.position.x - g_target_pos.x;
    float dy = qt.position.y - g_target_pos.y;
    float dz = qt.position.z - g_target_pos.z;
    if (dx * dx + dy * dy + dz * dz <= 200.0f * 200.0f) return false;  // same marker

    std::string label = qt.name;
    std::string q = game::GetTrackedQuestName();
    if (!q.empty()) label = q + ": " + qt.name;
    tolk::Speak("Cel zadania zmieniony.", tolk::Priority::System, true);
    StartTo(qt.position, label, qt.refr, qt.refid);
    g_follow_quest = true;
    return true;
}
} // namespace

void Tick(float dt)
{
    if (g_state == State::Idle) return;

    // Any menu (other than the HUD) or leaving gameplay cancels the walk —
    // EXCEPT while crossing worldspaces, where a load screen (gameplay briefly
    // inactive, a LoadingMenu on top) is exactly what we triggered by walking
    // through a door. Wait it out instead of aborting the trip; the post-load
    // wait timer in TickCross only ticks down once gameplay resumes.
    // Note the mover::Clear() on the cross-worldspace paths. The TRIP stays
    // alive across a load screen, but the walking must not: the mover keeps
    // pushing toward whatever goal it was last given, and after a load that goal
    // is a stale position in the worldspace we just left — which is how the
    // player ended up strolling off on their own with no walk apparently
    // running. Steering re-states the goal as soon as the walk resumes.
    if (!poll::IsGameplayActive()) {
        if (g_cross) { mover::Clear(); return; }   // loading between cells
        StopWalking(nullptr);
        return;
    }
    auto active = menu::ActiveMenu();
    if (active != menu::Id::None && active != menu::Id::HUDMain) {
        // Only the load a door of ours triggers keeps a crossing alive. Any
        // other menu (Pip-Boy, a lockpick screen at a locked door, pause)
        // ends it, as it ends an ordinary walk — before, the crossing waited
        // under it and walked off on its own the moment it closed.
        if (g_cross && (active == menu::Id::Loading || g_cross_wait > 0.0f)) {
            mover::Clear();
            return;
        }
        StopWalking(nullptr);
        return;
    }

    // Release a recovery-jump once its brief hold elapses (both modes use it).
    if (g_kjump) { g_jump_timer -= dt; if (g_jump_timer <= 0.0f) SetJump(false); }

    // A door we opened is still swinging: stand still until it is out of the way.
    if (DoorHoldTick(dt)) return;

    // Quest walks follow the LIVE objective: if it advanced (or another quest
    // took the tracked slot), switch targets mid-walk instead of marching to a
    // marker that no longer matters.
    if (FollowQuestRetarget()) return;

    // Waiting for the engine's route (start of a walk, the next leg of a long
    // one, a door in cross mode): stand for a moment rather than set off along
    // a route that is about to be replaced. If it is slow, ours takes over —
    // and its answer is still adopted when it comes.
    if (g_engine_wait > 0.0f) {
        AdoptEngineRoute();                  // clears g_engine_wait on an answer
        if (g_engine_wait > 0.0f) {
            g_engine_wait -= dt;
            g_probe_timer = 0.0f;            // standing on purpose is no stall
            g_probe_pos   = game::GetPlayerPosition();
            if (g_engine_wait > 0.0f) return;
            F3A_INFO("autowalk: the engine has not answered yet - %s",
                     g_have_path ? "walking our own route meanwhile" : "no route yet");
        }
    }

    // Cross-worldspace routing runs its own steering (walk to a door, pass
    // through, re-evaluate) instead of the straight-line/navmesh walk below.
    if (g_cross) { TickCross(dt); return; }

    float dist = DistanceToTarget();                       // horizontal only
    float dz   = g_target_pos.z - game::GetPlayerPosition().z;
    bool  sameFloor = std::fabs(dz) <= kFloorDelta;

    // Arrival requires being on the SAME floor too — DistanceToTarget is 2D, so
    // without this we'd falsely "arrive" while standing directly under/over a
    // target on another floor.
    if (dist <= kArriveDist && sameFloor) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Dotarłeś: %s", g_target_name.c_str());
        Arrive(buf);
        return;
    }

    // A quest marker ends the walk at arm's length of the AREA, always.
    // Markers are placed for the map, not for walking into: this one sits on a
    // rifle range you are meant to shoot from, and the walk kept shuffling
    // around it long after the player was standing exactly where the game
    // wanted them. Unconditional on purpose — no floor test, no path test.
    if (g_follow_quest && config::Get().quest_arrive_dist > 0 &&
        dist <= (float)config::Get().quest_arrive_dist) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Jesteś w rejonie celu: %s",
                      g_target_name.c_str());
        Arrive(buf);
        return;
    }

    // ...and the backstop, which does not care about the floor.
    if (dist <= kNearArrive) {
        g_close_timer += dt;
        if (g_close_timer >= kCloseArriveSecs) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "Jesteś przy celu: %s",
                          g_target_name.c_str());
            Arrive(buf);
            return;
        }
    } else {
        g_close_timer = 0.0f;
    }

    // Different floor and no path yet: DON'T bail — go anyway. Keep walking and
    // retry BuildPath as we move; once we reach the stairs / a connected mesh
    // region the path is found and handles the height. Warn once so the player
    // knows why it's wandering. Stuck-detection below gives up gracefully if the
    // target is truly unreachable on foot.
    if (!g_have_path) {
        g_path_retry_timer += dt;
        if (g_path_retry_timer >= kPathRetryEvery) {
            g_path_retry_timer = 0.0f;
            g_waypoints.clear(); g_wp_index = 0;
            if (navmesh::BuildPath(game::GetPlayerPosition(), g_target_pos,
                                   g_waypoints, &g_avoid) && !g_waypoints.empty()) {
                g_have_path = true;
                g_route_engine  = false;
                g_route_partial = false;
                g_last_probe_dist = dist;   // fresh progress base for the new route
                tolk::Speak("Trasa wyznaczona.", tolk::Priority::Background, false);
            }
            // And the engine again, unless it is already working on one.
            if (mover::EnginePathState() != 1) RequestEngineRoute();
        }
        if (!g_floor_warned && !sameFloor) {
            g_floor_warned = true;
            tolk::Speak(dz > 0 ? "Cel wyżej — szukam drogi po schodach."
                               : "Cel niżej — szukam drogi po schodach.",
                        tolk::Priority::Background, false);
        }
    }

    AdoptEngineRoute();
    AdvanceWaypoints();

    // The engine's route ended short of the target — it routes only as far as
    // the navmesh it has loaded. Ask for the next leg from here, as the FNV mod
    // does, while each leg still gains real ground; otherwise walk ours.
    if (g_route_engine && g_route_partial && g_have_path &&
        g_wp_index >= g_waypoints.size()) {
        g_route_partial = false;
        float gain = g_leg_start_dist - DistanceToRouteGoal();
        if (gain > kLegMinGain && g_route_legs < kMaxRouteLegs && RequestEngineRoute()) {
            ++g_route_legs;
            F3A_INFO("autowalk: engine leg %d ended short (gained %.0f) - asking for the next",
                     g_route_legs, gain);
            mover::Clear();
            g_engine_wait = kEngineWaitSecs;
            return;
        }
        F3A_INFO("autowalk: engine leg ended short and gained %.0f - our own route from here",
                 gain);
        g_engine_given_up = true;
        g_route_engine    = false;
        if (!RebuildPath(nullptr)) {
            // Neither the engine nor our mesh goes any further: this is as
            // close as walking gets — a rifle-range target behind its
            // counter, a marker on a roof. Stop here and face it (the FNV
            // mod's "closest reachable point"), instead of pushing on into
            // the counter.
            char buf[192];
            std::snprintf(buf, sizeof(buf), "Bliżej nie dojdę: %s.", g_target_name.c_str());
            Arrive(buf);
            return;
        }
    }

    // The engine says there is no route, our mesh has none either, and the
    // target is close: it is out of reach from here (behind a counter, up on
    // a ledge). Stop and face it rather than walk at it in a straight line.
    if (g_engine_said_none) {
        g_engine_said_none = false;
        if (!g_have_path && DistanceToTarget() <= 600.0f) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "Bliżej nie dojdę: %s.", g_target_name.c_str());
            Arrive(buf);
            return;
        }
    }

    // Lost the route? Far from the waypoint we are meant to be heading for —
    // pushed aside, slid down a slope, went round something the long way —
    // then the old route starts from somewhere we no longer are. Work out a
    // new one from here, and say so.
    if (g_rebuild_cooldown > 0.0f) g_rebuild_cooldown -= dt;
    if (g_have_path && g_wp_index < g_waypoints.size() && g_rebuild_cooldown <= 0.0f &&
        g_recover_timer <= 0.0f) {
        // Off the LEG being walked (previous waypoint -> current one), not
        // merely far from the next point: the engine's routes have long legs,
        // and on one of those the next point is far away by design — which
        // set this off over and over on a walk that was going fine.
        auto pp = game::GetPlayerPosition();
        const auto& w = g_waypoints[g_wp_index];
        const auto& a = g_wp_index > 0 ? g_waypoints[g_wp_index - 1] : w;
        float sx = w.x - a.x, sy = w.y - a.y;
        float len2 = sx * sx + sy * sy;
        float t = len2 > 1.0f ? ((pp.x - a.x) * sx + (pp.y - a.y) * sy) / len2 : 1.0f;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        float dx = a.x + t * sx - pp.x, dy = a.y + t * sy - pp.y;
        if (std::sqrt(dx * dx + dy * dy) > kLostDist) {
            // Said once per episode; a second rebuild in a row is silent.
            if (!RebuildPath(g_lost_said ? nullptr : "Zgubiłem trasę, przeliczam."))
                g_rebuild_cooldown = kRebuildEvery;   // try again in a moment
            g_lost_said = true;
        } else {
            g_lost_said = false;
        }
    }

    if (g_recover_timer > 0.0f) {
        // Recovery: strafe sideways to slip past the obstacle. On the first few
        // stalls push FORWARD+strafe; once we've stalled repeatedly (wedged into
        // a corner/rock in open terrain), REVERSE+strafe to back out first, then
        // steer around next window.
        bool backOut = (g_stuck_count >= 2);
        if (config::Get().native_walk)
            SidestepNative(g_recover_dir, CurrentGoal(), backOut);
        else
            Move(!backOut, g_recover_dir < 0, backOut, g_recover_dir > 0);
        g_recover_timer -= dt;
        if (g_recover_timer <= 0.0f)
            g_last_probe_dist = DistanceToTarget();   // fresh progress base
    } else {
        SteerRoute();             // along the route, rounding its bends
    }

    // While a just-opened door swings, don't count us as stuck — keep walking
    // through. Reset the progress base when the grace ends.
    if (g_door_cooldown > 0.0f) {
        g_door_cooldown -= dt;
        if (g_door_cooldown <= 0.0f) g_last_probe_dist = DistanceToTarget();
    }

    // Stuck detection: measure PROGRESS TOWARD THE TARGET over the window. If we
    // didn't get meaningfully closer — blocked, or spinning in place near an
    // unreachable marker — treat as stuck: open a door, strafe around, give up.
    g_probe_timer += dt;
    if (g_probe_timer >= kProbeEvery) {
        g_probe_timer = 0.0f;
        // "Trying to move" covers BOTH drives: keys held, or the engine mover
        // being steered. Testing only the keys meant that under native walking
        // the probe never ran — so a walk that could not quite reach its target
        // was never recognised as arrived-or-stuck, and just pushed forever.
        bool driving = g_forward_down || mover::Active();
        auto ppNow = game::GetPlayerPosition();
        float mvx = ppNow.x - g_probe_pos.x, mvy = ppNow.y - g_probe_pos.y;
        g_probe_pos = ppNow;
        const bool onEngineRoute = g_route_engine && g_have_path &&
                                   g_wp_index < g_waypoints.size();
        if (g_recover_timer <= 0.0f && driving && g_door_cooldown <= 0.0f) {
            float progress = g_last_probe_dist - dist;   // + = got closer
            g_last_probe_dist = dist;
            // The engine's route may lead away from the target first (round a
            // wall, down a corridor); there a stall means NOT MOVING, as in the
            // FNV mod — measured as distance covered, not distance gained.
            if (onEngineRoute) progress = std::sqrt(mvx * mvx + mvy * mvy);
            if (progress < kMinProgress) {
                // Stalled right next to the target → we've effectively arrived
                // (the exact origin may be unreachable). Announce arrival, not
                // an obstacle — fixes "doszedł, ale się nie zatrzymał".
                if (dist <= kNearArrive && sameFloor) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "Jesteś przy celu: %s",
                                  g_target_name.c_str());
                    Arrive(buf);
                    return;
                }
                // Blocked further out — a closed door is the usual culprit. Open
                // the nearest one and keep going before treating this as a real
                // obstacle. (No-op if it's the same door we already opened.)
                if (TryOpenBlockingDoor()) return;
                // On a navmesh path but wedged at this waypoint (e.g. off-mesh on
                // a rock, or an awkward corner) → SKIP to the next waypoint so one
                // bad hop doesn't stall the whole route. Changing the heading
                // often frees us; recovery/jump below still run as backup.
                // Not on the engine's route: its waypoints follow walls and
                // doorways, and skipping them cuts straight into one.
                if (!onEngineRoute && g_have_path && g_wp_index + 1 < g_waypoints.size()) {
                    // Escalate: the longer we've been wedged here, the further we
                    // skip ahead (capped) to find a heading PAST the obstacle
                    // cluster rather than re-hitting the same blocked waypoint.
                    size_t skip = (size_t)(1 + (g_stuck_count > 3 ? 3 : g_stuck_count));
                    size_t ni = g_wp_index + skip;
                    g_wp_index = ni < g_waypoints.size() ? ni : g_waypoints.size() - 1;
                    g_last_probe_dist = DistanceToTarget();
                }
                ++g_stuck_count;
                if (g_stuck_count >= kMaxStuck) {
                    StopWalking("Utknąłem. Spróbuj naprowadzania ręcznego.");
                    return;
                }
                // Wedged twice in a row: the route itself may be the problem
                // (a gap the navmesh claims is there). Work it out afresh from
                // here before more sidestepping.
                // On the engine's route at once (it repaths, as the FNV mod does).
                if (g_stuck_count == (onEngineRoute ? 1 : 2) && g_rebuild_cooldown <= 0.0f)
                    RebuildPath("Przeliczam trasę.");
                g_recover_dir   = -g_recover_dir;   // try the other side
                g_recover_timer = kRecoverSecs;
                TriggerJump();   // hop while strafing — frees ledges/lips/rubble
                F3A_DEBUG("autowalk: %s", DescribeObstacle().c_str());
            } else {
                g_stuck_count = 0;                  // made progress — reset
            }
        }
        if (!driving) g_last_probe_dist = DistanceToTarget();
    }
}

void Init()
{
    F3A_INFO("AutoWalk module ready.");
}

void Shutdown()
{
    Move(false, false, false, false);   // never leave a movement key stuck down
    g_state = State::Idle;
}

} // namespace f3a::modules::autowalk
