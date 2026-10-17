#pragma once

// The world's load doors, as a graph: which door leads where, from game data
// worked out offline (tools/generate_world_doors.py -> doors.dat).
//
// A walk to a target in another place needs the right door, and the right
// door may be kilometres away and not loaded at all: from the Vault 101 exit,
// Megaton's gate is ~13000 units off, and the quest marker is on the bomb
// INSIDE Megaton, a worldspace of its own. Nearby doors are found in memory;
// this answers the question for the whole map, including the chain of doors
// through several places (the wasteland -> a DC ruins worldspace -> a metro).
//
// A SPACE is either a worldspace (all its exterior cells share one coordinate
// frame) or an interior cell; it is named by that form's id. The table covers
// Fallout3.esm and the five DLCs: their ids get the load-order byte each DLC
// has in the running game, and what belongs to a DLC that is not loaded is
// left out. The DLC travel points that are not doors (the handcart to the
// Pitt, the Duchess Gambit, the presidential metro) are in it as doors whose
// far side is where their script puts the player.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "f3a/game_access.h"

namespace f3a::worlddoors {

// Read the table. False (and the walk keeps to nearby doors) when missing.
bool Load(const std::wstring& path);
bool Loaded();

// Match the table to the plugins the game has loaded. Done by the first
// question anyway; called on a new or loaded game so it happens while the
// loading screen is up.
void Resolve();

// The space a persistent reference lives in — for a target whose place is
// not loaded, so it has no cell in memory to ask. 0 when unknown.
uint32_t RefSpace(uint32_t refid);

// A space's display name ("Megatona"), empty when unknown.
std::string SpaceName(uint32_t space);

struct Hop {
    uint32_t    door = 0;       // the door to walk to, in the space we are in
    game::Vec3  pos{};          // where it stands
    uint32_t    to_space = 0;   // where it leads
    std::string to_name;        // that place's name
    int         doors = 0;      // doors on the whole way, this one included
    float       walk = 0.0f;    // rough length of the whole way (units)
    int         travel = 0;     // not a door but a DLC travel point: 1 handcart,
                                // 2 boat, 3 metro console (activated, not walked through)
};

// The first door on the shortest way from (fromSpace, from) to (toSpace, to).
// Walking is measured in straight lines inside each space, a door counts a
// little extra. Doors in `exclude` (form ids) are not used — ones the walk
// found missing where the table put them. False when the target is not
// reachable through doors in the table — or needs none (same space and
// nothing shorter through doors).
//
// `area` (optional) says which connected part of a space a point is on
// (roadnet::Area; -1 = cannot tell): two points of one space in different
// parts are joined only through doors - a roof reached through the building
// under it - so the walk is routed through them.
using AreaFn = std::function<int(uint32_t space, const game::Vec3& p)>;
bool NextDoor(uint32_t fromSpace, const game::Vec3& from,
              uint32_t toSpace, const game::Vec3& to, Hop* out,
              const std::vector<uint32_t>* exclude = nullptr,
              const AreaFn* area = nullptr);

} // namespace f3a::worlddoors
