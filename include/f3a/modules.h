#pragma once

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

} // namespace f3a::modules
