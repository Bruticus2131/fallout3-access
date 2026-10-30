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

// The player's own walk and run speeds, as the FNV mod takes them. Actor
// vtable +0x448 is CalculateWalkSpeed (0x006FB200) and +0x44C
// CalculateRunSpeed (0x006FB2E0) - New Vegas's +0x458/+0x45C - found through
// the formulas they call: the walk one hands IsSneaking (0x0045C200) and the
// load carried to 0x005845B0, which applies fMoveSneakMult itself; the run
// one multiplies by fMoveRunMult. Called only while the live vtable holds
// exactly these.
constexpr uintptr_t kWalkSpeedSlot = 0x448, kWalkSpeedFn = 0x006FB200;
constexpr uintptr_t kRunSpeedSlot  = 0x44C, kRunSpeedFn  = 0x006FB2E0;
// Always run, which Caps Lock toggles: PlayerCharacter+0x5AF. The player's
// input update flips it on the AlwaysRun control (0x00788E27) and runs when
// it is on and the Run key is not held, or the other way round (0x007897A9);
// the constructor fills it from bAlwaysRunByDefault (0x0077AD8A).
constexpr uintptr_t kAlwaysRunOffset = 0x5AF;
constexpr uint32_t  kControlRun      = 9;
// The game's own Auto Move (control 11): PlayerCharacter+0x5B0, toggled by
// the input handler (0x00788E46). While it is set and no movement control is
// pressed, the handler holds Forward itself every frame (0x00788EAD, the
// OSInputGlobals "control held" 0x00BE9590); a movement control clears it
// (0x00788EB4). Walking by it, the ENGINE moves the player - its own speed,
// walk or run as Caps Lock and the Run key say, sneaking, its animations and
// footsteps - and the walk only turns the player toward the route. Used only
// where those bytes are the ones disassembled; else the old way (Actor::Move).
constexpr uintptr_t kAutoMoveOffset = 0x5B0;

// Turning. The walk never slows down: it moves at the game's own walk or run
// speed and nothing else (the player's wish, 2026-10-30 - the turn bands and
// the turning-circle cap braked every bend). So the body turns about as fast
// as a mouse turns it, ~690 degrees a second: at a run that is a turning
// circle of ~30 units, tight enough for a doorway.
constexpr float kTurnRateRadPerSec = 12.0f;
constexpr float kPi    = 3.14159265f;
constexpr float kTwoPi = 6.28318530f;

// Following a stretch of route (SetRoute), frame by frame. The rules are the
// walk module's own (autowalk's AdvanceWaypoints and LookAheadGoal), moved
// here so the point steered at is worked out every frame: aim a little ahead
// along the route, by the turning circle (the FNV mod's 1.5 x, 48 to 96
// units), never round a corner sharper than 60 degrees, and barely at all
// where the route climbs or drops - stairs, ramps, walkways with no rails.
constexpr float kLookAheadMin   = 48.0f;
constexpr float kLookAheadMax   = 96.0f;
constexpr float kLookAheadTight = 12.0f;
constexpr float kRouteClimb     = 24.0f;
constexpr float kSharpCornerCos = 0.5f;
constexpr float kRouteRadiusMin = 48.0f;
constexpr float kRouteFloorDelta = 160.0f;

std::mutex            g_rt_mx;
game::Vec3            g_rt_pts[kRouteMax];   // under g_rt_mx
size_t                g_rt_n = 0;            // under g_rt_mx
std::atomic<uint32_t> g_rt_stamp{ 0 };       // bumped under g_rt_mx
std::atomic<bool>     g_rt_on{ false };      // the goal is a route, not a point
// The hook's own copy, and the leg of it being walked.
game::Vec3 h_rt[kRouteMax];
size_t     h_rt_n = 0, h_rt_seg = 0;
uint32_t   h_rt_seen = 0;

std::atomic<float> g_speed{ 0.0f };          // the walk's speed now (CurrentSpeed)
std::atomic<bool>  g_speed_unreadable{ false };   // the game's speed could not be read

using UpdateFn = void (__thiscall*)(void* mover, float dt);
UpdateFn g_original = nullptr;

