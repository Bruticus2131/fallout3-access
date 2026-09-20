#pragma once

#include <cstdint>
#include <string>

namespace f3a::config {

struct Hotkeys {
    // DirectInput scancodes (DIK_*). 0 = disabled.
    // Defaults avoid keys Fallout 3 binds itself, and avoid extended (E0)
    // scancodes (PgUp/Home/...) which GetAsyncKeyState reads unreliably for
    // some keyboards. We use plain letters and the punctuation cluster.
    uint32_t read_selection   = 0x1C; // ENTER
    uint32_t repeat_last      = 0x35; // /
    uint32_t silence          = 0x39; // SPACE-ish — overridden in INI usually
    uint32_t where_am_i       = 0x26; // L — location ("where am I")
    uint32_t player_status    = 0x23; // H — HP/AP/etc.
    uint32_t quest_target     = 0x25; // K — quest target direction
    uint32_t scan_nearby      = 0x2D; // X
    uint32_t scan_hostiles    = 0x2E; // C
    uint32_t describe_compass = 0x24; // J (F is the game's view-toggle)
    uint32_t toggle_mod       = 0x58; // F12
    uint32_t dump_menu_tree   = 0x57; // F11 — diagnostic dump to log
    uint32_t debug_start_game = 0x44; // F10 — run [Debug] StartGameCommand
    uint32_t intro_describe   = 0x42; // F8 — toggle intro audio-description track
    uint32_t menu_back        = 0x0E; // Backspace — click the menu Back button
    uint32_t restore_defaults = 0x13; // R — restore default controls (controls page)
    uint32_t skip_objective   = 0x0D; // = — skip current objective (advance quest stage)
    uint32_t vats_body_part   = 0x30; // B — cycle targeted body part in VATS
    uint32_t center_camera    = 0xC7; // Home — level the view (pitch to horizontal)
    // Object scanner + navigation. PgUp/PgDn cycle objects; Ctrl+PgUp/PgDn
    // cycle the category (matches the author's earlier mod).
    uint32_t scan_prev        = 0xC9; // Page Up — previous nearby object
    uint32_t scan_next        = 0xD1; // Page Down — next nearby object
    uint32_t turn_to          = 0x28; // ' — face the selected object
    uint32_t guide_beacon     = 0x27; // ; — beacon guidance to the target
    uint32_t auto_walk        = 0x2B; // \ — auto-walk to the target
    uint32_t guide_quest      = 0x34; // . — beacon guidance to the quest marker
    uint32_t activate_target  = 0xCF; // End — activate the selected object (no aiming)
    uint32_t crosshair_info   = 0x22; // G — say what's under the crosshair
    uint32_t view_toggle      = 0x21; // F — game's view key; we announce the result
    uint32_t item_info        = 0x20; // D — read selected item's details (in menus)
    uint32_t aim_target       = 0x33; // , — aim view at the selected/nearest target
    uint32_t attack_key       = 0x2F; // V (DIK) — weapon attack, used by the aim burst
    uint32_t drop_item        = 0xD3; // Delete — drop the selected item (inventory)
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
    bool   vats_tutorial      = true;  // speak the VATS how-to on first open
    bool   intro_audio_desc   = true;  // auto-play the opening-cinematic audio description
    // Line-of-sight aim cue: a synth blip when the crosshair is on an enemy
    // (free-aim combat aid, no VATS). Pitch tunable to taste.
    bool   target_cue         = true;
    int    target_cue_hz      = 880;   // blip frequency in Hz
    // Speak the NAME of whatever the camera is pointing at (not just the
    // activation verb), and optionally how far it is. Turn the distance off if
    // the extra words get tiring in crowded areas.
    bool   crosshair_names    = true;
    bool   crosshair_distance = true;
    // Turn toward targets with the engine's own actor-facing routine (the one
    // Command Extender's FaceObject uses) instead of writing angle fields.
    bool   native_face        = true;
    // Walk by driving the engine's own movement update instead of holding W/A/S/D.
    // Precise speed, smooth turns that slow down instead of skidding. 0 falls
    // back to the key-holding walker.
    bool   native_walk        = true;
    int    autowalk_speed     = 160;   // game units per second at run speed
    // Synthesized footstep clicks while auto-walking (the engine's own footstep
    // sounds ride on the walk animation, which driving the mover skips).
    bool   footstep_cue       = true;
    // Play the game's own walk/run animation while auto-walking. This also
    // restores the game's real footstep sounds, which ride on that animation;
    // the synthesized clicks above are only used when this is off.
    bool   walk_animation     = true;
    int    barter_warn_loss_caps = 10; // warn when transaction loses player >=N caps
    int    nearby_scan_radius   = 1200; // game units (~ 1 meter = 64 units)
    int    nearby_scan_max_items = 8;
    int    compass_units = 0;           // 0 = clock face ("godzina trzecia"), 1 = degrees
    // AutoWalk mouse-turn gain: mouse counts injected per degree of heading
    // error. Higher = turns faster (may overshoot/spin), lower = gentler/slower.
    // Tune live in the INI if auto-walk turns too hard or too softly.
    int    autowalk_turn_gain = 4;
    // Activation tolerance: the game's fActivatePickSphereRadius (default 16) is
    // enlarged to this on Activate so the Use key grabs the nearby object
    // without precise crosshair aim. Bigger = more forgiving (may grab a wrong
    // neighbour); smaller = stricter. 0 = leave the game's value untouched.
    int    activate_pick_radius = 150;

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
