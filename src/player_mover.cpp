#include "f3a/player_mover.h"
#include "f3a/fose_runtime.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/audio_beacon.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace f3a::mover {
namespace {

namespace rt = fose_rt;

// --- Addresses (Fallout 3 1.7.0.3) ------------------------------------------
//
// PlayerMover's vtable and the Update slot inside it. The layout matches New
// Vegas exactly, which is how the slot was identified: slots 1/2/7 set, clear
// and read the movement flags at mover+0x94, slot 4 clears mover+0x38 and the
// byte at +0x71, and slot 5 is the big per-frame function — Update.
constexpr uintptr_t kPlayerMoverVtable = 0x00E1EE04;
constexpr uintptr_t kUpdateSlot        = kPlayerMoverVtable + 5 * 4;
constexpr uintptr_t kExpectedUpdate    = 0x007E8950;

// Actor::Move lives in the ACTOR's vtable at +0x250 — read from the live object
// rather than hard-coded, so it cannot drift with the build. The call site
// inside Update reads:
//   mov esi,[ecx] ; mov edx,[esi+0x250] ; push flags ; push disp ; fstp dt ; call edx
// __thiscall void Move(float deltaTime, float displacement[3], UInt32 flags)
constexpr uintptr_t kActorMoveVtableOffset = 0x250;
constexpr uint32_t  kMoveFlags             = 0x0001;

// Movement flag words on the mover. Scripts read these for IsMoving/IsRunning,
// and the engine uses them for footstep and animation decisions.
constexpr uintptr_t kMoverFlags1 = 0x34;
constexpr uintptr_t kMoverFlags2 = 0x94;
constexpr uint32_t  kFlagForward = 0x001;
constexpr uint32_t  kFlagWalking = 0x100;
constexpr uint32_t  kFlagRunning = 0x200;
constexpr uint32_t  kFlagMask    = kFlagForward | kFlagWalking | kFlagRunning;

// Turning: cap the rate so the body swings smoothly, and cut speed while a turn
// is still in progress. An open-loop move vector at full speed through a sharp
// corner is what walks a player off a ledge.
constexpr float kTurnRateRadPerSec = 4.0f;
constexpr float kPi    = 3.14159265f;
constexpr float kTwoPi = 6.28318530f;

using UpdateFn = void (__thiscall*)(void* mover, float dt);
UpdateFn g_original = nullptr;

std::atomic<bool>  g_installed{ false };
std::atomic<bool>  g_active{ false };
std::atomic<float> g_goal_x{ 0.0f };
std::atomic<float> g_goal_y{ 0.0f };
// The route's next corner, for braking (see SetGoalWithCorner).
std::atomic<bool>  g_corner_on{ false };
std::atomic<float> g_corner_x{ 0.0f };
std::atomic<float> g_corner_y{ 0.0f };
std::atomic<bool>  g_run{ false };
float g_heading = 0.0f;          // radians, engine convention (0 = +Y/north)
bool  g_heading_seeded = false;
// A new walk was started from the poll thread: the hook re-seeds its own
// heading/footstep/animation state on its next frame (those are its own).
std::atomic<bool> g_reseed_pending{ false };

// Stopping is requested from the polling thread but CARRIED OUT in the hook,
// which runs on the game's own thread and is handed the engine's real mover as
// `this`. Doing it the other way round meant guessing the mover's offset inside
// the player and calling an animation function off-thread — and if that guess
// was wrong the movement bits were never cleared, which left the player walking
// on by themselves with nothing on screen to explain it.
std::atomic<bool> g_stop_pending{ false };

// Watchdog. Whoever drives the mover re-states the goal every tick, so a goal
// that stops being refreshed means the driver is gone — the walk was cancelled,
// a menu opened, a load screen came up, the mod was switched off mid-walk. The
// engine would happily keep pushing the player toward the last goal forever, so
// treat silence as "stop". This is the backstop that makes self-walking
// impossible regardless of which caller forgot to clean up.
// Frames of "brake" after a stop: see the stop path in the hook.
// Short enough that the coast after a stop is a step, not a stride: at walking
// speed 20 frames carried the player ~50 units past the point where the mod
// said it had stopped, which is far enough to walk into whatever you stopped
// to avoid. Still long enough to outlast a frame hitch.
constexpr int kBrakeFrames = 10;
int g_brake = 0;

// Post-stop probe. Neither of us can watch the screen, so "it said stopped and
// kept walking" has to become a measurement rather than a guess — and it has to
// be an UNCONDITIONAL one. The first version only wrote a line when the player
// had travelled more than 40 units, so silence meant either "stood still" or
// "the probe never got that far", and a log that ends before the window closes
// cannot tell those apart. Now every stop writes three lines whatever happens,
// which also settles the other half of the question: a walk animation left
// looping sounds EXACTLY like walking to a player who cannot see the screen,
// and distance-moved is what separates real motion from a phantom.
constexpr float kWatchAt[3] = { 0.5f, 1.5f, 3.0f };   // seconds after the stop
float g_watch = 0.0f;
int   g_watch_stage = 3;                // 3 = finished / not watching
float g_watch_x = 0.0f, g_watch_y = 0.0f;

std::atomic<uint32_t> g_goal_stamp{ 0 };   // bumped by every SetGoal
uint32_t g_seen_stamp = 0;                 // hook-side copy of it
float    g_goal_age   = 0.0f;              // seconds since it last changed
constexpr float kGoalTimeout = 0.40f;      // ~5 autowalk ticks

// Footsteps. Driving the player through Actor::Move skips the walk animation,
// and the game hangs its footstep sounds off that animation — so an autowalk was
// silent, losing the one cue that tells a blind player they are actually moving
// and how fast. Until the animation calls are reverse-engineered, synthesize the
// beat ourselves: a short, quiet click every so many units of ground covered, so
// the rhythm follows real speed and stops dead when the player wedges.
constexpr float kFootstepEvery = 55.0f;   // game units between steps
float g_step_accum = 0.0f;
float g_anim_refresh = 0.0f;              // seconds since the walk anim was issued
constexpr float kAnimRefreshSecs = 1.5f;  // long enough not to stutter the anim
bool  g_step_alt = false;                 // alternate pitch: left/right feel

// Animation groups the engine itself uses (same ids as the PlayGroup command).
constexpr uint32_t kAnimIdle        = 0;
constexpr uint32_t kAnimForward     = 3;   // walk
constexpr uint32_t kAnimFastForward = 7;   // run
uint32_t g_current_anim = kAnimIdle;
bool     g_anim_works = true;              // cleared if the address guards refuse

float NormalizeAngle(float a)
{
    while (a >  kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

// Speed scale through a turn: crawl while badly misaligned, full speed once
// pointed the right way. Mirrors what the engine's own followers do.
// Speed for a heading error, in the bands the engine's own NPC follower uses
// (the FNV mod mirrors ComputeTurnAndSpeed): full speed to ~45°, then half,
// and a crawl — never a dead stop — through hairpins, so the player keeps
// moving while turning and a turn never slides off at full speed. The old
// ramp braked from 11° on, which made every slight bend a slowdown.
float TurnSpeedScale(float absErrRad)
{
    if (absErrRad > 2.71f) return 0.05f;   // > ~155°: turning round
    if (absErrRad > 2.01f) return 0.10f;   // > ~115°
    if (absErrRad > 1.60f) return 0.20f;   // > ~92°
    if (absErrRad > 0.79f) return 0.50f;   // > ~45°
    return 1.0f;
}

void SetMoverFlags(void* mover, bool moving, bool running)
{
    if (!mover || IsBadWritePtr(mover, 0x98)) return;
    auto* m = reinterpret_cast<uint8_t*>(mover);
    uint32_t bits = moving ? (kFlagForward | (running ? kFlagRunning : kFlagWalking)) : 0u;
    auto* f1 = reinterpret_cast<uint32_t*>(m + kMoverFlags1);
    auto* f2 = reinterpret_cast<uint32_t*>(m + kMoverFlags2);
    *f1 = (*f1 & ~kFlagMask) | bits;
    *f2 = (*f2 & ~kFlagMask) | bits;
}

// Stop the mover the way the ENGINE stops it, through its own vtable, instead
// of writing the flag words ourselves.
//
//   slot 2  0x007E8F30  ClearMovementFlags(UInt32)  ANDs ~bits into mover+0x94
//   slot 4  0x007DDB40  ClearMoveState()            zeroes mover+0x38 and +0x71
//
// The second one matters: +0x38 and +0x71 are state we never touched, and
// clearing the flag words alone left them set — which is a very good reason for
// a player to keep walking after everything else said the walk was over.
// Only called when the object really is a PlayerMover, checked by its vtable,
// so the slot numbers are the ones that were disassembled.
void EngineStopMover(void* mover)
{
    if (!mover || IsBadReadPtr(mover, 4)) return;
    void** vt = *reinterpret_cast<void***>(mover);
    if (reinterpret_cast<uintptr_t>(vt) != kPlayerMoverVtable) return;
    if (IsBadReadPtr(vt, 5 * sizeof(void*))) return;

    using ClearFlagsFn = void (__thiscall*)(void*, uint32_t);
    using ClearStateFn = void (__thiscall*)(void*);
    if (vt[2]) reinterpret_cast<ClearFlagsFn>(vt[2])(mover, 0xFFFFFFFFu);
    if (vt[4]) reinterpret_cast<ClearStateFn>(vt[4])(mover);
}

void ActorMove(void* actor, float dt, float disp[3])
{
    if (!actor) return;
    void** vtable = *reinterpret_cast<void***>(actor);
    if (!vtable || IsBadReadPtr(vtable, kActorMoveVtableOffset + 4)) return;
    using MoveFn = void (__thiscall*)(void* self, float dt, float* disp, uint32_t flags);
    auto fn = reinterpret_cast<MoveFn>(vtable[kActorMoveVtableOffset / 4]);
    if (!fn) return;
    fn(actor, dt, disp, kMoveFlags);
}

// Put the mover back the way the engine expects and drop out of the walk
// animation. Main thread only — it calls into the game.
//
// The idle animation is not cosmetic here. The walk animation is what produces
// the game's FOOTSTEP SOUNDS, and for a player who cannot see the screen those
// sounds ARE the message "you are still walking". Leaving them running after a
// walk has ended is indistinguishable, by ear, from the walk never stopping.
// The mover's own movement DIRECTION, and the byte that says it has one.
//
// Clearing the flag words and calling the engine's stop was not enough: the
// player kept walking after the mod had said "stopped", and only pressing W
// halted them — which is the tell, because W is what rewrites these fields
// through the normal input path. The New Vegas mod zeroes exactly these on its
// own stop, and the two flag words either side of them (+0x34, +0x94) already
// match this build, so the layout is shared.
//
// Checked before written, not assumed: a direction vector is small and finite,
// and the flag byte is 0 or 1. Anything else means the offsets are not what we
// think on this build, and then nothing is written at all.
constexpr uintptr_t kMoverDirX   = 0x10;
constexpr uintptr_t kMoverHasDir = 0x77;

void ClearMoveDirection(void* mover)
{
    if (!mover || IsBadWritePtr(mover, 0x98)) return;
    auto* m = reinterpret_cast<uint8_t*>(mover);
    auto* d = reinterpret_cast<float*>(m + kMoverDirX);
    auto* hasDir = m + kMoverHasDir;

    bool sane = std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2]) &&
                std::fabs(d[0]) <= 4.0f && std::fabs(d[1]) <= 4.0f &&
                std::fabs(d[2]) <= 4.0f && *hasDir <= 1;
    if (!sane) {
        F3A_INFO("Mover: direction fields look wrong (%.3f,%.3f,%.3f hasDir=%u)"
                 " — leaving them alone.", d[0], d[1], d[2], (unsigned)*hasDir);
        return;
    }
    if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f && *hasDir == 0) return;
    F3A_INFO("Mover: clearing direction (%.3f,%.3f,%.3f hasDir=%u)",
             d[0], d[1], d[2], (unsigned)*hasDir);
    d[0] = d[1] = d[2] = 0.0f;
    *hasDir = 0;
}

