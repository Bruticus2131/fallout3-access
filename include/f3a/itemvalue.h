#pragma once

// Which loose items of the cheap kinds (misc things, aid, armour, clothing,
// ingredients) are worth a mention - value of 10 caps or more, or a script
// (bobbleheads) - from game data worked out offline
// (tools/generate_item_values.py -> items.dat). Exploration names only these
// on the way, not every coffee mug in a vault.

#include <cstdint>
#include <string>

namespace f3a::itemvalue {

// Read the table. False when missing (then every item counts as worth it).
bool Load(const std::wstring& path);

// Match the table to the plugins the game has loaded (done by the first
// question anyway).
void Resolve();

// Is this base form (running game's id) worth a mention? True when the
// table is missing, so a missing file never silences exploration.
bool Worthwhile(uint32_t baseId);

} // namespace f3a::itemvalue
