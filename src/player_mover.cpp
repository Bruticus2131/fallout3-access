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
std::atomic<bool>  g_run{ false };
float g_heading = 0.0f;          // radians, engine convention (0 = +Y/north)
bool  g_heading_seeded = false;

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
constexpr int kBrakeFrames = 20;   // ~1/3 s — long enough to outlast a hitch
int g_brake = 0;

// Drift check. Neither of us can watch the screen, so "it said I arrived and
// then kept walking" has to become a measurement rather than a guess: after a
// stop, watch the player's position for a second and write to the log if they
// are still travelling. A log line either names the bug or rules this out.
constexpr float kDriftWindow = 1.0f;    // seconds to watch
constexpr float kDriftLimit  = 40.0f;   // units; walking covers far more
float g_watch = 0.0f;
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
float TurnSpeedScale(float absErrRad)
{
    if (absErrRad < 0.20f) return 1.0f;    // ~11 deg — effectively aligned
    if (absErrRad > 1.20f) return 0.15f;   // ~69 deg — near a right-angle turn
    float t = (absErrRad - 0.20f) / 1.0f;
    return 1.0f - t * 0.85f;
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
void StopNow(void* mover)
{
    SetMoverFlags(mover, false, false);
    EngineStopMover(mover);
    if (g_current_anim != kAnimIdle) {
        bool idled = false;
        if (auto* p = rt::Player()) idled = game::PlayActorAnimGroup(p, kAnimIdle);
        g_current_anim = kAnimIdle;
        F3A_INFO("Mover: stop — flags cleared, idle animation %s.",
                 idled ? "played" : "REFUSED");
    } else {
        F3A_INFO("Mover: stop — flags cleared.");
    }
    g_step_accum = 0.0f;

    if (auto* p = rt::Player()) {
        g_watch   = kDriftWindow;
        g_watch_x = p->posX;
        g_watch_y = p->posY;
    }
}

void __fastcall Update_Hook(void* mover, void* /*edx*/, float dt)
{
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
            float clamped = dt;
            if (clamped > 0.033f) clamped = 0.033f;
            if (clamped < 0.0f)   clamped = 0.0f;
            float zero[3] = { 0.0f, 0.0f, 0.0f };
            if (auto* p = rt::Player()) ActorMove(p, clamped, zero);
        }
        if (g_watch > 0.0f) {
            g_watch -= dt;
            if (g_watch <= 0.0f) {
                if (auto* p = rt::Player()) {
                    float dx = p->posX - g_watch_x, dy = p->posY - g_watch_y;
                    float moved = std::sqrt(dx * dx + dy * dy);
                    if (moved > kDriftLimit)
                        F3A_INFO("Mover: player STILL MOVING %.0f units in the "
                                 "second after the stop.", moved);
                }
                g_watch = 0.0f;
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

    bool running = g_run.load();
    float speed = static_cast<float>(config::Get().autowalk_speed);
    if (!running) speed *= 0.5f;
    speed *= TurnSpeedScale(std::fabs(err));

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

void SetGoal(float x, float y, bool run)
{
    g_goal_x.store(x);
    g_goal_y.store(y);
    g_run.store(run);
    g_goal_stamp.fetch_add(1);      // feeds the watchdog in the hook
    if (!g_active.exchange(true)) {
        g_heading_seeded = false;   // seed on next frame
        g_step_accum = 0.0f;
        g_current_anim = kAnimIdle;  // force the walk anim to be issued
    }
}

void Clear()
{
    if (!g_active.exchange(false)) return;
    // Hand the actual stopping to the hook: it runs on the game's thread and is
    // given the engine's own mover, so nothing here has to guess an offset or
    // call into the game from the polling thread. The very next frame clears the
    // movement bits before the engine's Update gets to act on them.
    g_stop_pending.store(true);
}

float CurrentHeadingDeg()
{
    float d = g_heading * 57.2957795f;
    if (d < 0.0f) d += 360.0f;
    return d;
}

} // namespace f3a::mover