// Re-issues of the idle animation left to make during the brake. One call on
// the stop frame is not enough: the engine re-evaluates the player's animation
// inside the same update we are about to hand the frame back to, and can put
// the walk cycle straight back. The walk cycle is what plays the game—s own
// FOOTSTEPS, so one that outlives the walk is not cosmetic: it is the sound of
// walking, continuing, after the mod has announced that it stopped.
int g_idle_left = 0;
constexpr int kIdleReissues = 4;

void StopNow(void* mover)
{
    SetMoverFlags(mover, false, false);
    EngineStopMover(mover);
    ClearMoveDirection(mover);
    // Unconditionally, not "only if we think the walk animation is up". That
    // bookkeeping is our note about what we last asked for, not what the engine
    // is actually playing, and the periodic re-issue above deliberately lies to
    // it. Asking for idle when idle is already playing costs nothing.
    {
        bool killed = false, idled = false;
        if (auto* p = rt::Player()) {
            // Order matters: take the walk cycle out of its own slot FIRST,
            // then put the idle up. The other way round the idle lands while
            // the walk is still running beside it, which is the state that was
            // producing footsteps on a stationary player.
            killed = game::StopActorMovementAnim(p);
            idled  = game::PlayActorAnimGroup(p, kAnimIdle);
        }
        g_current_anim = kAnimIdle;
        g_idle_left    = kIdleReissues;
        F3A_INFO("Mover: stop — flags cleared, walk animation %s, idle %s.",
                 killed ? "stopped" : "NOT STOPPED",
                 idled ? "played" : "REFUSED");
    }
    g_step_accum = 0.0f;
    g_anim_refresh = 0.0f;

    if (auto* p = rt::Player()) {
        g_watch       = 0.0f;
        g_watch_stage = 0;
        g_watch_x     = p->posX;
        g_watch_y     = p->posY;
    }
}

