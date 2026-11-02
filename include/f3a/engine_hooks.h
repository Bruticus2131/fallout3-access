#pragma once

// Patches to Fallout3.exe (1.7.0.3) call sites, for engine bugs the mod has to
// work round. Each site is checked byte for byte first; a different build is
// left untouched. Implementation and the reasoning per patch: engine_hooks.cpp.
//
//   - Dropping a stack from the inventory: the "Ile?" prompt's Ok crashed the
//     game when played with keyboard and mouse (the selected item was cleared
//     behind the prompt).
//   - Up and Down in a text field (the name at birth) typed an invisible
//     character, and the name could not be confirmed.
//   - The player's shot: the game's own bullet correction is pointed at the
//     spot the mod's aim lock holds, so a far enemy is hit, not only a near one.
//   - Enter in a menu: held back from the game while the mod answers it
//     itself (the hacking screen), so one press is not two clicks.
//   - Left and Right in the container screen: the game no longer steps the
//     category with them (the mod switches columns; Shift+arrows step it).
//   - Up and Down in the container screen: taken from the game while the mod
//     moves the row itself (the game's moved whichever list had a row last).

#include <cstdint>

namespace f3a::engine {

// The container screen's keys before the game sees them (its slot 14 codes:
// 1 up, 2 down, 3 right, 4 left, 10 tab, 13/14 shift+left/right). True takes
// the key from the game. Called on the game's thread, inside its key handling.
using ContainerKeyFilter = bool (*)(unsigned int code);
void SetContainerKeyFilter(ContainerKeyFilter filter);

// The function now in the container screen's key slot, 0 when that patch is
// not installed - game_access checks the slot to know the screen, and the
// container module drives Up/Down in the player's column only with it.
uintptr_t ContainerKeySlotHook();

// Whether the game's own Enter - a click on the tile under the mouse - is held
// back now. Called on the game's thread, inside its key handling.
using EnterFilter = bool (*)();
void SetMenuEnterFilter(EnterFilter filter);

// Where the next shot should go: true and the point while the aim key holds
// an enemy, false to leave the shot to the game. Called on the game's thread,
// inside the shot.
using ShotAimProvider = bool (*)(float out[3]);
void SetShotAimProvider(ShotAimProvider provider);

// When the player last fired a ranged weapon (GetTickCount), 0 never: the
// evidence for a hit when the game keeps no record of who dealt it.
unsigned long LastPlayerShotAt();

// Install every patch. Returns false if any one was not installed; the mod
// carries on either way.
bool InstallHooks();

// Nothing to roll back: the patched code lives as long as the process.
void RemoveHooks();

} // namespace f3a::engine
