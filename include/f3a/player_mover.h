#pragma once

#include <cstdint>
#include <vector>
#include "f3a/game_access.h"

// Driving the player during a walk. Since 2026-10-30 THE ENGINE WALKS: the
// game's own Auto Move (PlayerCharacter+0x5B0) holds Forward, and the hook
// only turns the player toward the route every frame - speed, walk or run,
// sneaking, animations and footsteps are the game's. What follows describes
// the older way, kept for a game build whose auto-move code differs: TAKING
// OVER the engine's movement update instead of holding W/A/S/D through
// SendInput.
//
// The engine calls PlayerMover::Update every frame to turn input into motion.
// We hook that vtable slot: while a walk is active the original never runs, so
// nothing overwrites what we do — which is why we can finally write the heading
// directly (writing rotZ while the input pipeline was still running is what made
// the player spin in place). Movement itself goes through the actor's own move
// routine with an exact displacement vector, at the game's own walk or run speed.
//
// This is the technique the FNV accessibility mod uses; its "AI driven" flag
// turned out to be its own gate, not New Vegas's script function, so nothing
// here depends on an engine feature FO3 lacks.

namespace f3a::mover {

// Hook/unhook the movement update. Safe to call more than once; Install()
// verifies the vtable slot still holds the function we expect and does nothing
// if it doesn't (a different game build), leaving key-based walking usable.
bool Install();
void Shutdown();

// True when the hook is in place and steering is available.
bool Available();

// Diagnostic, logged once per walk: does the player's own mover already hold a
// PathingSolution we could read waypoints from? This is the question the whole
// engine-pathfinding plan turns on, and the game can answer it far more cheaply
// than a disassembler can.
void ProbePath();

// Steer toward a world point this frame. Call every tick while walking; the
// goal is consumed by the next engine movement update. `run` false keeps to a
// walk; true moves the way the player would (CurrentSpeed).
//
// Restating the goal is also what keeps the walk alive: a goal that stops being
// refreshed for a moment makes the mover stop by itself. Whatever goes wrong
// upstream — a cancelled walk, a load screen, the mod switched off mid-step —
// the player cannot be left walking on with nothing driving them.
void SetGoal(float x, float y, bool run);

// The same, plus the route's next real corner (kept for the callers; the walk
// no longer brakes for it - its speed is the game's, turns included).
void SetGoalWithCorner(float x, float y, bool run, float cornerX, float cornerY);

// Follow a stretch of route: pts[0] is where the leg being walked starts,
// pts[1] its end, and so on (at most kRouteMax points). The hook works out
// the point to steer at by itself, EVERY FRAME, as the FNV mod's does: a
// point worked out on the walk's own tick (every 80 ms) was walked past
// between two ticks on stairs, where it lies only a few steps ahead, and the
// player braked and turned back for it - three steps, a stop, three steps.
constexpr size_t kRouteMax = 12;
void SetRoute(const game::Vec3* pts, size_t n);

// The speed a walk moves at now (units/s): the game's own walk or run speed,
// as the player's Caps Lock (always run) and the Run key say, and nothing
// else - it never brakes for turns. 0 before the first step.
float CurrentSpeed();

// Once, after the walk stopped because the game's speed could not be read (a
// game build this was not made for): there is no other speed to walk at.
bool SpeedUnreadable();

// Once, after the player pressed a movement key during a walk: the game
// cancelled its auto-move, so the player has taken over and the walk ends.
bool UserTookOver();

// A game was loaded: for its first frames, whatever walking the save kept
// (the game's auto-move, the mover's "moving forward" state, the walk cycle)
// is cleared, unless a walk has started or a movement key is held.
void OnGameLoaded();

// A point to stop ON, not walk through: at the game's walking speed, never
// stepping past it, holding still once within `stopRadius`. For a spot the
// game checks with a small trigger box (where to stand for a quest photo).
void SetGoalPrecise(float x, float y, float stopRadius);

// Stop driving: the engine goes back to processing the player's own input.
// Safe from any thread — the stop itself is performed by the hook on the game's
// own thread, on the next frame.
void Clear();

// Is a goal currently being driven?
bool Active();

// Heading the engine is currently being steered to, in degrees (0 = north) —
// for diagnostics and speech.
float CurrentHeadingDeg();

// --- The engine's own pathfinder --------------------------------------------
//
// Ask the GAME to work out a route to `refr` — the same SetPathfindingGoal an
// NPC's travel package uses, so doors, stairs and the navmesh links between
// cells are handled by the game's own rules. Runs inside the movement hook on
// the game's thread; the call returns at once. `refid` guards against a
// stale pointer. Safe from any thread.
void RequestEnginePath(const void* refr, uint32_t refid, float radius);
// The same, to a bare POINT in the player's own cell/worldspace (the engine's
// position variant of SetPathfindingGoal) — for places with no reference: a
// road on the way somewhere far, a spot to stand on. Safe from any thread.
void RequestEnginePathToPoint(float x, float y, float z, float radius);
// Drop a request (the walk was stopped). Safe from any thread.
void CancelEnginePath();
// 0 = nothing asked, 1 = working, 2 = route ready, 3 = the game found none.
int  EnginePathState();
// Take the finished route (state 2 or 3 → back to 0). False if not finished.
bool TakeEnginePath(std::vector<game::Vec3>* out);
// Did the last route the game worked out go on into another cell or
// worldspace (through a door)? Those points are left out of the route, which
// then ends at that door.
bool EnginePathLeadsElsewhere();

} // namespace f3a::mover