// The live mover, captured for the path probe. Set inside the engine's own
// update, so it is always the object the game is actually using.
void* g_last_mover = nullptr;

// --- The engine's own pathfinder (FO3 1.7.0.3) ------------------------------
//
// Found by disassembly, matching the New Vegas recipe the FNV mod uses (the
// code is near-identical; see tools/fo3_pathfinding_report.md):
//   SetPathfindingGoal 0x006F8AB0  bool __thiscall(Actor*, Ref*, float, void*)
//   ActorMover::Update 0x007E0D40  void __thiscall(mover, float dt)
//   FullReset          0x007DF400  void __thiscall(mover)
// The player's mover never paths by itself — PlayerMover::Update overrides the
// ActorMover one — so after setting the goal the NPC update is ticked by hand
// until the request is answered (mover+0x74 drops), the waypoints are copied
// out of the PathingSolution (mover+0x20, or the path handler's +0x0C; data
// +0x24, count +0x28, stride 0x3C, xyz +0x08), and the mover is reset so no
// NPC path handler is left steering the player.
constexpr uintptr_t kSetPathfindingGoal = 0x006F8AB0;
constexpr uintptr_t kActorMoverUpdate   = 0x007E0D40;
constexpr uintptr_t kMoverFullReset     = 0x007DF400;
constexpr int       kPathTimeoutFrames  = 240;     // ~4 s of frames

using SetGoalFn   = bool (__thiscall*)(void* actor, void* ref, float radius, void* avoid);
using MoverUpdFn  = void (__thiscall*)(void* mover, float dt);
using FullResetFn = void (__thiscall*)(void* mover);

