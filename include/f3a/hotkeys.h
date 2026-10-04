#pragma once

#include <cstdint>
#include <functional>

namespace f3a::hotkeys {

using Action = std::function<void()>;

// Called once during plugin load.
void Init();
void Shutdown();

// Bind a hotkey by DIK scancode. Replaces any previous binding for that key.
void Bind(uint32_t dik, Action action);

// Called every frame (from the FOSE OnFrame tick).
void Poll();

// Tell the poller that WE are about to press this key, so the injected press is
// not mistaken for the player's and does not fire our own hotkey.
void SuppressKey(uint32_t dik);

// Re-load bindings from config (after INI reload).
void Rebind();

// True if Shift / Alt is (or was very recently) held — a short grace window
// makes modifier+key combos reliable despite the ~80 ms poll granularity.
bool ShiftActive();
bool AltActive();

// Shift read live, without that grace window: the modifier for Shift+PgUp /
// Shift+PgDn category cycling, which has to stop counting the moment Shift is
// released or the next plain press is eaten as another category step.
bool ShiftHeldNow();

} // namespace f3a::hotkeys
