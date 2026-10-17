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

} // namespace f3a::navmesh