// Only on the build these were read from: the first bytes of each function
// must be exactly the ones disassembled, or the feature stays off.
int g_ep_verified = -1;                  // -1 unknown, 0 no, 1 yes
bool EnginePathCodeOk()
{
    if (g_ep_verified < 0) {
        static const unsigned char kGoal[]  = { 0x8B,0x44,0x24,0x0C,0x85,0xC0,0x56,0x8B };
        static const unsigned char kUpd[]   = { 0x6A,0xFF,0x68,0x28,0x17,0xC7,0x00,0x64 };
        static const unsigned char kReset[] = { 0x53,0x56,0x8B,0xF1,0x57,0xC7,0x46,0x6C };
        auto is = [](uintptr_t a, const unsigned char* b) {
            return !IsBadReadPtr(reinterpret_cast<void*>(a), 8) &&
                   std::memcmp(reinterpret_cast<void*>(a), b, 8) == 0;
        };
        bool ok = is(kSetPathfindingGoal, kGoal) && is(kActorMoverUpdate, kUpd) &&
                  is(kMoverFullReset, kReset);
        g_ep_verified = ok ? 1 : 0;
        F3A_INFO("EnginePath: game functions %s", ok ? "verified"
                 : "do not match this build - engine routes disabled");
    }
    return g_ep_verified == 1;
}

std::mutex              g_ep_mutex;       // guards the request and the result
const void*             g_ep_ref    = nullptr;
uint32_t                g_ep_refid  = 0;
float                   g_ep_radius = 100.0f;
std::vector<game::Vec3> g_ep_points;
std::atomic<int>        g_ep_request{ 0 };  // 1 = a new request is waiting
std::atomic<int>        g_ep_cancel{ 0 };
std::atomic<int>        g_ep_state{ 0 };    // see EnginePathState()
// Every request and cancel gets a new number. A result is handed over only if
// it answers the newest one: a route worked out for the previous target must
// never be adopted by the walk to the next.
uint32_t                g_ep_seq      = 0;  // under g_ep_mutex
uint32_t                g_ep_req_seq  = 0;  // the waiting request's number (mutex)
uint32_t                g_ep_work_seq = 0;  // hook thread: the request being worked
bool                    g_ep_ticking = false;   // hook thread only
int                     g_ep_frames  = 0;       // hook thread only
// Where the player was when the request went in (hook thread). A load door,
// fast travel or a load frees the navmesh the request points into; ticking the
// mover after that crashes deep in the engine's pathing (the FNV mod saw it),
// so the request is then dropped WITHOUT calling into the mover.
const void*             g_ep_cell = nullptr;
const void*             g_ep_ws   = nullptr;
float                   g_ep_px = 0.0f, g_ep_py = 0.0f;
constexpr float         kEpTeleportJump = 4096.0f;   // as the FNV mod

// Hand a result to the walk, unless a newer request or a cancel came in since
// this one was taken (then it answers a question nobody is asking any more).
void EnginePathPublish(std::vector<game::Vec3>&& pts, int state)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    if (g_ep_work_seq != g_ep_seq) {
        F3A_INFO("EnginePath: result of an older request dropped");
        return;
    }
    g_ep_points = std::move(pts);
    g_ep_state.store(state);
}

void EnginePathFail(const char* why)
{
    F3A_INFO("EnginePath: %s", why);
    EnginePathPublish({}, 3);
}

// The player's cell and worldspace (null for an interior).
void PlayerArea(const void** cell, const void** ws)
{
    *cell = nullptr; *ws = nullptr;
    auto* p = rt::Player();
    if (!p || IsBadReadPtr(p, sizeof(*p))) return;
    TESObjectCELL* c = p->parentCell;
    *cell = c;
    if (c && !IsBadReadPtr(c, 0xC4)) *ws = c->worldSpace;
}

// Is the player still where the request being worked went in? (FNV mod's
// guard: the worldspace for exteriors — crossing exterior cells keeps the
// streamed navmesh — the cell for interiors, and a jump of thousands of units
// for a teleport.) `area` says which of the two failed.
bool EpSameArea(bool* area)
{
    const void* cell; const void* ws;
    PlayerArea(&cell, &ws);
    auto* p = rt::Player();
    float jx = p ? p->posX - g_ep_px : 0.0f, jy = p ? p->posY - g_ep_py : 0.0f;
    bool areaChanged = !p || ws != g_ep_ws || (!ws && cell != g_ep_cell);
    bool teleported  = !(jx * jx + jy * jy <= kEpTeleportJump * kEpTeleportJump);
    if (area) *area = areaChanged;
    return !areaChanged && !teleported;
}

