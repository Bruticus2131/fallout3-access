#pragma once

// Holotapes and notes that come in a series and lie about the world, from game
// data worked out offline (tools/generate_note_series.py -> notes.dat).
//
// Some quests are only a set of tapes to find - the Keller family transcripts,
// five tapes over the whole Wasteland - and nothing in the Pip-Boy points at
// them. Once the player has one note of a series, the ones still to find are
// listed with the scanner's quest objects, with the way to each.
//
// Ids are numbered in the file like the door table's (0 = Fallout3.esm, then
// the DLCs) and get each plugin's load-order byte once the game has loaded its
// plugins; a DLC that is not loaded is left out.

#include <cstdint>
#include <string>
#include <vector>
#include "f3a/game_access.h"

namespace f3a::noteseries {

// Read the table. False when missing (nothing is listed then).
bool Load(const std::wstring& path);

// Match the table to the plugins the game has loaded (done by the first
// question anyway).
void Resolve();

struct Missing {
    uint32_t    note  = 0;      // the note's form id (running game's numbering)
    uint32_t    ref   = 0;      // where it lies: the reference...
    uint32_t    space = 0;      // ...the place (interior cell or worldspace)...
    game::Vec3  pos{};          // ...and the position in that place
    std::string name;           // the note's own name, in the game's language
    int         have  = 0;      // notes of its series the player has
    int         total = 0;      // notes of its series lying in the world
};

// The notes still to find of every series the player has started (has at
// least one note of, and not all). Plain memory reads: any thread.
std::vector<Missing> MissingNotes();

} // namespace f3a::noteseries
