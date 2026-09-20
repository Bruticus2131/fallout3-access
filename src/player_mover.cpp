#include "f3a/player_mover.h"
#include "f3a/fose_runtime.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/audio_beacon.h"
#include "f3a/game_access.h"

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

void __fastcall Update_Hook(void* mover, void* /*edx*/, float dt)
{
    if (!g_active.load()) {
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
    if (!g_active.exchange(true)) {
        g_heading_seeded = false;   // seed on next frame
        g_step_accum = 0.0f;
        g_current_anim = kAnimIdle;  // force the walk anim to be issued
    }
}

void Clear()
{
    if (!g_active.exchange(false)) return;
    if (g_current_anim != kAnimIdle) {
        if (auto* p = rt::Player()) game::PlayActorAnimGroup(p, kAnimIdle);
        g_current_anim = kAnimIdle;
    }
    // Drop the movement bits so scripts and the HUD do not think we are still
    // walking. The mover is reachable through the player.
    if (auto* p = rt::Player()) {
        void* mover = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(p) + 0x184);          // player->actorMover
        SetMoverFlags(mover, false, false);
    }
}

float CurrentHeadingDeg()
{
    float d = g_heading * 57.2957795f;
    if (d < 0.0f) d += 360.0f;
    return d;
}

} // namespace f3a::mover