// Called at the top of every PlayerMover::Update, on the game's thread.
void TickEnginePath(void* mover, float dt)
{
    if (g_ep_cancel.exchange(0) && g_ep_ticking) {
        // Reset only where the request still belongs: after a load door what
        // the mover holds points into freed memory (see below).
        if (EpSameArea(nullptr)) reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
        g_ep_ticking = false;
        F3A_INFO("EnginePath: cancelled");
    }

    if (g_ep_request.exchange(0)) {
        const void* ref; uint32_t refid; float radius;
        {
            std::lock_guard<std::mutex> lk(g_ep_mutex);
            ref = g_ep_ref; refid = g_ep_refid; radius = g_ep_radius;
            g_ep_work_seq = g_ep_req_seq;
        }
        if (g_ep_ticking) {                       // a newer request replaces it
            if (EpSameArea(nullptr)) reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
            g_ep_ticking = false;
        }
        if (!EnginePathCodeOk()) { EnginePathFail("unavailable on this build"); return; }
        auto* player = reinterpret_cast<UInt8*>(rt::Player());
        if (!player || IsBadReadPtr(player, 0x188)) { EnginePathFail("no player"); return; }
        // The game calls these without checking them; we check first.
        if (*reinterpret_cast<void**>(player + 0x184) != mover) {
            EnginePathFail("the hooked mover is not the player's"); return;
        }
        void* proc = *reinterpret_cast<void**>(player + 0x60);
        if (!proc || IsBadReadPtr(proc, 4)) { EnginePathFail("player has no process"); return; }
        auto* r = reinterpret_cast<const TESObjectREFR*>(ref);
        if (!r || IsBadReadPtr(r, 0x40) || r->refID != refid || !r->parentCell) {
            EnginePathFail("target reference is gone"); return;
        }
        auto* m = reinterpret_cast<UInt8*>(mover);
        if (IsBadReadPtr(m, 0x80)) { EnginePathFail("mover unreadable"); return; }

        reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
        bool ok = reinterpret_cast<SetGoalFn>(kSetPathfindingGoal)(
                      player, const_cast<void*>(ref), radius, nullptr);
        UInt8 inflight = m[0x74];
        F3A_INFO("EnginePath: SetPathfindingGoal(%08X, r=%.0f) -> %d, in flight %d",
                 refid, radius, (int)ok, (int)inflight);
        if (!ok) {
            reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
            EnginePathFail("the game found no route");
            return;
        }
        if (!inflight) {                           // already within the radius
            reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
            EnginePathFail("already there - nothing to route");
            return;
        }
        g_ep_ticking = true;
        g_ep_frames  = 0;
        PlayerArea(&g_ep_cell, &g_ep_ws);
        g_ep_px = rt::Player()->posX;
        g_ep_py = rt::Player()->posY;
    }
    if (!g_ep_ticking) return;

    // Same area as when the request went in?
    {
        bool areaChanged = false;
        if (!EpSameArea(&areaChanged)) {
            // Not reset, not ticked: what the mover holds points into freed
            // memory. The next request's FullReset clears it, as the engine's
            // own load does.
            g_ep_ticking = false;
            EnginePathFail(areaChanged ? "the player changed area mid-request - dropped"
                                       : "the player was moved mid-request - dropped");
            return;
        }
        auto* p = rt::Player();
        g_ep_px = p->posX;
        g_ep_py = p->posY;
    }

    reinterpret_cast<MoverUpdFn>(kActorMoverUpdate)(mover, dt);
    ++g_ep_frames;
    auto* m = reinterpret_cast<UInt8*>(mover);
    if (m[0x74] && g_ep_frames < kPathTimeoutFrames) return;   // still working

    // Answered (or given up): copy the waypoints straight away, while the
    // solution is certainly alive, then reset the mover.
    std::vector<game::Vec3> pts;
    const UInt8* sol = *reinterpret_cast<const UInt8**>(m + 0x20);
    if (!sol) {
        const UInt8* h = *reinterpret_cast<const UInt8**>(m + 0x24);
        if (h && !IsBadReadPtr(h, 0x10)) sol = *reinterpret_cast<const UInt8* const*>(h + 0x0C);
    }
    const bool timedOut = g_ep_frames >= kPathTimeoutFrames;
    UInt32 total = 0, otherArea = 0;
    if (!timedOut && sol && !IsBadReadPtr(sol, 0x2C)) {
        const UInt8* data = *reinterpret_cast<const UInt8* const*>(sol + 0x24);
        UInt32 n = *reinterpret_cast<const UInt32*>(sol + 0x28);
        if (data && n > 0 && n <= 4096 && !IsBadReadPtr(data, n * 0x3C)) {
            total = n;
            for (UInt32 i = 0; i < n; ++i) {
                const UInt8* e = data + i * 0x3C;
                // Each waypoint names its cell (+0x1C) and worldspace (+0x20).
                // A route that goes on through a door continues in another
                // area whose coordinates mean something else here: the walk
                // takes it up to the door and crosses by its own door logic.
                auto* wcell = *reinterpret_cast<TESObjectCELL* const*>(e + 0x1C);
                const void* wws = *reinterpret_cast<const void* const*>(e + 0x20);
                if (!wws && wcell && !IsBadReadPtr(wcell, 0xC4)) wws = wcell->worldSpace;
                bool other;
                if (wws)        other = (wws != g_ep_ws);           // exterior point
                else if (wcell) other = g_ep_ws ? true              // interior point
                                                : (wcell != g_ep_cell);
                else            other = false;                      // not recorded
                if (other) {
                    otherArea = n - i;
                    break;
                }
                const float* xyz = reinterpret_cast<const float*>(e + 0x08);
                if (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]))
                    pts.push_back({ xyz[0], xyz[1], xyz[2] });
            }
        }
    }
    int state = *reinterpret_cast<int*>(m + 0x6C);
    F3A_INFO("EnginePath: answered after %d frame(s)%s, mover state %d, %u waypoint(s)"
             " (%u of %u in another area, left out)",
             g_ep_frames, timedOut ? " (timed out)" : "", state,
             (unsigned)pts.size(), (unsigned)otherArea, (unsigned)total);
    reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
    g_ep_ticking = false;
    // State 7 is the engine's own "failed": whatever partial route it left is
    // not a way to the target, and walking it ends at a wall.
    if (state == 7) pts.clear();
    bool any = !pts.empty();
    EnginePathPublish(std::move(pts), any ? 2 : 3);
}

