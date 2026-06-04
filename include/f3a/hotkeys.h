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

// Re-load bindings from config (after INI reload).
void Rebind();

} // namespace f3a::hotkeys