std::atomic<bool>  g_installed{ false };
std::atomic<bool>  g_active{ false };
std::atomic<float> g_goal_x{ 0.0f };
std::atomic<float> g_goal_y{ 0.0f };
std::atomic<bool>  g_run{ false };
// A goal to stop ON rather than walk through (see SetGoalPrecise): within this
// many units the hook holds still. 0 = an ordinary goal.
std::atomic<float> g_goal_stop{ 0.0f };
// Hook thread: the spot the walk last stopped at (NaN: none) - started again
// from there only on a clear miss.
float g_spot_x = NAN, g_spot_y = NAN;
float g_heading = 0.0f;          // radians, engine convention (0 = +Y/north)
bool  g_heading_seeded = false;
// A new walk was started from the poll thread: the hook re-seeds its own
// heading/footstep/animation state on its next frame (those are its own).
std::atomic<bool> g_reseed_pending{ false };
bool g_speed_log = false;        // hook thread: say the speed once the walk starts

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
float g_anim_refresh = 0.0f;              // seconds since the movement slot was looked at
constexpr float kAnimCheckSecs = 0.0f;    // every frame: two pointer reads
bool  g_pace_said = false;                // the cycle's pace logged this walk
bool  g_anim_sneaking = false;            // the posture the walk anim was played in
int   g_anim_dropped_said = 0;            // "dropped"/"wrong cycle" log lines this walk
float g_anim_cool = 0.0f;                 // s before the next replay is allowed
// Walking or running, as the walk goes: a change counts only once it has
// held a moment. Something flipped it for a frame or two about a second into
// every walk (log 2026-10-29), and the run cycle played after that blip did
// not take - the walk cycle stayed, slow steps under a run.
bool  g_run_state = true;
bool  g_run_seeded = false;
float g_run_flip = 0.0f;                  // s the other state has held
constexpr float kRunFlipSecs = 0.25f;
// How fast the player really goes, measured: the first few 2-second windows
// of each walk are logged against the speed asked for.
float g_meas_t = 0.0f, g_meas_d = 0.0f;
int   g_meas_said = 0;
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

// The game's walk or run speed for the player, -1 when it cannot be had.
float GameMoveSpeed(void* player, bool running)
{
    void** vt = *reinterpret_cast<void***>(player);
    if (!vt || IsBadReadPtr(vt, kRunSpeedSlot + 4)) return -1.0f;
    const uintptr_t fn = running ? kRunSpeedFn : kWalkSpeedFn;
    if (reinterpret_cast<uintptr_t>(vt[(running ? kRunSpeedSlot : kWalkSpeedSlot) / 4]) != fn)
        return -1.0f;
    using SpeedFn = float (__thiscall*)(void* self);
    const float s = reinterpret_cast<SpeedFn>(fn)(player);
    return std::isfinite(s) && s >= 10.0f && s <= 2000.0f ? s : -1.0f;
}

// Does the player run, as the game would decide it: always run (Caps Lock)
// unless the Run key is held, or the other way round. What changed is logged
// (a few times), so a flip nobody asked for can be traced.
bool PlayerRuns(const void* player)
{
    auto* p = reinterpret_cast<const uint8_t*>(player);
    if (IsBadReadPtr(p + kAlwaysRunOffset, 1)) return true;
    const uint8_t always = p[kAlwaysRunOffset];
    if (always > 1) return true;          // not the flag it should be: run, as before
    const bool held = game::IsControlKeyHeld(kControlRun);
    static int s_always = -1, s_held = -1, s_said = 0;
    if ((always != s_always || (int)held != s_held) && s_always >= 0 && s_said < 12) {
        ++s_said;
        F3A_INFO("Mover: always run %u -> %u, Run key %s", (unsigned)s_always,
                 (unsigned)always, held ? "held" : "not held");
    }
    s_always = always; s_held = held;
    return (always != 0) != held;
}

