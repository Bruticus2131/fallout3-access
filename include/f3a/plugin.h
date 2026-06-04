#pragma once

// Forward declarations of FOSE plugin API types we touch. Real types live in
// fose/PluginAPI.h (provided by the FOSE source tree via FOSE_SDK_PATH).
// We pull the real headers in plugin_main.cpp.

namespace f3a {

// Called once at FOSE_Load. Initializes Tolk, config, hotkeys, and registers
// menu / event dispatch.
void OnPluginLoad();

// Called from FOSE messaging (PostLoad, PostLoadGame, NewGame, ExitGame, etc.).
void OnGameLoaded();
void OnNewGame();
void OnGameExit();

// Called on every script-tick (via OnFrameUpdate hook).
void OnFrame(float dt_seconds);

} // namespace f3a