void __fastcall Update_Hook(void* mover, void* /*edx*/, float dt)
{
    g_last_mover = mover;
    TickEnginePath(mover, dt);
    // Aiming rides along on this hook. It has nothing to do with walking; it is
    // here because this is the one place we already run INSIDE the engine's
    // per-frame update, which is where a view change has to happen to survive
    // into the camera and the shot. See poll::AimTick.
    struct AimAfter {
        ~AimAfter() { poll::AimTick(); }
    } aimAfter;

    // Carry out a stop asked for from another thread, before anything else —
    // the engine reads these flags in the original Update we are about to call.
    if (g_stop_pending.exchange(false)) {
        StopNow(mover);
        g_brake = kBrakeFrames;
    }
    // A walk (re)started by SetGoal: fresh state, set here on the thread that
    // owns it rather than written from the poll thread mid-frame.
    if (g_reseed_pending.exchange(false)) {
        g_heading_seeded = false;    // seed on this frame
        g_step_accum     = 0.0f;
        g_current_anim   = kAnimIdle;  // force the walk anim to be issued
        g_watch_stage    = 3;          // a new walk ends the previous stop's probe
    }

    if (!g_active.load()) {
        // Braking. Simply ceasing to call Move does NOT stop the actor: it keeps
        // whatever motion it was last given until something replaces it, so the
        // player coasted on after arriving — which, with no screen to check
        // against, reads as "it said I arrived and then kept walking". Feed the
        // same call a zero displacement for a few frames to take that motion
        // away, and keep the movement bits down while the engine settles.
        if (g_brake > 0) {
            --g_brake;
            SetMoverFlags(mover, false, false);
            EngineStopMover(mover);
            ClearMoveDirection(mover);
            // Spread across the brake rather than fired in one burst, so each
            // re-issue lands on a DIFFERENT frame of the engine's own animation
            // update and one of them survives it.
            if (g_idle_left > 0 && (g_brake % 5) == 0) {
                --g_idle_left;
                if (auto* p = rt::Player()) {
                    game::StopActorMovementAnim(p);
                    game::PlayActorAnimGroup(p, kAnimIdle);
                }
            }
            float clamped = dt;
            if (clamped > 0.033f) clamped = 0.033f;
            if (clamped < 0.0f)   clamped = 0.0f;
            float zero[3] = { 0.0f, 0.0f, 0.0f };
            if (auto* p = rt::Player()) ActorMove(p, clamped, zero);
        }
        if (g_watch_stage < 3) {
            g_watch += dt;
            while (g_watch_stage < 3 && g_watch >= kWatchAt[g_watch_stage]) {
                if (auto* p = rt::Player()) {
                    float dx = p->posX - g_watch_x, dy = p->posY - g_watch_y;
                    F3A_INFO("Mover: %.1f s after the stop the player has moved "
                             "%.0f units (anim=%u).", kWatchAt[g_watch_stage],
                             std::sqrt(dx * dx + dy * dy), g_current_anim);
                }
                ++g_watch_stage;
            }
        }
        if (g_original) g_original(mover, dt);
        return;
    }

    auto* player = rt::Player();
    if (!player) {                       // no player yet — behave normally
        if (g_original) g_original(mover, dt);
        return;
    }

    // Clamp dt: a hitch (loading, alt-tab) would otherwise translate into one
    // enormous displacement step.
    float clamped = dt;
    if (clamped > 0.033f) clamped = 0.033f;
    if (clamped < 0.0f)   clamped = 0.0f;

    // Watchdog: has anyone restated the goal recently?
    uint32_t stamp = g_goal_stamp.load();
    if (stamp != g_seen_stamp) {
        g_seen_stamp = stamp;
        g_goal_age   = 0.0f;
    } else {
        g_goal_age += clamped;
        if (g_goal_age >= kGoalTimeout) {
            F3A_INFO("Mover: goal not refreshed for %.2f s — stopping.", g_goal_age);
            g_active.store(false);
            StopNow(mover);
            if (g_original) g_original(mover, dt);
            return;
        }
    }

    float gx = g_goal_x.load();
    float gy = g_goal_y.load();
    float dx = gx - player->posX;
    float dy = gy - player->posY;

    if (!g_heading_seeded) { g_heading = player->rotZ; g_heading_seeded = true; }

    float target = std::atan2(dx, dy);                 // engine convention
    float err    = NormalizeAngle(target - g_heading);

    float maxTurn = kTurnRateRadPerSec * clamped;
    float step = err;
    if (step >  maxTurn) step =  maxTurn;
    if (step < -maxTurn) step = -maxTurn;
    g_heading = NormalizeAngle(g_heading + step);

    // Write the heading before handing the frame to the engine: this is the one
    // field that decides WHERE "forward" points.
    player->rotZ = g_heading;

    // Brake for the sharper of the two turns: toward the look-ahead point we
    // steer at, and toward the route's real corner (when one was given and is
    // not right underfoot, where its bearing means nothing).
    float brake = std::fabs(err);
    if (g_corner_on.load()) {
        float cx = g_corner_x.load() - player->posX;
        float cy = g_corner_y.load() - player->posY;
        if (cx * cx + cy * cy > 32.0f * 32.0f) {
            float cerr = std::fabs(NormalizeAngle(std::atan2(cx, cy) - g_heading));
            if (cerr > brake) brake = cerr;
        }
    }

    bool running = g_run.load();
    float speed = static_cast<float>(config::Get().autowalk_speed);
    if (!running) speed *= 0.5f;
    speed *= TurnSpeedScale(brake);

    // Publish "moving forward" on the mover so scripts and the HUD agree that the
    // player is walking, then move the player ourselves.
    //
    // A word on what was tried and rejected: setting these flags and calling the
    // ORIGINAL Update looked ideal, because the engine would then walk the player
    // itself and bring its own animations and footstep sounds along. In play it
    // moved the player exactly 0.0 units per frame — the flags alone are not what
    // Update acts on — and worse, calling it first also killed the movement that
    // did work, so autowalk just stood still. Measured, logged, reverted.
    //
    // So the original Update stays out of the way while a walk is active, and the
    // motion comes from Actor::Move with an exact displacement. The cost is that
    // the walk animation never plays, and with it the game's footstep sounds, so
    // we synthesize a step beat below.
    SetMoverFlags(mover, true, running);

    // Play the walk or run animation ourselves. Actor::Move only displaces the
    // body; the animation is a separate thing, and the game's own FOOTSTEP SOUNDS
    // are driven by it — so this is what brings back the real footsteps (with
    // per-surface variation) instead of our synthesized clicks. Re-issued only
    // when the desired group changes, since replaying it every frame would
    // restart the animation continuously.
    if (g_anim_works && config::Get().walk_animation) {
        uint32_t want = running ? kAnimFastForward : kAnimForward;
        // Re-issue periodically, not only on change. Whether the animation is
        // still playing is the ENGINE's business — a collision or a posture
        // change can drop it — and our own note saying "already playing" then
        // goes stale. The footsteps stop with it, and since the synthesized
        // clicks are suppressed while the real ones are expected, the walk goes
        // silent while the player is still moving. That is the gap you hear.
        g_anim_refresh += clamped;
        if (g_anim_refresh >= kAnimRefreshSecs) {
            g_anim_refresh = 0.0f;
            g_current_anim = 0;          // force the re-issue below
        }
        if (want != g_current_anim) {
            if (game::PlayActorAnimGroup(player, want)) {
                g_current_anim = want;
            } else {
                g_anim_works = false;      // guards refused; stop trying
                F3A_INFO("Mover: walk animation unavailable on this build.");
            }
        }
    }

    float beforeX = player->posX, beforeY = player->posY;
    float disp[3] = { 0.0f, speed * clamped, 0.0f };   // local space; engine rotates it
    ActorMove(player, clamped, disp);

    // Step clicks follow the distance ACTUALLY covered, so wedging against a wall
    // silences them — which is the cue that something is wrong.
    bool real_steps = g_anim_works && config::Get().walk_animation;
    if (config::Get().footstep_cue && !real_steps) {
        float mdx = player->posX - beforeX, mdy = player->posY - beforeY;
        g_step_accum += std::sqrt(mdx * mdx + mdy * mdy);
        if (g_step_accum >= kFootstepEvery) {
            g_step_accum = 0.0f;
            g_step_alt = !g_step_alt;
            audio::Cue(g_step_alt ? 150 : 130, 28);
        }
    }

}

} // namespace

