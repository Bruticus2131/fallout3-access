#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace f3a::menu {

// Menu IDs as they appear in Fallout 3's MenuManager. Values match
// the engine's enum so we can use them directly when hooking.
enum class Id : uint32_t {
    None             = 0,
    Message          = 1001,
    Inventory        = 1002,
    Stats            = 1003,
    HUDMain          = 1004,
    Loading          = 1005,
    Container        = 1008,
    Dialog           = 1009,
    SleepWait        = 1010,
    Start            = 1011,
    LockPick         = 1012,
    Quantity         = 1013,
    Map              = 1023,
    Book             = 1027,
    LevelUp          = 1028,
    Repair           = 1029,
    Race             = 1030,
    TextEdit         = 1031,
    Barter           = 1053,
    Surgery          = 1054,
    HackingShort     = 1055,
    VATS             = 1056,
    Computers        = 1057,
    RepairServices   = 1058,
    Tutorial         = 1059,
    SpecialBookMenu  = 1060,
    PipBoy           = 1080,
    // Sub-tabs of Pip-Boy are reached through internal state; not separate IDs.
};

using OpenHandler  = std::function<void()>;
using CloseHandler = std::function<void()>;
using TickHandler  = std::function<void(float dt)>;

// Register handlers for a particular menu. Modules call this in their Init().
void RegisterMenu(Id id, OpenHandler on_open,
                  CloseHandler on_close = {},
                  TickHandler on_tick = {});

// Called by FOSE event hooks when a menu opens or closes.
void OnMenuOpen(Id id);
void OnMenuClose(Id id);
void OnTick(float dt);

// Identify currently active top menu.
Id ActiveMenu();

// Resolve a human-readable name for announcement.
std::string_view DisplayName(Id id);

void Init();
void Shutdown();

} // namespace f3a::menu
