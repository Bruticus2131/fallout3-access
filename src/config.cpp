#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace f3a::config {
namespace {

Settings          g_settings;
std::atomic<bool> g_enabled{ true };

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback,
            const wchar_t* path)
{
    return (int)GetPrivateProfileIntW(section, key, fallback, path);
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback,
              const wchar_t* path)
{
    return ReadInt(section, key, fallback ? 1 : 0, path) != 0;
}

std::string ReadStr(const wchar_t* section, const wchar_t* key,
                    const char* fallback, const wchar_t* path)
{
    wchar_t buf[256];
    wchar_t wfb[64];
    MultiByteToWideChar(CP_UTF8, 0, fallback, -1, wfb, 64);
    GetPrivateProfileStringW(section, key, wfb, buf, 256, path);
    // GetPrivateProfileString does not strip inline ';' comments;
    // we do it manually + trim trailing whitespace.
    if (wchar_t* semi = wcschr(buf, L';')) *semi = 0;
    int len = (int)wcslen(buf);
    while (len > 0 && (buf[len-1] == L' ' || buf[len-1] == L'\t')) buf[--len] = 0;

    char out[512];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 512, nullptr, nullptr);
    return out;
}

uint32_t ReadKey(const wchar_t* section, const wchar_t* key,
                 uint32_t fallback, const wchar_t* path)
{
    // INI stores hex like "0x14" or decimal. We accept both via wcstoul.
    wchar_t buf[32]; wchar_t fb[16];
    swprintf_s(fb, L"0x%02X", fallback);
    GetPrivateProfileStringW(section, key, fb, buf, 32, path);
    if (wchar_t* semi = wcschr(buf, L';')) *semi = 0;
    return (uint32_t)wcstoul(buf, nullptr, 0);
}

} // namespace

bool Load(const wchar_t* ini_path)
{
    if (GetFileAttributesW(ini_path) == INVALID_FILE_ATTRIBUTES) {
        F3A_WARN("INI not found at the expected path; using defaults.");
        return false;
    }

    auto& s = g_settings;

    s.enable_pipboy    = ReadBool(L"Modules", L"PipBoy",    true,  ini_path);
    s.enable_dialog    = ReadBool(L"Modules", L"Dialog",    true,  ini_path);
    s.enable_barter    = ReadBool(L"Modules", L"Barter",    true,  ini_path);
    s.enable_lockpick  = ReadBool(L"Modules", L"Lockpick",  true,  ini_path);
    s.enable_vats      = ReadBool(L"Modules", L"VATS",      true,  ini_path);
    s.enable_nav       = ReadBool(L"Modules", L"NavAssist", true,  ini_path);
    s.enable_worldscan = ReadBool(L"Modules", L"WorldScan", true,  ini_path);
    s.enable_subtitles = ReadBool(L"Modules", L"Subtitles", true,  ini_path);

    s.speak_on_menu_open  = ReadBool(L"Voice", L"SpeakOnMenuOpen", true, ini_path);
    s.verbose_pipboy      = ReadBool(L"Voice", L"VerbosePipBoy",   false, ini_path);
    s.read_item_weight    = ReadBool(L"Voice", L"ReadItemWeight",  true, ini_path);
    s.read_item_value     = ReadBool(L"Voice", L"ReadItemValue",   true, ini_path);
    s.barter_warn_loss_caps = ReadInt (L"Voice", L"BarterWarnLossCaps", 10, ini_path);
    s.nearby_scan_radius    = ReadInt (L"Voice", L"NearbyScanRadius",   1200, ini_path);
    s.nearby_scan_max_items = ReadInt (L"Voice", L"NearbyScanMaxItems", 8, ini_path);
    s.compass_units         = ReadInt (L"Voice", L"CompassUnits",       0, ini_path);
    s.autowalk_turn_gain    = ReadInt (L"Voice", L"AutoWalkTurnGain",   4, ini_path);
    s.activate_pick_radius  = ReadInt (L"Voice", L"ActivatePickRadius", 150, ini_path);

    s.language    = ReadStr(L"General", L"Language",   "pl",   ini_path);
    s.prefer_sapi = ReadBool(L"General", L"PreferSAPI", false, ini_path);
    s.game_text_codepage = ReadInt(L"General", L"GameTextCodepage", 0, ini_path);

    s.debug_start_command = ReadStr(L"Debug", L"StartGameCommand",
                                    "coc MegatonWorld", ini_path);

    auto& h = s.hotkeys;
    h.read_selection   = ReadKey(L"Hotkeys", L"ReadSelection",   h.read_selection,   ini_path);
    h.repeat_last      = ReadKey(L"Hotkeys", L"RepeatLast",      h.repeat_last,      ini_path);
    h.silence          = ReadKey(L"Hotkeys", L"Silence",         h.silence,          ini_path);
    h.where_am_i       = ReadKey(L"Hotkeys", L"WhereAmI",        h.where_am_i,       ini_path);
    h.player_status    = ReadKey(L"Hotkeys", L"PlayerStatus",    h.player_status,    ini_path);
    h.quest_target     = ReadKey(L"Hotkeys", L"QuestTarget",     h.quest_target,     ini_path);
    h.scan_nearby      = ReadKey(L"Hotkeys", L"ScanNearby",      h.scan_nearby,      ini_path);
    h.scan_hostiles    = ReadKey(L"Hotkeys", L"ScanHostiles",    h.scan_hostiles,    ini_path);
    h.describe_compass = ReadKey(L"Hotkeys", L"DescribeCompass", h.describe_compass, ini_path);
    h.toggle_mod       = ReadKey(L"Hotkeys", L"ToggleMod",       h.toggle_mod,       ini_path);
    h.dump_menu_tree   = ReadKey(L"Hotkeys", L"DumpMenuTree",    h.dump_menu_tree,   ini_path);
    h.debug_start_game = ReadKey(L"Hotkeys", L"DebugStartGame",  h.debug_start_game, ini_path);
    h.menu_back        = ReadKey(L"Hotkeys", L"MenuBack",        h.menu_back,        ini_path);
    h.scan_next        = ReadKey(L"Hotkeys", L"ScanNext",        h.scan_next,        ini_path);
    h.scan_prev        = ReadKey(L"Hotkeys", L"ScanPrev",        h.scan_prev,        ini_path);
    h.turn_to          = ReadKey(L"Hotkeys", L"TurnTo",          h.turn_to,          ini_path);
    h.guide_beacon     = ReadKey(L"Hotkeys", L"GuideBeacon",     h.guide_beacon,     ini_path);
    h.auto_walk        = ReadKey(L"Hotkeys", L"AutoWalk",        h.auto_walk,        ini_path);
    h.guide_quest      = ReadKey(L"Hotkeys", L"GuideQuest",      h.guide_quest,      ini_path);
    h.activate_target  = ReadKey(L"Hotkeys", L"ActivateTarget",  h.activate_target,  ini_path);
    h.crosshair_info   = ReadKey(L"Hotkeys", L"CrosshairInfo",   h.crosshair_info,   ini_path);
    h.view_toggle      = ReadKey(L"Hotkeys", L"ViewToggle",      h.view_toggle,      ini_path);

    F3A_INFO("Config loaded. Language='%s'.", s.language.c_str());
    F3A_INFO("Hotkeys: toggle=0x%02X silence=0x%02X dump=0x%02X",
             h.toggle_mod, h.silence, h.dump_menu_tree);
    return true;
}

const Settings& Get() { return g_settings; }

void SetEnabled(bool e) { g_enabled.store(e); }
bool IsEnabled()        { return g_enabled.load(); }

} // namespace f3a::config
