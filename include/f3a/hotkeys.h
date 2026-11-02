#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace f3a::hotkeys {

using Action = std::function<void()>;

// Called once during plugin load.
void Init();
void Shutdown();

// Bind a hotkey by DIK scancode. Replaces any previous binding for that key.
// `dik` is normally a field of config::Get().hotkeys: the binding then
// remembers which INI key it came from, so Remap can change it at run time
// (an unbound field, 0, is kept for that too, and never fires).
void Bind(const uint32_t& dik, Action action);
// Move the binding made from this config field to another key (0 = none).
// Applied on the next Poll; the new key does not fire until it is let go.
void Remap(const uint32_t* field, uint32_t dik);
// A key value is a DirectInput scancode in the low byte, and for a shortcut
// the modifiers held with it above that: Shift+Home = 0x1C7.
constexpr uint32_t kModShift = 0x100, kModCtrl = 0x200, kModAlt = 0x400;
constexpr uint32_t kModMask  = kModShift | kModCtrl | kModAlt;
// The virtual-key code a key value's scancode is read with (0 = none).
int VkForDik(uint32_t dik);
// A key value's name as Windows gives it, modifiers first ("Shift+Home");
// empty for none.
std::string KeyText(uint32_t value);
// While a new key is being chosen, none of the mod's keys fire.
void SetCapturing(bool on);

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