// The point to steer at along the route SetRoute gave, and the route's next
// real corner (for braking). False when there is no route to follow.
bool RouteSteer(const TESObjectREFR* player, float speed, float* gx, float* gy,
                float* cx, float* cy)
{
    const uint32_t stamp = g_rt_stamp.load();
    if (stamp != h_rt_seen) {
        // A new stretch from the walk. Taken when the lock is free; a frame
        // on the previous copy costs nothing.
        std::unique_lock<std::mutex> lk(g_rt_mx, std::try_to_lock);
        if (lk.owns_lock()) {
            h_rt_n = g_rt_n;
            for (size_t i = 0; i < h_rt_n; ++i) h_rt[i] = g_rt_pts[i];
            h_rt_seen = g_rt_stamp.load();
            h_rt_seg  = 0;
        }
    }
    if (h_rt_n < 2) return false;
    const float px = player->posX, py = player->posY, pz = player->posZ;
    // The look-ahead keeps the geometry it was tuned with (a turning circle
    // of speed / 4 rad/s, at most 64 units), whatever the body turns at now.
    const float turn   = (speed < 256.0f ? speed : 256.0f) / 4.0f;
    const float radius = turn * 1.2f > kRouteRadiusMin ? turn * 1.2f : kRouteRadiusMin;

    // On past the points reached, or passed beside (the last one stays).
    while (h_rt_seg + 2 < h_rt_n) {
        const auto& a = h_rt[h_rt_seg];
        const auto& w = h_rt[h_rt_seg + 1];
        if (std::fabs(w.z - pz) > kRouteFloorDelta) break;   // fallen off: not walked
        const float dx = w.x - px, dy = w.y - py;
        bool on = dx * dx + dy * dy <= radius * radius;
        if (!on) {
            const float sx = w.x - a.x, sy = w.y - a.y, len2 = sx * sx + sy * sy;
            if (len2 > 1.0f) {
                const float t  = ((px - a.x) * sx + (py - a.y) * sy) / len2;
                const float lx = a.x + t * sx - px, ly = a.y + t * sy - py;
                on = t >= 0.92f && lx * lx + ly * ly <= 4.0f * radius * radius;
            }
        }
        if (!on) break;
        ++h_rt_seg;
    }

    const auto& a = h_rt[h_rt_seg];
    const auto& b = h_rt[h_rt_seg + 1];
    *cx = b.x; *cy = b.y;
    const float abx = b.x - a.x, aby = b.y - a.y, len2 = abx * abx + aby * aby;
    float t = len2 > 1.0f ? ((px - a.x) * abx + (py - a.y) * aby) / len2 : 1.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float curX = a.x + abx * t, curY = a.y + aby * t;

    bool climbs = false;
    for (size_t i = h_rt_seg; i + 1 < h_rt_n && i < h_rt_seg + 3; ++i)
        if (std::fabs(h_rt[i + 1].z - h_rt[i].z) > kRouteClimb) climbs = true;
    float remaining = turn * 1.5f;
    if (remaining < kLookAheadMin) remaining = kLookAheadMin;
    if (remaining > kLookAheadMax) remaining = kLookAheadMax;
    if (climbs) remaining = kLookAheadTight;

    size_t next = h_rt_seg + 1;
    float nx = b.x, ny = b.y;
    for (int guard = 0; guard < (int)kRouteMax; ++guard) {
        const float dx = nx - curX, dy = ny - curY;
        const float seg = std::sqrt(dx * dx + dy * dy);
        if (remaining <= seg || next + 1 >= h_rt_n) {
            float f = seg > 1.0f ? remaining / seg : 1.0f;
            if (f > 1.0f) f = 1.0f;
            *gx = curX + dx * f; *gy = curY + dy * f;
            return true;
        }
        const auto& o = h_rt[next + 1];
        const float ox = o.x - nx, oy = o.y - ny;
        const float olen = std::sqrt(ox * ox + oy * oy);
        if (seg > 1.0f && olen > 1.0f && (dx * ox + dy * oy) / (seg * olen) < kSharpCornerCos) {
            *gx = nx; *gy = ny;                    // stop at a sharp corner
            return true;
        }
        remaining -= seg;
        curX = nx; curY = ny;
        ++next;
        nx = o.x; ny = o.y;
    }
    *gx = nx; *gy = ny;
    return true;
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

// The engine walks (see kAutoMoveOffset): checked once.
int  g_automove_ok = -1;           // -1 untested, 0 no, 1 yes
std::atomic<bool> g_auto_moving{ false };   // we set the game's auto-move on
std::atomic<bool> g_engine_walk{ false };   // the walk going on is the engine's
int  g_saved_always_run = -1;      // always run as it was before a walk-only stretch
std::atomic<bool> g_user_took_over{ false };
// Frames left in which a loaded save's leftover walking is cleared (see the
// hook). Armed by OnGameLoaded.
std::atomic<int>  g_after_load{ 0 };
constexpr int     kAfterLoadFrames = 30;

bool AutoMoveReady()
{
    if (g_automove_ok >= 0) return g_automove_ok != 0;
    // cmp byte [ebx+0x5B0],0 / je  (the per-frame auto-move test), and
    // mov byte [ebx+0x5B0],0 (a movement control cancels it).
    static const uint8_t test[] = { 0x80, 0xBB, 0xB0, 0x05, 0x00, 0x00, 0x00, 0x74 };
    static const uint8_t clear[] = { 0xC6, 0x83, 0xB0, 0x05, 0x00, 0x00, 0x00 };
    const auto* a = reinterpret_cast<const uint8_t*>(0x00788E56);
    const auto* b = reinterpret_cast<const uint8_t*>(0x00788EB4);
    g_automove_ok = !IsBadReadPtr(a, sizeof(test)) && !IsBadReadPtr(b, sizeof(clear)) &&
                    std::memcmp(a, test, sizeof(test)) == 0 &&
                    std::memcmp(b, clear, sizeof(clear)) == 0;
    F3A_INFO("Mover: %s", g_automove_ok ? "the game's own auto-move drives the walk"
                                        : "auto-move code differs - the walk moves the player itself");
    return g_automove_ok != 0;
}

bool AutoMoveOn(const void* player)
{
    const auto* b = reinterpret_cast<const uint8_t*>(player) + kAutoMoveOffset;
    return !IsBadReadPtr(b, 1) && *b != 0;
}

void SetAutoMove(void* player, bool on)
{
    auto* b = reinterpret_cast<uint8_t*>(player) + kAutoMoveOffset;
    if (IsBadWritePtr(b, 1)) return;
    *b = on ? 1 : 0;
    g_auto_moving = on;
}

// A stretch to be walked, not run (a spot to stop on): always run off for
// it, put back as it was afterwards. The Run key still runs, as in the game.
void HoldWalk(void* player, bool walk)
{
    auto* ar = reinterpret_cast<uint8_t*>(player) + kAlwaysRunOffset;
    if (IsBadWritePtr(ar, 1)) return;
    if (walk) {
        if (g_saved_always_run < 0 && *ar <= 1) { g_saved_always_run = *ar; *ar = 0; }
    } else if (g_saved_always_run >= 0) {
        *ar = (uint8_t)g_saved_always_run;
        g_saved_always_run = -1;
    }
}

// Returns true when it was the game's auto-move that was stopped: the engine
// stops the player by itself then, and nothing has to be braked.
bool StopNow(void* mover);
bool StopEngineWalk()
{
    const bool engine = g_engine_walk.exchange(false);
    if (!engine && !g_auto_moving && g_saved_always_run < 0) return false;
    if (auto* p = rt::Player()) {
        SetAutoMove(p, false);
        HoldWalk(p, false);
        g_watch       = 0.0f;
        g_watch_stage = 0;
        g_watch_x     = p->posX;
        g_watch_y     = p->posY;
    }
    g_auto_moving = false;
    F3A_INFO("Mover: stop - the game's auto-move is off.");
    return true;
}

bool StopNow(void* mover)
{
    if (StopEngineWalk()) return true;
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
            // The cycle's pace back to what the walk found (SetMovementAnimPace).
            game::RestoreMovementAnimPace(p);
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
    return false;
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
// The same goal set from a POSITION — what SetPathfindingGoal itself calls
// after reading the reference's position, cell and worldspace (the engine
// uses it directly from 20+ AI sites). With a worldspace given, the cell is
// ignored and the grid cell comes from the position.
constexpr uintptr_t kSetPathfindingGoalPos = 0x006F8A70;
using SetGoalPosFn = bool (__thiscall*)(void* actor, const float* pos, void* cell,
                                        void* ws, float radius, void* avoid);
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

// The position variant, checked on its own: routes to a point need it, routes
// to a reference do not.
int g_ep_pos_verified = -1;
bool EnginePathPosOk()
{
    if (g_ep_pos_verified < 0) {
        static const unsigned char kPos[] = { 0x8B,0x44,0x24,0x14,0x85,0xC0,0x56,0x8B,
                                              0xF1,0x75,0x0E,0x8B,0x4E,0x60,0x8B,0x01 };
        bool ok = !IsBadReadPtr(reinterpret_cast<void*>(kSetPathfindingGoalPos), 16) &&
                  std::memcmp(reinterpret_cast<void*>(kSetPathfindingGoalPos), kPos, 16) == 0;
        g_ep_pos_verified = ok ? 1 : 0;
        F3A_INFO("EnginePath: the point variant %s", ok ? "is verified"
                 : "does not match this build - routes to points disabled");
    }
    return g_ep_pos_verified == 1;
}

std::mutex              g_ep_mutex;       // guards the request and the result
const void*             g_ep_ref    = nullptr;
uint32_t                g_ep_refid  = 0;
float                   g_ep_radius = 100.0f;
bool                    g_ep_to_point = false;   // a point, not a reference
float                   g_ep_point[3] = { 0.0f, 0.0f, 0.0f };
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
bool g_ep_crossed = false;   // last result went on into another area (mutex)

void EnginePathPublish(std::vector<game::Vec3>&& pts, int state, bool crossed = false)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    if (g_ep_work_seq != g_ep_seq) {
        F3A_INFO("EnginePath: result of an older request dropped");
        return;
    }
    g_ep_points  = std::move(pts);
    g_ep_crossed = crossed;
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
        bool toPoint; float pt[3];
        {
            std::lock_guard<std::mutex> lk(g_ep_mutex);
            ref = g_ep_ref; refid = g_ep_refid; radius = g_ep_radius;
            toPoint = g_ep_to_point;
            pt[0] = g_ep_point[0]; pt[1] = g_ep_point[1]; pt[2] = g_ep_point[2];
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
        const void* pcell = nullptr; const void* pws = nullptr;
        if (toPoint) {
            if (!EnginePathPosOk()) { EnginePathFail("routes to points unavailable"); return; }
            // "Must be finite": NaN makes the engine's grid lookup invalid.
            if (!(std::isfinite(pt[0]) && std::isfinite(pt[1]) && std::isfinite(pt[2]))) {
                EnginePathFail("the point is not a real position"); return;
            }
            PlayerArea(&pcell, &pws);
            if (!pcell) { EnginePathFail("the player has no cell"); return; }
        } else {
            auto* r = reinterpret_cast<const TESObjectREFR*>(ref);
            if (!r || IsBadReadPtr(r, 0x40) || r->refID != refid || !r->parentCell) {
                EnginePathFail("target reference is gone"); return;
            }
        }
        auto* m = reinterpret_cast<UInt8*>(mover);
        if (IsBadReadPtr(m, 0x80)) { EnginePathFail("mover unreadable"); return; }

        reinterpret_cast<FullResetFn>(kMoverFullReset)(mover);
        bool ok;
        if (toPoint) {
            // The player's own area: the point is in it (the walk asks only for
            // points in the space it is walking).
            ok = reinterpret_cast<SetGoalPosFn>(kSetPathfindingGoalPos)(
                     player, pt, const_cast<void*>(pcell), const_cast<void*>(pws),
                     radius, nullptr);
        } else {
            ok = reinterpret_cast<SetGoalFn>(kSetPathfindingGoal)(
                     player, const_cast<void*>(ref), radius, nullptr);
        }
        UInt8 inflight = m[0x74];
        if (toPoint)
            F3A_INFO("EnginePath: SetPathfindingGoal(point %.0f,%.0f,%.0f, r=%.0f) -> %d, "
                     "in flight %d", pt[0], pt[1], pt[2], radius, (int)ok, (int)inflight);
        else
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
    EnginePathPublish(std::move(pts), any ? 2 : 3, otherArea > 0);
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
    if (g_stop_pending.exchange(false))
        g_brake = StopNow(mover) ? 0 : kBrakeFrames;

    // Just after a game was loaded. A save made during a walk keeps the
    // game's auto-move on (a quick save: no menu, the walk still going) or
    // the mover's "moving forward" state (a save from the pause menu, where
    // the game stands still before a frame can clear it) - and the loaded
    // player walked on, or ran in place, with nothing driving them (user,
    // 2026-10-30). Cleared in the first frames, unless a walk has started
    // or the player is pressing a movement key.
    if (const int left = g_after_load.load(); left > 0) {
        g_after_load.store(left - 1);
        auto* p = rt::Player();
        bool keys = false;
        for (uint32_t c = 0; c < 4; ++c) keys = keys || game::IsControlKeyHeld(c);
        if (p && !g_active.load() && !keys && !IsBadReadPtr(mover, 0x98)) {
            const auto* mf = reinterpret_cast<const uint8_t*>(mover);
            const uint32_t f1 = *reinterpret_cast<const uint32_t*>(mf + kMoverFlags1);
            const uint32_t f2 = *reinterpret_cast<const uint32_t*>(mf + kMoverFlags2);
            const bool autoMove = AutoMoveOn(p);
            // A direction (forward, back, the sides), not the walk/run mode
            // bits: 0x200 stays set on a player standing with always run on.
            const bool moving = ((f1 | f2) & 0x0F) != 0;
            bool ok = false;
            int group = -1;
            game::ActorMovementSequence(p, &ok, &group);
            const bool cycle = ok && (group == (int)kAnimForward || group == (int)kAnimFastForward);
            if (autoMove || moving || cycle) {
                static int s_said = 0;
                if (s_said++ < 4)
                    F3A_INFO("Mover: the loaded save was still walking (auto-move %d, movement "
                             "flags %08X / %08X, walk cycle %d) - stopped", (int)autoMove, f1, f2,
                             (int)cycle);
                if (autoMove) SetAutoMove(p, false);
                SetMoverFlags(mover, false, false);
                EngineStopMover(mover);
                if (cycle) {
                    game::StopActorMovementAnim(p);
                    game::PlayActorAnimGroup(p, kAnimIdle);
                }
            }
        }
    }
    // A walk (re)started by SetGoal: fresh state, set here on the thread that
    // owns it rather than written from the poll thread mid-frame.
    if (g_reseed_pending.exchange(false)) {
        g_heading_seeded = false;    // seed on this frame
        g_step_accum     = 0.0f;
        g_current_anim   = kAnimIdle;  // force the walk anim to be issued
        g_watch_stage    = 3;          // a new walk ends the previous stop's probe
        g_speed_log      = true;
        g_anim_dropped_said = 0;
        g_meas_t = g_meas_d = 0.0f;
        g_meas_said = 0;
        g_pace_said = false;
        g_run_seeded = false;
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

    // How fast: the player's own way of moving holds during a walk, as in the
    // FNV mod. Running or walking as Caps Lock (always run) and the Run key
    // say, at the game's own speed for it. Crouched (the 0x400 posture bit of
    // the movement flags; 0x800 is swimming) the walk creeps instead of
    // running - running stood the player up and gave them away - and carrying
    // more than they can (inventory weight, AV 46, over carry weight, AV 13)
    // it walks, as the game makes them. The engine picks the crouched variant
    // of the walk animation itself from the posture bit, and the game's walk
    // speed already counts the crouch.
    bool running = false;
    if (g_run.load()) {
        const bool now = PlayerRuns(player);
        if (!g_run_seeded) { g_run_seeded = true; g_run_state = now; g_run_flip = 0.0f; }
        if (now != g_run_state) {
            g_run_flip += clamped;
            if (g_run_flip >= kRunFlipSecs) { g_run_state = now; g_run_flip = 0.0f; }
        } else {
            g_run_flip = 0.0f;
        }
        running = g_run_state;
    }
    const auto* mf = reinterpret_cast<const uint8_t*>(mover);
    const uint32_t f1 = *reinterpret_cast<const uint32_t*>(mf + kMoverFlags1);
    const uint32_t f2 = *reinterpret_cast<const uint32_t*>(mf + kMoverFlags2);
    const bool sneaking = (f1 & 0xC00) == 0x400 || (f2 & 0xC00) == 0x400;
    const float carried = game::GetPlayerAV(46), carry = game::GetPlayerAV(13);
    const bool overloaded = carry > 0.0f && carried > carry;
    static bool s_said_sneak = false, s_said_load = false;
    if (sneaking != s_said_sneak || overloaded != s_said_load) {
        s_said_sneak = sneaking; s_said_load = overloaded;
        F3A_INFO("Mover: sneaking=%d overloaded=%d (flags %08X / %08X)",
                 (int)sneaking, (int)overloaded, f1, f2);
    }
    if (sneaking || overloaded) running = false;
    // The game's speed and no other: when it cannot be read (another build
    // of the game), the walk stops instead of making one up.
    const bool engineWalk = config::Get().native_walk && AutoMoveReady();
    float speed = GameMoveSpeed(player, running);
    if (engineWalk && !(speed > 0.0f)) speed = g_speed.load() > 0.0f ? g_speed.load() : 150.0f;
    if (!(speed > 0.0f)) {
        F3A_WARN("Mover: the game's %s speed cannot be read - the walk stops.",
                 running ? "run" : "walk");
        g_speed_unreadable.store(true);
        g_active.store(false);
        StopNow(mover);
        if (g_original) g_original(mover, dt);
        return;
    }
    g_speed.store(speed);
    {
        static int   s_said_run = -1;
        static float s_said_speed = 0.0f;
        if (g_speed_log || (int)running != s_said_run ||
            std::fabs(speed - s_said_speed) > 0.1f * s_said_speed) {
            g_speed_log = false;
            g_pace_said = false;            // and the cycle's pace with it
            s_said_run = (int)running; s_said_speed = speed;
            F3A_INFO("Mover: %s at %.0f units/s (the game's own speed)",
                     running ? "running" : "walking", speed);
        }
    }

    float gx = g_goal_x.load();
    float gy = g_goal_y.load();
    // Along a route: the point a little ahead on it, worked out now.
    if (g_rt_on.load()) {
        float rx, ry, rcx, rcy;
        if (RouteSteer(player, speed, &rx, &ry, &rcx, &rcy)) { gx = rx; gy = ry; }
    }
    float dx = gx - player->posX;
    float dy = gy - player->posY;
    // A goal that is not a number (a bad read somewhere upstream) must never
    // reach the player's heading: stop instead.
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        F3A_INFO("Mover: the goal is not a number - stopping.");
        g_active.store(false);
        StopNow(mover);
        if (g_original) g_original(mover, dt);
        return;
    }

    if (!g_heading_seeded) { g_heading = player->rotZ; g_heading_seeded = true; }

    // THE ENGINE WALKS. The game's own auto-move holds Forward; the walk only
    // turns the player toward the route, as a player with a mouse would.
    // Speed, walk or run, sneaking, stairs, animations and footsteps are all
    // the game's.
    if (engineWalk) {
        // A movement key cancels the game's auto-move: the player has taken
        // over, and the walk ends.
        if (g_auto_moving && !AutoMoveOn(player)) {
            g_auto_moving = false;
            F3A_INFO("Mover: the game's auto-move was cancelled (a movement key) - the walk stops.");
            g_user_took_over.store(true);
            g_active.store(false);
            StopNow(mover);
            if (g_original) g_original(mover, dt);
            return;
        }
        const float want = std::atan2(dx, dy);
        float turn = NormalizeAngle(want - g_heading);
        const float maxTurn = kTurnRateRadPerSec * clamped;
        if (turn >  maxTurn) turn =  maxTurn;
        if (turn < -maxTurn) turn = -maxTurn;
        g_heading = NormalizeAngle(g_heading + turn);
        if (!game::SteerYawRad(g_heading)) player->rotZ = g_heading;
        g_engine_walk.store(true);
        // A spot to stop ON (the photo's trigger box): the game stops a
        // walking player within a few units, so it is let go just before the
        // spot - at a fifth of a second's way it stood 35-odd units short,
        // outside the 12 that count as on it. Once stopped, only a clear miss
        // starts it again, so it never paces to and fro over the spot.
        bool go = true;
        const float stopR = g_goal_stop.load();
        if (stopR > 0.0f) {
            const float d = std::sqrt(dx * dx + dy * dy);
            const float lead = stopR + speed * 0.04f;
            const bool stoppedHere = std::fabs(gx - g_spot_x) < 1.0f && std::fabs(gy - g_spot_y) < 1.0f;
            go = d > lead + ((!g_auto_moving && stoppedHere) ? 24.0f : 0.0f);
            if (!go && g_auto_moving) { g_spot_x = gx; g_spot_y = gy; }
        }
        HoldWalk(player, !g_run.load());
        if (go != g_auto_moving) SetAutoMove(player, go);
        const float bx = player->posX, by = player->posY;
        if (g_original) g_original(mover, dt);
        const float mx = player->posX - bx, my = player->posY - by;
        g_meas_d += std::sqrt(mx * mx + my * my);
        g_meas_t += clamped;
        if (g_meas_t >= 2.0f) {
            if (g_meas_said < 3) {
                ++g_meas_said;
                F3A_INFO("Mover: the game moved the player %.0f units in %.1f s = %.0f units/s",
                         g_meas_d, g_meas_t, g_meas_d / g_meas_t);
            }
            g_meas_t = g_meas_d = 0.0f;
        }
        return;
    }

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

    // No braking for turns or corners: the speed stays the game's (see
    // kTurnRateRadPerSec).

    // A spot to stand ON (a trigger box a few dozen units across): never step
    // past it in one frame. The walk module only looks every 80 ms, which at
    // full speed is a stride - so the stopping has to happen here, frame by
    // frame.
    float stopR = g_goal_stop.load();
    if (stopR > 0.0f) {
        float left = std::sqrt(dx * dx + dy * dy);
        if (left <= stopR) {
            // On the spot: hold still. Lowering the flags alone leaves the
            // actor its last motion (see the brake above), so a zero move
            // takes it away, and the walk animation gives way to idle once.
            SetMoverFlags(mover, false, false);
            ClearMoveDirection(mover);
            float zero[3] = { 0.0f, 0.0f, 0.0f };
            ActorMove(player, clamped, zero);
            if (g_current_anim != kAnimIdle) {
                game::StopActorMovementAnim(player);
                game::PlayActorAnimGroup(player, kAnimIdle);
                g_current_anim = kAnimIdle;
            }
            return;
        }
        float maxStep = (left - stopR * 0.5f) / (clamped > 0.0f ? clamped : 0.016f);
        if (speed > maxStep) speed = maxStep > 0.0f ? maxStep : 0.0f;
    }

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
        // Whether the animation is still playing is the ENGINE's business — a
        // collision or a posture change can drop it — and our own note saying
        // "already playing" then goes stale. The footsteps stop with it, and
        // since the synthesized clicks are suppressed while the real ones are
        // expected, the walk goes silent while the player is still moving. So
        // the movement slot is looked at every frame, and the cycle is played
        // again when it is empty or the game has put another one in its
        // place - at most twice a second. It used to be replayed every
        // 1.5 s whatever, which restarted a cycle that was playing fine; and
        // replaying only an EMPTY slot left the game's own choice playing, a
        // slow walk's footsteps while the player ran (log 2026-10-29).
        // Checked by the cycle's GROUP, not by which object plays it: a run
        // played just after a walk did not take, the walk cycle stayed in
        // the slot as a new object of the walk's group, and a check by
        // object saw nothing wrong (log 2026-10-29).
        bool clearSlot = false;
        g_anim_refresh += clamped;
        if (g_anim_cool > 0.0f) g_anim_cool -= clamped;
        if (g_anim_refresh >= kAnimCheckSecs && want == g_current_anim && g_anim_cool <= 0.0f) {
            g_anim_refresh = 0.0f;
            bool ok = false;
            int group = -1;
            const void* seq = game::ActorMovementSequence(player, &ok, &group);
            if (ok && (!seq || group != (int)want)) {
                if (g_anim_dropped_said < 6) {
                    ++g_anim_dropped_said;
                    F3A_INFO("Mover: the movement slot holds %s (group %d), not the %s cycle "
                             "- playing it again", seq ? "another cycle" : "nothing", group,
                             running ? "run" : "walk");
                }
                g_current_anim = 0;          // re-issued below
                clearSlot = seq != nullptr;
            }
        }
        // Crouching or standing up mid-walk: the same group, the other
        // variant of it, which the engine picks when the group is played.
        if (sneaking != g_anim_sneaking) { g_current_anim = 0; clearSlot = true; }
        if (want != g_current_anim) {
            // From a walk to a run or back, or a cycle that did not take:
            // the slot is emptied first, so the new cycle is not left waiting
            // behind the old one.
            if (clearSlot || g_current_anim == kAnimForward || g_current_anim == kAnimFastForward)
                game::StopActorMovementAnim(player);
            if (game::PlayActorAnimGroup(player, want)) {
                g_current_anim = want;
                g_anim_sneaking = sneaking;
                g_anim_refresh = 0.0f;
                g_anim_cool = 0.25f;
            } else {
                g_anim_works = false;      // guards refused; stop trying
                F3A_INFO("Mover: walk animation unavailable on this build.");
            }
        }
        // And in step with how fast the player really goes, this frame -
        // braking for a turn included. The game sets this pace only when it
        // starts a walk or run itself; ours ran at whatever pace was left,
        // which is why the steps were slow under a full run.
        float pace = 0.0f, animSpeed = 0.0f, before = 0.0f;
        if (game::SetMovementAnimPace(player, speed, &pace, &animSpeed, &before) && !g_pace_said) {
            g_pace_said = true;
            F3A_INFO("Mover: %s cycle paced x%.2f (its own speed %.0f units/s; it was x%.2f)",
                     running ? "run" : "walk", pace, animSpeed, before);
        }
    }

    float beforeX = player->posX, beforeY = player->posY;
    float disp[3] = { 0.0f, speed * clamped, 0.0f };   // local space; engine rotates it
    ActorMove(player, clamped, disp);

    {
        const float mx = player->posX - beforeX, my = player->posY - beforeY;
        g_meas_d += std::sqrt(mx * mx + my * my);
        g_meas_t += clamped;
        if (g_meas_t >= 2.0f) {
            if (g_meas_said < 3) {
                ++g_meas_said;
                F3A_INFO("Mover: moved %.0f units in %.1f s = %.0f units/s (asked %.0f, "
                         "before braking for turns)", g_meas_d, g_meas_t, g_meas_d / g_meas_t,
                         g_speed.load());
            }
            g_meas_t = g_meas_d = 0.0f;
        }
    }

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
    StopEngineWalk();            // never leave the game walking by itself
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

void SetRoute(const game::Vec3* pts, size_t n)
{
    if (!pts || n == 0) return;
    if (n > kRouteMax) n = kRouteMax;
    {
        std::lock_guard<std::mutex> lk(g_rt_mx);
        for (size_t i = 0; i < n; ++i) g_rt_pts[i] = pts[i];
        g_rt_n = n;
        g_rt_stamp.fetch_add(1);
    }
    g_goal_stop.store(0.0f);
    // Where the leg being walked ends: what the hook steers at until it has
    // its copy of the route (and what the heading readout names).
    const auto& end = pts[n > 1 ? 1 : 0];
    g_goal_x.store(end.x);
    g_goal_y.store(end.y);
    g_run.store(true);
    g_rt_on.store(true);
    g_goal_stamp.fetch_add(1);      // feeds the watchdog in the hook
    if (!g_active.exchange(true))
        g_reseed_pending.store(true);   // the hook resets its state (next frame)
}

float CurrentSpeed()
{
    const float s = g_speed.load();
    return s > 0.0f ? s : 0.0f;
}

bool SpeedUnreadable() { return g_speed_unreadable.exchange(false); }

bool UserTookOver() { return g_user_took_over.exchange(false); }

void OnGameLoaded() { g_after_load.store(kAfterLoadFrames); }

void SetGoalWithCorner(float x, float y, bool run, float, float)
{
    SetGoal(x, y, run);   // the corner only braked the walk, which it no longer does
}

void SetGoal(float x, float y, bool run)
{
    g_rt_on.store(false);
    g_goal_stop.store(0.0f);
    g_goal_x.store(x);
    g_goal_y.store(y);
    g_run.store(run);
    g_goal_stamp.fetch_add(1);      // feeds the watchdog in the hook
    if (!g_active.exchange(true))
        g_reseed_pending.store(true);   // the hook resets its state (next frame)
}

void SetGoalPrecise(float x, float y, float stopRadius)
{
    g_rt_on.store(false);
    g_goal_x.store(x);
    g_goal_y.store(y);
    g_run.store(false);
    g_goal_stop.store(stopRadius > 1.0f ? stopRadius : 1.0f);
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
    // The game's auto-move off at once: under a menu the hook does not run,
    // and the player would walk on for a frame when it closes. A byte the
    // game's own input handler also writes; the rest is the hook's.
    if (g_auto_moving.load())
        if (auto* p = rt::Player()) SetAutoMove(p, false);
    // Hand the actual stopping to the hook: it runs on the game's thread and is
    // given the engine's own mover, so nothing here has to guess an offset or
    // call into the game from the polling thread. The very next frame clears the
    // movement bits before the engine's Update gets to act on them.
    g_rt_on.store(false);
    g_stop_pending.store(true);
}

void RequestEnginePath(const void* refr, uint32_t refid, float radius)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    ++g_ep_seq;                       // whatever was being worked is now stale
    g_ep_points.clear();
    if (!g_installed.load() || !config::Get().engine_path) { g_ep_state.store(3); return; }
    g_ep_ref = refr; g_ep_refid = refid; g_ep_radius = radius;
    g_ep_to_point = false;
    g_ep_req_seq = g_ep_seq;
    g_ep_state.store(1);
    g_ep_request.store(1);
}

void RequestEnginePathToPoint(float x, float y, float z, float radius)
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    ++g_ep_seq;                       // whatever was being worked is now stale
    g_ep_points.clear();
    if (!g_installed.load() || !config::Get().engine_path) { g_ep_state.store(3); return; }
    g_ep_ref = nullptr; g_ep_refid = 0; g_ep_radius = radius;
    g_ep_to_point = true;
    g_ep_point[0] = x; g_ep_point[1] = y; g_ep_point[2] = z;
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

bool EnginePathLeadsElsewhere()
{
    std::lock_guard<std::mutex> lk(g_ep_mutex);
    return g_ep_crossed;
}

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
