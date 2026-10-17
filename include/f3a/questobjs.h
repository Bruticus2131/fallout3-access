#pragma once

// The things in the world quests wait for the player to use - buttons,
// terminals, doors, items, trigger spots, the seat for the G.O.A.T. - from
// game data worked out offline (tools/generate_quest_objects.py ->
// questobjs.dat). The scanner's "quest objects" category shows those near the
// player that still move a running quest on.
//
// Ids are numbered in the file like the door table's (0 = Fallout3.esm, then
// the DLCs) and get each plugin's load-order byte once the game has loaded its
// plugins; a DLC that is not loaded is left out.

#include <cstdint>
#include <string>
#include <vector>

namespace f3a::questobjs {

enum Kind : uint8_t {
    kActivator = 1, kTerminal = 2, kDoor = 3, kTrigger = 4, kSeat = 5,
    kMarker = 6, kItem = 7, kFurniture = 8, kContainer = 9,
};

struct Entry {
    uint32_t quest = 0;       // the quest's form id (running game's numbering)
    uint16_t stage = 0;       // the stage using it sets; 0 = none
    int16_t  objective = -1;  // the objective it is the target of; -1 = none
    uint8_t  kind = 0;
};

// Read the table. False when missing (the category is then empty).
bool Load(const std::wstring& path);

// Match the table to the plugins the game has loaded (done by the first
// question anyway).
void Resolve();

// The entries for a reference (running game's form id). False when it is not
// a quest object.
bool Lookup(uint32_t refid, std::vector<Entry>* out);

} // namespace f3a::questobjs
