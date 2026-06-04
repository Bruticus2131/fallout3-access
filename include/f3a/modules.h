#pragma once

#include <string>

namespace f3a::game { struct Vec3; }

namespace f3a::modules {

void InitAll();
void ShutdownAll();

namespace pipboy   { void Init(); void Shutdown(); }
namespace dialog   { void Init(); void Shutdown(); }
namespace barter   { void Init(); void Shutdown(); }
namespace lockpick { void Init(); void Shutdown(); }
namespace vats     { void Init(); void Shutdown(); }
namespace container{ void Init(); void Shutdown(); }
namespace message  { void Init(); void Shutdown(); }
namespace nav      { void Init(); void Shutdown(); void Tick(float dt); }
namespace worldscan{ void Init(); void Shutdown(); }

// AutoWalk: walks the player to a scanner-selected target. Ticked from the
// polling loop while in gameplay.
namespace autowalk {
void Init();
void Shutdown();
void Tick(float dt);
void StartTo(const game::Vec3& pos, const std::string& name);
void Stop();
bool IsWalking();
}

// Beacon guidance: the player walks; the mod gives audio direction cues.
namespace guide {
void Init();
void Shutdown();
void Tick(float dt);
void StartTo(const game::Vec3& pos, const std::string& name);
void Stop();
bool IsActive();
}

} // namespace f3a::modules
