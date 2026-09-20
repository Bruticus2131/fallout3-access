#pragma once

#include <cstdint>

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

// Steer toward a world point this frame. Call every tick while walking; the
// goal is consumed by the next engine movement update. `run` picks run speed.
void SetGoal(float x, float y, bool run);

// Stop driving: the engine goes back to processing the player's own input.
void Clear();

// Is a goal currently being driven?
bool Active();

// Heading the engine is currently being steered to, in degrees (0 = north) —
// for diagnostics and speech.
float CurrentHeadingDeg();

} // namespace f3a::mover