// Does the player's own mover already hold a PathingSolution?
//
// This is the question the whole engine-pathfinding plan turns on, and it is
// cheaper to ASK THE GAME than to disassemble for it. The solution object is
// recognisable: its first word is the PathingSolution vtable. So walk the
// mover's fields and report any pointer to such an object, with the offset it
// sits at — the New Vegas mod reads waypoints out of exactly this.
//
// Read-only, bounds-checked, and logged once per walk. If nothing turns up, the
// answer is equally useful: the player's mover does not path on its own and the
// goal has to be set first.
void ProbeForPathingSolution(void* mover)
{
    constexpr uintptr_t kPathingSolutionVtable = 0x00E0272C;
    if (!mover || IsBadReadPtr(mover, 0x200)) {
        F3A_INFO("PathProbe: mover unreadable");
        return;
    }
    auto* base = reinterpret_cast<UInt8*>(mover);
    int found = 0;
    for (uintptr_t off = 0; off + 4 <= 0x200; off += 4) {
        void* cand = *reinterpret_cast<void**>(base + off);
        if (!cand || IsBadReadPtr(cand, 0x44)) continue;
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(cand);
        if (vt != kPathingSolutionVtable) continue;
        // The +0x20 sub-array is the waypoint list: data at +0x24, count at
        // +0x28 (worked out earlier from the follow-math that reads it).
        void*  data = *reinterpret_cast<void**>(reinterpret_cast<UInt8*>(cand) + 0x24);
        UInt32 n    = *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(cand) + 0x28);
        F3A_INFO("PathProbe: PathingSolution at mover+0x%02X -> %p, waypoints "
                 "data=%p count=%u", (unsigned)off, cand, data, n);
        ++found;
    }
    if (!found)
        F3A_INFO("PathProbe: no PathingSolution anywhere in the player's mover "
                 "(first 0x200 bytes) — it does not path on its own.");
}

