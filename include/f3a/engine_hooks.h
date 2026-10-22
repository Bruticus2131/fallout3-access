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

namespace f3a::engine {

// Install every patch. Returns false if any one was not installed; the mod
// carries on either way.
bool InstallHooks();

// Nothing to roll back: the patched code lives as long as the process.
void RemoveHooks();

} // namespace f3a::engine
