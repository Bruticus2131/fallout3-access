#pragma once

#include <cstdint>
#include <vector>
#include "f3a/game_access.h"

// Driving the player by TAKING OVER the engine's movement update, instead of
// holding W/A/S/D through SendInput.
//
// The engine calls PlayerMover::Update every frame to turn input into motion.
// We hook that vtable slot: while a walk is active the original never runs, so
// nothing overwrites what we do — which is why we can finally write the heading
// directly (writing rotZ while the input pipeline was still running is what made
// the player spin in place). Movement itself goes through the actor's own move
// routine with an exact displacement vector, so we control speed precisely and
// can slow down into turns instead of skidding off ledges.
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
// goal is consumed by the next engine movement update. `run` picks run speed.
//
// Restating the goal is also what keeps the walk alive: a goal that stops being
// refreshed for a moment makes the mover stop by itself. Whatever goes wrong
// upstream — a cancelled walk, a load screen, the mod switched off mid-step —
// the player cannot be left walking on with nothing driving them.
void SetGoal(float x, float y, bool run);

// The same, plus the route's next real corner. The walk steers at a point a
// little ahead along the route (smoother), but brakes for whichever is the
// sharper turn — that point or the corner — so a bend is never taken at full
// speed just because the look-ahead had already rounded it (the FNV mod's rule).
void SetGoalWithCorner(float x, float y, bool run, float cornerX, float cornerY);

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
