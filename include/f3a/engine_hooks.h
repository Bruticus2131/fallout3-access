#pragma once

// Low-level engine hooks. Patches Fallout3.exe (1.7.0.3) call sites so we get
// notified when:
//   - the MenuManager opens or closes a menu
//   - the main loop ticks one frame
//   - a subtitle / dialog line is queued
//
// Implementation lives in src/engine_hooks.cpp. Offsets and call-site patterns
// for 1.7.0.3 are documented inline next to each hook.

namespace f3a::engine {

// Install all hooks. Returns false if any single hook failed; callers should
// continue (the mod degrades gracefully — menus may still announce via
// FOSE messaging fallback).
bool InstallHooks();

// Roll back all hooks (best-effort) — called on game shutdown.
void RemoveHooks();

} // namespace f3a::engine
