#pragma once

#include <cstdint>
#include <string>

namespace f3a::config {

struct Hotkeys {
    // DirectInput scancodes (DIK_*). 0 = disabled.
    uint32_t read_selection   = 0x1C; // ENTER
    uint32_t repeat_last      = 0x35; // /
    uint32_t silence          = 0x39; // SPACE-ish — overridden in INI usually
    uint32_t where_am_i       = 0x14; // T
    uint32_t player_status    = 0x23; // H  (HP/AP/etc.)
    uint32_t quest_target     = 0x10; // Q
    uint32_t scan_nearby      = 0x2D; // X
    uint32_t scan_hostiles    = 0x2E; // C
    uint32_t describe_compass = 0x21; // F
    uint32_t toggle_mod       = 0x58; // F12
    uint32_t dump_menu_tree   = 0x57; // F11 — diagnostic dump to log
    uint32_t debug_start_game = 0x44; // F10 — run [Debug] StartGameCommand
    uint32_t menu_back        = 0x0E; // Backspace — click the menu Back button
};

struct Settings {
    // Modules
    bool enable_pipboy   = true;
    bool enable_dialog   = true;
    bool enable_barter   = true;
    bool enable_lockpick = true;
    bool enable_vats     = true;
    bool enable_nav      = true;
    bool enable_worldscan = true;
    bool enable_subtitles = true;

    // Voice behavior
    bool   speak_on_menu_open = true;
    bool   verbose_pipboy     = false;
    bool   read_item_weight   = true;
    bool   read_item_value    = true;
    int    barter_warn_loss_caps = 10; // warn when transaction loses player >=N caps
    int    nearby_scan_radius   = 1200; // game units (~ 1 meter = 64 units)
    int    nearby_scan_max_items = 8;
    int    compass_units = 0;           // 0 = clock face ("godzina trzecia"), 1 = degrees

    // Debug
    // Console command run by the debug_start_game hotkey. From the main menu
    // a `coc <cell>` starts a new game teleported into that cell, skipping
    // the intro — essential for a blind tester with no save.
    std::string debug_start_command = "coc MegatonWorld";

    // Localization
    std::string language = "pl";        // "pl" or "en"
    // Code page of the GAME's own text (tile labels, item names). Polish FO3
    // stores text in Windows-1250; English/Western in 1252. 0 = auto: derive
    // from `language` (pl → 1250, otherwise the system ANSI code page).
    int game_text_codepage = 0;

    // Tolk
    bool prefer_sapi = false;

    Hotkeys hotkeys;
};

bool Load(const wchar_t* ini_path);
const Settings& Get();

// For runtime tweaks (toggle_mod hotkey).
void SetEnabled(bool enabled);
bool IsEnabled();

} // namespace f3a::config
