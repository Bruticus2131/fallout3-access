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
constexpr float kWaypointRadius = 96.0f;   // advance when this close (~1.5 m)
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

// Distance callouts.
float        g_callout_timer = 0.0f;
constexpr float kCalloutEvery = 2.5f;

// Door-opening: when blocked, the obstacle is usually a closed door (e.g. the
// Overseer's office). Open the nearest one with the native Activate and resume.
// Track the last door tried so that, if we're STILL stuck against it after a
// grace period, we give up instead of toggling it shut again.
uint32_t     g_last_door     = 0;
float        g_door_cooldown = 0.0f;
constexpr float kDoorReach   = 300.0f;   // ~4.7 m — a door we've walked up to

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

// Different-floor guard: with no navmesh path we can't climb stairs, so if the
// target is well above/below us, stop and tell the player to use the beacon.
constexpr float kFloorDelta = 160.0f;

// No-reference cross-space test: with no target reference to compare cells with,
// a target this far away that the navmesh cannot path to is treated as being in
// another location rather than walked at blindly.
constexpr float kNoPathFarDist = 4000.0f;

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

// --- Movement keys (scancodes: W=0x11, A=0x1E, S=0x1F, D=0x20) --------------
constexpr WORD kScanW = 0x11;
constexpr WORD kScanA = 0x1E;
constexpr WORD kScanS = 0x1F;
constexpr WORD kScanD = 0x20;
bool g_forward_down = false;
int  g_strafe_down = 0;   // 0 = none, -1 = left (A), +1 = right (D)

