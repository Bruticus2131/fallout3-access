#include "f3a/modules.h"
#include "f3a/config.h"
#include "f3a/logger.h"

namespace f3a::modules {

void InitAll()
{
    const auto& c = config::Get();
    if (c.enable_pipboy)    pipboy::Init();
    if (c.enable_dialog)    dialog::Init();
    if (c.enable_barter)    barter::Init();
    if (c.enable_lockpick)  lockpick::Init();
    if (c.enable_vats)      vats::Init();
    if (c.enable_nav)       nav::Init();
    if (c.enable_worldscan) worldscan::Init();
    container::Init();
    message::Init();
    F3A_INFO("Modules initialized.");
}

void ShutdownAll()
{
    pipboy::Shutdown();
    dialog::Shutdown();
    barter::Shutdown();
    lockpick::Shutdown();
    vats::Shutdown();
    nav::Shutdown();
    worldscan::Shutdown();
    container::Shutdown();
    message::Shutdown();
}

} // namespace f3a::modules
