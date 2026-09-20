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
namespace worldscan{ void Init(); void Shutdown(); void Tick(float dt); void ResetSession(); }
namespace intro    { void Init(); void Shutdown(); void Tick(float dt); void Start(); }
namespace hacking  { void Init(); void Shutdown(); void Tick(float dt); }
// World-map navigation: browse the map's location markers, travel, or set one
// as a walking destination.
namespace mapnav   { void Init(); void Shutdown(); void Tick(float dt); }
// The "how many?" prompt for stacks: arrows pick the amount, Enter confirms.
namespace quantity { void Init(); void Shutdown(); void Tick(float dt); }

// AutoWalk: walks the player to a scanner-selected target. Ticked from the
// polling loop while in gameplay.
namespace autowalk {
void Init();
void Shutdown();
void Tick(float dt);
void StartTo(const game::Vec3& pos, const std::string& name,
             const void* refr = nullptr, uint32_t form_id = 0);
// Walk to the LIVE quest marker, re-read each tick so the walk follows an
// objective that advances mid-route. False if the tracked objective has no
// marker. (StartTo walks to a fixed position instead.)
bool StartToQuest();
void Stop();
bool IsWalking();
// Current destination (for Alt+Home teleport-to-where-you're-walking). Null/0/
// empty when idle.
const void*        TargetRefr();
uint32_t           TargetFormId();
const std::string& TargetName();
// One-line diagnostic (F11 dump): current target, player/target positions,
// Z-delta, whether a navmesh path exists, and a live path-rebuild result — so a
// "why is autowalk stuck" report needs no screenshot.
std::string DiagString();
}

// Beacon guidance: the player walks; the mod gives audio direction cues.
namespace guide {
void Init();
void Shutdown();
void Tick(float dt);
void StartTo(const game::Vec3& pos, const std::string& name);
bool StartToQuest();   // follow the live quest marker; false if none
void Stop();
bool IsActive();
}

} // namespace f3a::modules
