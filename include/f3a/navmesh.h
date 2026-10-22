#pragma once

// Navmesh pathfinding for world navigation. Reads the player cell's navmeshes
// (structure reverse-engineered from FO3 1.7), builds a triangle adjacency
// graph (edge-matched across all navmeshes in the cell), runs A*, and returns
// a list of waypoints from `from` to `to` that follow walkable ground around
// walls — what a straight line can't do indoors.

#include "f3a/game_access.h"
#include <vector>

namespace f3a::navmesh {

// Compute a walkable path in the player's current cell. On success fills
// `out_waypoints` (ordered, ending at/near `to`) and returns true. Returns
// false if there's no navmesh, the endpoints aren't on it, or no path exists
// — callers should fall back to straight-line guidance then.
//
// `avoid` (optional): points the path must not pass through — a locked door
// found on the way. Navmesh triangles centred within ~120 units of one are
// treated as walls, so the search goes round them if there is another way.
bool BuildPath(const game::Vec3& from, const game::Vec3& to,
               std::vector<game::Vec3>& out_waypoints,
               const std::vector<game::Vec3>* avoid = nullptr);

// The height of the walkable ground at (x, y) in the loaded cells — the
// navmesh triangle under that point whose height is nearest `nearZ`. For a
// point picked on a straight line across the wasteland, whose own height is
// only a guess: the engine's pathfinder needs a point on the ground.
bool GroundZ(float x, float y, float nearZ, float* outZ);

// Keep a route off the walls it turns round. A route pulled taut (the
// engine's) has each corner ON the navmesh's edge, against the wall on the
// inside of the turn, and a player following it brushes that wall or, cutting
// the bend, walks into it. Each corner sharper than ~15 degrees moves
// `clearance` units out of the turn - less for a gentle bend, never more than
// half its shorter leg - if the point it moves to, and both legs to it, are
// still on the loaded navmesh at that height; else half as far; else it
// stays. Corners where the route climbs (ramps, walkways) stay. Returns how
// many moved.
int InsetCorners(std::vector<game::Vec3>& waypoints, float clearance);

// The engine's route walked off the walls. Its corners lie on the walls it
// turns round and its straight legs run along them; our own route between
// the same two ends keeps a margin off them. That one is put in `out` (and
// true returned) only when it goes the same way: it ends where the engine's
// does, is at most a fifth longer (the margin costs length on stairs and in
// small rooms), and passes near every one of the engine's points - else the
// engine's route stands (put through InsetCorners).
bool RetraceWithMargin(const std::vector<game::Vec3>& route, std::vector<game::Vec3>& out,
                       float clearance);

// BuildPath's search and string pulling on triangles given as corners (three
// per triangle), not read from the game: for testing it offline on a real
// navmesh taken from Fallout3.esm.
// `margin` keeps the route that far off the mesh's edges where it passes
// through (see kWallMargin in navmesh.cpp).
bool BuildPathOnTriangles(const std::vector<game::Vec3>& corners, const game::Vec3& from,
                          const game::Vec3& to, std::vector<game::Vec3>& out_waypoints,
                          float clearance = 40.0f, float margin = 32.0f);
// RetraceWithMargin on triangles given as corners, the same way.
bool RetraceOnTriangles(const std::vector<game::Vec3>& corners, const std::vector<game::Vec3>& route,
                        std::vector<game::Vec3>& out, float clearance);

} // namespace f3a::navmesh
