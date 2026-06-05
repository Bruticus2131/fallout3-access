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
bool BuildPath(const game::Vec3& from, const game::Vec3& to,
               std::vector<game::Vec3>& out_waypoints);

} // namespace f3a::navmesh
