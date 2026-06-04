#pragma once

namespace f3a::poll {

// Starts a worker thread that ticks every ~kPollIntervalMs and dispatches:
//   - menu open/close transitions via menu::OnMenuOpen / OnMenuClose
//   - per-frame work via menu::OnTick(dt)
//   - hotkey polling via hotkeys::Poll()
// We use a dedicated thread instead of hooking Main::Update because polling
// the InterfaceManager singleton + bool array is read-only and safe.
void Start();
void Stop();

// Diagnostic: dump the active top-level menu tile tree to the log. Bound to
// the DumpMenuTree hotkey (F11 by default) — used to figure out the per-menu
// "selected" marker convention when the heuristic doesn't pick it up.
void DumpActiveMenuTree();

// True while the player is actually in the game world (HUDMain has opened
// and Start menu is not currently up). Used by game-state hotkeys to stay
// silent in the main menu / loading screens.
bool IsGameplayActive();

// True while the main menu (Start) is open or hasn't been left yet via
// HUDMain. Used by polling to suppress phantom Pip-Boy sub-menu opens
// during the splash → main menu → loading transition.
bool IsInMainMenu();

inline constexpr int kPollIntervalMs = 80; // ~12 Hz; tweak via INI later

} // namespace f3a::poll
