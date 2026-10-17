#pragma once

// Long walks across a worldspace, along the ways a person can actually walk.
//
// The engine routes only as far as the navmesh it has loaded — a few cells —
// so a walk from the Vault 101 exit to Megaton, ~13000 units, has to be cut
// into legs, and the legs have to go somewhere sensible: a straight line over
// the wasteland runs into cliffs, rivers and ruins.
//
// Two kinds of graph, both built offline from the game's data:
//   * WALK graphs (tools/generate_walk_graph.py, 'FNVR' version 3): the
//     worldspace's whole navmesh as a coarse graph, roads cheaper than open
//     ground. A route in it is a way that exists on foot, so it is always
//     taken — it keeps to a road while the road goes the right way.
//   * ROAD graphs (tools/generate_road_network.py, versions 1-2): roads only,
//     taken when the whole walk is at most 1.5x the straight line.
// The road graph's loader, nearest-node grid and its route rules are ported
// from the FNV accessibility mod's RoadNetwork (FalloutNVAccess, MIT licence,
// (c) 2026 Alexander Epaneshnikov); version 2 is his format unchanged.

#include <cstdint>
#include <string>
#include <vector>
#include "f3a/game_access.h"

namespace f3a::roadnet {

// Find every graph in `dir` (files *.dat with the 'FNVR' magic). The
// worldspace each one belongs to comes from its header (the form id in a
// walk graph, the editor id in a road graph; version 4, a DLC's, also names
// the plugin and the DLC navmesh changes built in). Returns how many were
// found. They are loaded by Resolve().
int LoadDir(const std::wstring& dir);

// Load the graphs of the worldspaces the game has, now that it has loaded its
// plugins: a DLC worldspace under the load-order byte its DLC has, and of a
// worldspace's graphs the one whose DLC changes match the loaded DLCs best.
// Done by the first question anyway; called on a new or loaded game so the
// loading (16 MB for the wasteland) happens while the loading screen is up.
void Resolve();

// Is there a graph for this worldspace (form id)?
bool Has(uint32_t worldspace);

// A route from `from` to `to` in that worldspace, as points along it roughly
// 1500 (walk graph) or 2048 (road graph) units apart; the target itself is
// not included. False = no route worth taking: walk direct. `walk` gets the
// whole length, `roadShare` the part of it on roads (0..1).
// `minDistance`: shorter walks get no route (they are the engine's to
// route); 0 when the engine has already failed (Arefu's deck, reached only up
// a ramp the engine does not find from the ground).
bool Route(uint32_t worldspace, const game::Vec3& from, const game::Vec3& to,
           std::vector<game::Vec3>* out, float* walk = nullptr,
           float* roadShare = nullptr, float minDistance = 2000.0f);

// Which connected part of the walk graph a point is on (a roof or ledge
// reached only through a building is a part of its own). -1 when there is no
// walk graph or no graph point close enough to tell.
int Area(uint32_t worldspace, const game::Vec3& p);

} // namespace f3a::roadnet