void ProbePath()
{
    if (!g_installed.load()) {
        F3A_INFO("PathProbe: the mover hook is not installed (NativeWalk=0?)");
        return;
    }
    ProbeForPathingSolution(g_last_mover);
}

bool Install()
{
    if (g_installed.load()) return true;

    auto* slot = reinterpret_cast<uintptr_t*>(kUpdateSlot);
    if (IsBadReadPtr(slot, sizeof(uintptr_t))) {
        F3A_INFO("Mover: vtable slot unreadable — key-based walking stays in use.");
        return false;
    }
    // Refuse to patch anything but the function we identified, so a different
    // build simply keeps the old behaviour instead of crashing.
    if (*slot != kExpectedUpdate) {
        F3A_INFO("Mover: Update slot holds 0x%08X, expected 0x%08X — not hooking.",
                 (unsigned)*slot, (unsigned)kExpectedUpdate);
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &old)) {
        F3A_INFO("Mover: VirtualProtect failed — not hooking.");
        return false;
    }
    g_original = reinterpret_cast<UpdateFn>(*slot);
    *slot = reinterpret_cast<uintptr_t>(&Update_Hook);
    VirtualProtect(slot, sizeof(uintptr_t), old, &old);

    g_installed.store(true);
    F3A_INFO("Mover: hooked PlayerMover::Update (slot 5) at 0x%08X.",
             (unsigned)kExpectedUpdate);
    return true;
}

void Shutdown()
{
    if (!g_installed.load()) return;
    g_active.store(false);
    auto* slot = reinterpret_cast<uintptr_t*>(kUpdateSlot);
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &old)) {
        *slot = reinterpret_cast<uintptr_t>(g_original);
        VirtualProtect(slot, sizeof(uintptr_t), old, &old);
    }
    g_installed.store(false);
}

bool Available() { return g_installed.load(); }
bool Active()    { return g_active.load(); }

void SetGoalWithCorner(float x, float y, bool run, float cornerX, float cornerY)
{
    g_corner_x.store(cornerX);
    g_corner_y.store(cornerY);
    g_corner_on.store(std::isfinite(cornerX) && std::isfinite(cornerY));
    g_goal_x.store(x);
    g_goal_y.store(y);
    g_run.store(run);
    g_goal_stamp.fetch_add(1);      // feeds the watchdog in the hook
    if (!g_active.exchange(true))
        g_reseed_pending.store(true);   // the hook resets its state (next frame)
}

void SetGoal(float x, float y, bool run)
{
    g_corner_on.store(false);
    g_goal_x.store(x);
    g_goal_y.store(y);
    g_run.store(run);
    g_goal_stamp.fetch_add(1);      // feeds the watchdog in the hook
    if (!g_active.exchange(true))
        g_reseed_pending.store(true);   // the hook resets its state (next frame)
}

void Clear()
{
    // No early-out when we were already inactive. The watchdog in the hook
    // clears g_active by itself when a goal stops being refreshed, and after
    // that this function used to do NOTHING — so a walk that ended that way,
    // and then a player pressing the stop key, cleared no flags, played no idle
    // animation and ran no brake. The mod said "Zatrzymano" and nothing in the
    // game was told to stop. A stop request is cheap; swallowing one is not.
    g_active.store(false);
    // Hand the actual stopping to the hook: it runs on the game's thread and is
    // given the engine's own mover, so nothing here has to guess an offset or
    // call into the game from the polling thread. The very next frame clears the
    // movement bits before the engine's Update gets to act on them.
    g_stop_pending.store(true);
}

void RequestEnginePath(const void* refr, uint32_t refid, float radius)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    ++g_ep_seq;                       // whatever was being worked is now stale
    g_ep_points.clear();
    if (!g_installed.load() || !config::Get().engine_path) { g_ep_state.store(3); return; }
    g_ep_ref = refr; g_ep_refid = refid; g_ep_radius = radius;
    g_ep_req_seq = g_ep_seq;
    g_ep_state.store(1);
    g_ep_request.store(1);
}

void CancelEnginePath()
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    ++g_ep_seq;
    g_ep_request.store(0);
    g_ep_cancel.store(1);
    g_ep_points.clear();
    g_ep_state.store(0);
}

int EnginePathState() { return g_ep_state.load(); }

bool TakeEnginePath(std::vector<game::Vec3>* out)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    int st = g_ep_state.load();
    if (st != 2 && st != 3) return false;
    if (out) *out = std::move(g_ep_points);
    g_ep_points.clear();
    g_ep_state.store(0);
    return true;
}

float CurrentHeadingDeg()
{
    float d = g_heading * 57.2957795f;
    if (d < 0.0f) d += 360.0f;
    return d;
}

} // namespace f3a::mover