void SendScan(WORD scan, bool down)
{
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
void TriggerJump()      { if (!g_kjump) { SetJump(true); g_jump_timer = 0.30f; } }

// --- Stuck recovery: when forward progress stalls (an obstacle), strafe
// sideways for a beat to slip around it, alternating sides on repeats, before
// giving up. No raycasts available, so it "feels" the wall by trial.
float g_recover_timer = 0.0f;   // seconds of strafing left; 0 = normal steering
int   g_recover_dir   = 1;      // side to strafe next (alternates)
int   g_stuck_count   = 0;      // consecutive stalls without progress
constexpr float kRecoverSecs = 1.2f;
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

// Steer toward the goal. FIRST person: mouse-free STRAFE — slide toward the goal
// with an 8-way W/A/S/D combo, never turning the camera (movement is camera-
// relative and rotZ = camera in first person, so the combo stays correct as we
// slide). THIRD person: the camera orbits independently of movement, so fall
// back to mouse-turn + forward. Returns |relative bearing|.
float Steer(const game::Vec3& goal)
{
    auto  pp  = game::GetPlayerPosition();
    float rel = game::ComputeBearing(pp, game::GetPlayerYaw(), goal).relative_yaw;

    // Preferred: drive the engine's movement update directly (see player_mover).
    // No keys, exact speed, and the turn slows the walk instead of skidding
    // through it. Falls through to the key-holding path when the hook isn't in
    // place or [Voice] NativeWalk is off, so walking always works somehow.
    if (config::Get().native_walk && mover::Available()) {
        Move(false, false, false, false);          // make sure no key is stuck down
        mover::SetGoal(goal.x, goal.y, /*run*/true);
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

void StopWalking(const char* reason_utf8)
{
    if (g_state == State::Idle) return;
    mover::Clear();                     // stop driving the engine's mover
    Move(false, false, false, false);   // release all movement keys
    SetJump(false);                     // and the jump key
    g_jump_timer    = 0.0f;
    g_recover_timer = 0.0f;
    g_stuck_count   = 0;
    g_cross         = false;
    g_cross_wait    = 0.0f;
    g_cross_door_refr = nullptr;
    g_state = State::Idle;
    if (reason_utf8 && *reason_utf8)
        tolk::Speak(reason_utf8, tolk::Priority::System, true);
}


// When blocked, try to open the nearest door (native Activate). Returns true if
// it opened one (caller should give it a moment before re-checking progress).
// Returns false if the only door in reach is the one we already tried (it's
// probably locked) or there's no door — caller falls back to strafe/give-up.
bool TryOpenBlockingDoor()
{
    auto pp = game::GetPlayerPosition();
    auto ents = game::ScanNearby((int)kDoorReach, 8, /*actors_only*/false, false);
    const game::WorldEntity* best = nullptr;
    float bestd = kDoorReach * kDoorReach;
    for (const auto& e : ents) {
        if (e.kind != game::WorldEntity::Kind::Door || !e.refr || !e.form_id)
            continue;
        float dx = e.position.x - pp.x, dy = e.position.y - pp.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestd) { bestd = d2; best = &e; }
    }
    if (!best) return false;
    if (best->form_id == g_last_door) return false;   // already tried — likely locked
    game::QueueActivate(best->refr, best->form_id);
    tolk::Speak("Otwieram drzwi: " + best->name, tolk::Priority::Background, false);
    g_last_door     = best->form_id;
    g_door_cooldown = 2.5f;                            // let it swing open (progress
                                                       // base is reset when it ends)
    return true;
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
            // A door still in the cone here is the one TryOpenBlockingDoor already
            // tried (else it'd have opened it) — so it's locked or blocked.
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
    auto ents = game::ScanNearby(kCrossScan, 48, /*actors_only*/false, false);
    const void* hereCell = game::GetPlayerCell();
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

        int cat;
        if (game::DoorLeadsToTargetSpace(e.refr, g_target_refr)) cat = 2;
        else if (game::DoorLeadsToExterior(e.refr))             cat = 1;
        else continue;   // leads into some other interior — never a good pick

        float dx = e.position.x - pp.x, dy = e.position.y - pp.y;
        float d2 = dx * dx + dy * dy;
        bool better = !best || cat > bestCat || (cat == bestCat && d2 < bestd);
        if (better) { best = &e; bestCat = cat; bestd = d2; }
    }
    if (!best) return false;
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
    StopWalking("Nie mogę dojść pieszo. Otwórz mapę świata i użyj szybkiej podróży.");
}

// Begin (or restart) cross-worldspace routing: remember the cell we're leaving,
// then pick the first door. Teleports immediately if nothing connects.
void BeginCross()
{
    g_cross            = true;
    g_cross_from_cell  = game::GetPlayerCell();
    g_cross_door_refr  = nullptr;
    g_cross_stuck      = 0;
    g_probe_timer      = 0.0f;
    if (!PickCrossDoor()) { CrossFallbackTeleport(); return; }
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
            g_waypoints.clear(); g_wp_index = 0;
            g_have_path = navmesh::BuildPath(game::GetPlayerPosition(),
                          g_target_pos, g_waypoints) && !g_waypoints.empty();
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
        if (++g_hop_count > kMaxHops || !PickCrossDoor()) {
            CrossFallbackTeleport();
            return;
        }
        g_cross_stuck = 0;
        g_probe_timer = 0.0f;
        g_last_probe_dist = std::sqrt(
            (g_cross_door_pos.x - game::GetPlayerPosition().x) *
            (g_cross_door_pos.x - game::GetPlayerPosition().x) +
            (g_cross_door_pos.y - game::GetPlayerPosition().y) *
            (g_cross_door_pos.y - game::GetPlayerPosition().y));
        tolk::Speak("Kolejne przejście: " + g_cross_door_name,
                    tolk::Priority::Background, false);
        return;
    }

    if (!g_cross_door_refr) {                 // lost the door → repick or teleport
        if (!PickCrossDoor()) { CrossFallbackTeleport(); return; }
    }

    auto  pp = game::GetPlayerPosition();
    float dx = g_cross_door_pos.x - pp.x, dy = g_cross_door_pos.y - pp.y;
    float dd = std::sqrt(dx * dx + dy * dy);

    if (dd <= kCrossDoorReach) {              // at the door → go through it
        game::QueueActivate(g_cross_door_refr, g_cross_door_form);
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
        Move(true, g_recover_dir < 0, false, g_recover_dir > 0);
        g_recover_timer -= dt;
        if (g_recover_timer <= 0.0f) g_last_probe_dist = dd;
    } else {
        Steer(g_cross_door_pos);
    }

    // Progress probe toward the door: no progress → try a plain closed door in
    // the way, else count a stall; enough stalls → teleport fallback.
    g_probe_timer += dt;
    if (g_probe_timer >= kProbeEvery) {
        g_probe_timer = 0.0f;
        float progress = g_last_probe_dist - dd;
        g_last_probe_dist = dd;
        if (progress < kMinProgress && g_recover_timer <= 0.0f) {
            if (TryOpenBlockingDoor()) return;
            if (++g_cross_stuck >= kCrossMaxStuck) { CrossFallbackTeleport(); return; }
            g_recover_dir   = -g_recover_dir;
            g_recover_timer = kRecoverSecs;
            TriggerJump();
            tolk::Speak(DescribeObstacle(), tolk::Priority::Background, false);
        } else if (progress >= kMinProgress) {
            g_cross_stuck = 0;
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
    g_target_pos  = pos;
    g_target_name = name;
    g_target_refr = refr;
    g_target_form = form_id;

    g_last_probe_dist = DistanceToTarget();
    g_probe_timer    = 0.0f;
    g_callout_timer  = 0.0f;
    g_recover_timer  = 0.0f;
    g_stuck_count    = 0;
    g_recover_dir    = 1;
    g_last_door      = 0;
    g_door_cooldown  = 0.0f;

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

    g_path_retry_timer = 0.0f;
    g_floor_warned     = false;
    g_follow_quest     = false;   // a plain target; StartToQuest re-sets this
    g_state            = State::Walking;

    bool shares = game::TargetSharesPlayerSpace(refr);
    F3A_INFO("autowalk StartTo '%s' refr=%p shares_space=%d cross=%d | player_at:%s | target_at:%s",
             name.c_str(), refr, (int)shares, (int)(refr && !shares),
             game::GetCurrentLocationName().c_str(),
             game::GetRefSpaceLabel(refr).c_str());

    // A "travel leg" (hop to the nearest known marker, walk the rest) used to run
    // here. It relied on the native MoveTo, which CRASHES the game — so the hop
    // is disabled until fast travel can be driven through the map menu instead.
    // TryTravelLeg is kept for that rework; it is not called.

    // Try a real path first: whether the target is even reachable on foot is the
    // evidence we need below when there's no reference to test the space with.
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
    if (other_space) {
        tolk::Speak("Cel w innej lokacji: " + name +
                    ". Prowadzę przez przejścia.",
                    tolk::Priority::System, true);
        BeginCross();
        return;
    }

    Steer(CurrentGoal());

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Idę do: %s, %s%s",
                  name.c_str(),
                  strings::FormatDistance(DistanceToTarget()).c_str(),
                  g_have_path ? ", trasa wyznaczona" : "");
    tolk::Speak(buf, tolk::Priority::System, true);
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
    poll::RequestTeleport(best->refr);
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
    StartTo(qt.position, label, qt.refr, 0);
    g_follow_quest = true;          // StartTo cleared it
    return true;
}

void Stop()
{
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
    StartTo(qt.position, label, qt.refr, 0);
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
    if (!poll::IsGameplayActive()) {
        if (g_cross) return;             // loading between cells — keep the walk alive
        StopWalking(nullptr);
        return;
    }
    auto active = menu::ActiveMenu();
    if (active != menu::Id::None && active != menu::Id::HUDMain) {
        if (g_cross) return;             // a LoadingMenu during a door transition
        StopWalking(nullptr);
        return;
    }

    // Release a recovery-jump once its brief hold elapses (both modes use it).
    if (g_kjump) { g_jump_timer -= dt; if (g_jump_timer <= 0.0f) SetJump(false); }

    // Quest walks follow the LIVE objective: if it advanced (or another quest
    // took the tracked slot), switch targets mid-walk instead of marching to a
    // marker that no longer matters.
    if (FollowQuestRetarget()) return;

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
        StopWalking(buf);
        return;
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
                                   g_waypoints) && !g_waypoints.empty()) {
                g_have_path = true;
                g_last_probe_dist = dist;   // fresh progress base for the new route
                tolk::Speak("Trasa wyznaczona.", tolk::Priority::Background, false);
            }
        }
        if (!g_floor_warned && !sameFloor) {
            g_floor_warned = true;
            tolk::Speak(dz > 0 ? "Cel wyżej — szukam drogi po schodach."
                               : "Cel niżej — szukam drogi po schodach.",
                        tolk::Priority::Background, false);
        }
    }

    AdvanceWaypoints();

    if (g_recover_timer > 0.0f) {
        // Recovery: strafe sideways to slip past the obstacle. On the first few
        // stalls push FORWARD+strafe; once we've stalled repeatedly (wedged into
        // a corner/rock in open terrain), REVERSE+strafe to back out first, then
        // steer around next window.
        bool backOut = (g_stuck_count >= 2);
        Move(!backOut, g_recover_dir < 0, backOut, g_recover_dir > 0);
        g_recover_timer -= dt;
        if (g_recover_timer <= 0.0f)
            g_last_probe_dist = DistanceToTarget();   // fresh progress base
    } else {
        Steer(CurrentGoal());     // strafe (1st person) or mouse-turn (3rd)
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
        if (g_recover_timer <= 0.0f && g_forward_down && g_door_cooldown <= 0.0f) {
            float progress = g_last_probe_dist - dist;   // + = got closer
            g_last_probe_dist = dist;
            if (progress < kMinProgress) {
                // Stalled right next to the target → we've effectively arrived
                // (the exact origin may be unreachable). Announce arrival, not
                // an obstacle — fixes "doszedł, ale się nie zatrzymał".
                if (dist <= kNearArrive && sameFloor) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "Jesteś przy celu: %s",
                                  g_target_name.c_str());
                    StopWalking(buf);
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
                if (g_have_path && g_wp_index + 1 < g_waypoints.size()) {
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
                g_recover_dir   = -g_recover_dir;   // try the other side
                g_recover_timer = kRecoverSecs;
                TriggerJump();   // hop while strafing — frees ledges/lips/rubble
                tolk::Speak(DescribeObstacle(),
                            tolk::Priority::Background, false);
            } else {
                g_stuck_count = 0;                  // made progress — reset
            }
        }
        if (!g_forward_down) g_last_probe_dist = DistanceToTarget();
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
    Move(false, false, false, false);   // never leave a movement key stuck down
    g_state = State::Idle;
}

} // namespace f3a::modules::autowalk
