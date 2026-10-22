#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/strings.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

// Where the INI lives, remembered from Load() so a correction can be written
// back to the same file it was read from.
wchar_t g_ini_path[MAX_PATH] = L"";

std::vector<AimOffset> LoadAimOffsets()
{
    std::vector<AimOffset> out;
    if (!g_ini_path[0]) return out;
    // The whole section at once: the keys are form IDs, so they cannot be
    // enumerated any other way.
    std::vector<wchar_t> buf(8192);
    DWORD n = GetPrivateProfileSectionW(L"AimOffsets", buf.data(),
                                        (DWORD)buf.size(), g_ini_path);
    if (n == 0) return out;
    for (const wchar_t* e = buf.data(); *e; e += wcslen(e) + 1) {
        const wchar_t* eq = wcschr(e, L'=');
        if (!eq) continue;
        AimOffset o{};
        o.base_form_id = (uint32_t)wcstoul(e, nullptr, 0);
        const wchar_t* v = eq + 1;
        wchar_t* end = nullptr;
        o.x = wcstof(v, &end);            if (!end || *end != L',') continue;
        o.y = wcstof(end + 1, &end);      if (!end || *end != L',') continue;
        o.z = wcstof(end + 1, nullptr);
        if (o.base_form_id) out.push_back(o);
    }
    return out;
}

std::wstring DataDir()
{
    if (!g_ini_path[0]) return std::wstring();
    std::wstring dir = g_ini_path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return dir.substr(0, slash) + L"\\Fallout3Access";
}

void SaveAimOffset(uint32_t base_form_id, float x, float y, float z)
{
    if (!g_ini_path[0] || !base_form_id) return;
    wchar_t key[32], val[96];
    swprintf_s(key, L"0x%08X", base_form_id);
    swprintf_s(val, L"%.1f,%.1f,%.1f", x, y, z);
    if (!WritePrivateProfileStringW(L"AimOffsets", key, val, g_ini_path))
        F3A_INFO("Config: could not write the aim correction to %S (error %lu)",
                 g_ini_path, GetLastError());
}

namespace {

// The game's language from its word for "Yes" (the sYes setting), raw bytes in
// its own code page: "Tak" Polish, "Да" (cp1251) Russian, "Так" Ukrainian...
// "?" when it is none of these.
std::string LanguageFromYes(const std::string& v)
{
    if (v == "Tak")  return "pl";
    if (v == "Yes")  return "en";
    if (v == "Ja")   return "de";
    if (v == "Oui")  return "fr";
    if (v == "S\xED" || v == "Si") return "es";
    if (v == "S\xEC") return "it";
    if (v == "Ano")  return "cs";
    if (v == "Igen") return "hu";
    // Cyrillic in Windows-1251: Д = C4, а = E0 (А = C0); Т = D2, к = EA.
    if (v.size() == 2 && (unsigned char)v[0] == 0xC4 &&
        ((unsigned char)v[1] == 0xE0 || (unsigned char)v[1] == 0xC0)) return "ru";
    if (v == "\xD2\xE0\xEA" || v == "\xD2\xC0\xCA") return "uk";
    return "?";
}

// The code page of a language's text in the game's files.
unsigned CodepageFor(const std::string& code)
{
    if (code == "pl" || code == "cs" || code == "hu") return 1250;
    if (code == "ru" || code == "uk") return 1251;
    if (code == "en" || code == "de" || code == "fr" || code == "es" || code == "it") return 1252;
    return CP_ACP;
}
std::atomic<unsigned> g_game_cp{ CP_ACP };

// The language the game itself is in, from Fallout3.esm beside the plugin
// (Data\FOSE\Plugins\..\..\Fallout3.esm): the game setting sYes, in the first
// group of the file, is "Tak" in the Polish release and "Yes" in the English
// one. "pl", "en", "de", "fr", "es", "it", or empty when it cannot be told.
std::string ReadGameLanguage(const std::wstring& path);

std::string DetectGameLanguage(const wchar_t* ini_path)
{
    // The game's own folder first (beside Fallout3.exe), then beside the plugin.
    std::vector<std::wstring> tries;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring p = exe;
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) tries.push_back(p.substr(0, slash) + L"\\Data\\Fallout3.esm");
    }
    std::wstring path = ini_path;
    bool ok = true;
    for (int up = 0; up < 3 && ok; ++up) {           // the INI, Plugins, FOSE
        size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) ok = false;
        else path.resize(slash);
    }
    if (ok) tries.push_back(path + L"\\Fallout3.esm");
    for (const auto& t : tries) {
        std::string lang = ReadGameLanguage(t);
        if (!lang.empty()) return lang;
    }
    F3A_WARN("Game language: could not be read from Fallout3.esm");
    return {};
}

std::string ReadGameLanguage(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    std::string found;
    unsigned char hdr[24];
    bool ok = std::fread(hdr, 1, 24, f) == 24 && std::memcmp(hdr, "TES4", 4) == 0;
    uint32_t size = 0;
    if (ok) std::memcpy(&size, hdr + 4, 4);
    ok = ok && std::fseek(f, 24 + (long)size, SEEK_SET) == 0 && std::fread(hdr, 1, 24, f) == 24 &&
         std::memcmp(hdr, "GRUP", 4) == 0 && std::memcmp(hdr + 8, "GMST", 4) == 0;
    if (ok) std::memcpy(&size, hdr + 4, 4);
    // "Must be within": a damaged header must not make us read the whole disk.
    if (ok && size > 24 && size <= 8u * 1024 * 1024) {
        std::vector<unsigned char> g(size - 24);
        if (std::fread(g.data(), 1, g.size(), f) == g.size()) {
            for (size_t o = 0; o + 24 <= g.size() && found.empty();) {
                uint32_t rsize = 0, flags = 0;
                std::memcpy(&rsize, &g[o + 4], 4);
                std::memcpy(&flags, &g[o + 8], 4);
                size_t body = o + 24, end = body + rsize;
                if (end > g.size()) break;
                bool isYes = false;
                for (size_t q = body; !(flags & 0x40000) && q + 6 <= end;) {
                    uint16_t n = 0;
                    std::memcpy(&n, &g[q + 4], 2);
                    if (q + 6 + n > end) break;
                    const char* d = reinterpret_cast<const char*>(&g[q + 6]);
                    if (std::memcmp(&g[q], "EDID", 4) == 0) {
                        isYes = n == 5 && std::memcmp(d, "sYes", 5) == 0;
                    } else if (isYes && std::memcmp(&g[q], "DATA", 4) == 0) {
                        std::string v(d, strnlen(d, n));
                        found = LanguageFromYes(v);
                        F3A_INFO("Game language: sYes = '%s' -> %s", v.c_str(), found.c_str());
                        break;
                    }
                    q += 6 + n;
                }
                o = end;
            }
        }
    }
    std::fclose(f);
    return found == "?" ? std::string() : found;
}

} // namespace

bool Load(const wchar_t* ini_path)
{
    auto& s = g_settings;
    // The mod speaks the game's language unless the INI says otherwise:
    // Polish for the Polish release, Russian for a Russian one (from its
    // translation file), English for every other.
    s.game_language = DetectGameLanguage(ini_path);
    s.language = s.game_language.empty() ? "en" : s.game_language;
    // Not readable at all: the Windows language is the better guess.
    if (s.game_language.empty()) {
        const LANGID ui = PRIMARYLANGID(GetUserDefaultUILanguage());
        if (ui == LANG_POLISH)  s.language = "pl";
        if (ui == LANG_RUSSIAN) s.language = "ru";
    }
    g_game_cp.store(CodepageFor(s.game_language.empty() ? s.language : s.game_language));
    if (GetFileAttributesW(ini_path) == INVALID_FILE_ATTRIBUTES) {
        F3A_WARN("INI not found at the expected path; using defaults (language '%s').",
                 s.language.c_str());
        strings::SetLanguage(s.language, std::wstring());
        return false;
    }

    wcscpy_s(g_ini_path, ini_path);

    s.enable_pipboy    = ReadBool(L"Modules", L"PipBoy",    true,  ini_path);
    s.enable_dialog    = ReadBool(L"Modules", L"Dialog",    true,  ini_path);
    s.enable_barter    = ReadBool(L"Modules", L"Barter",    true,  ini_path);
    s.enable_lockpick  = ReadBool(L"Modules", L"Lockpick",  true,  ini_path);
    s.enable_vats      = ReadBool(L"Modules", L"VATS",      true,  ini_path);
    s.enable_nav       = ReadBool(L"Modules", L"NavAssist", true,  ini_path);
    s.enable_worldscan = ReadBool(L"Modules", L"WorldScan", true,  ini_path);
    s.enable_subtitles = ReadBool(L"Modules", L"Subtitles", true,  ini_path);

    s.speak_on_menu_open  = ReadBool(L"Voice", L"SpeakOnMenuOpen", true, ini_path);
    s.item_card           = ReadBool(L"Voice", L"ItemCard",        true, ini_path);
    s.vats_tutorial       = ReadBool(L"Voice", L"VatsTutorial",    true, ini_path);
    s.intro_audio_desc    = ReadBool(L"Voice", L"IntroAudioDesc",  true, ini_path);
    s.target_cue          = ReadBool(L"Voice", L"TargetCue",       true, ini_path);
    s.target_cue_hz       = ReadInt (L"Voice", L"TargetCueHz",     880,  ini_path);
    s.hud_reader          = ReadBool(L"Voice", L"HudReader",        true, ini_path);
    s.crosshair_names     = ReadBool(L"Voice", L"CrosshairNames",   true, ini_path);
    s.crosshair_distance  = ReadBool(L"Voice", L"CrosshairDistance",false, ini_path);
    s.native_face         = ReadBool(L"Voice", L"NativeFace",       true, ini_path);
    s.native_walk         = ReadBool(L"Voice", L"NativeWalk",       true, ini_path);
    s.aim_collision       = ReadBool(L"Voice", L"AimCollision",     false, ini_path);
    s.aim_bones           = ReadBool(L"Voice", L"AimBones",         false, ini_path);
    s.hostile_from_combat = ReadBool(L"Voice", L"HostileFromCombat", true, ini_path);
    s.aim_mouse_takeover  = ReadBool(L"Voice", L"AimMouseTakeover",  true, ini_path);
    s.autowalk_speed      = ReadInt (L"Voice", L"AutoWalkSpeed",    160,  ini_path);
    s.quest_arrive_dist   = ReadInt (L"Voice", L"QuestArriveDist",  0,    ini_path);
    s.footstep_cue        = ReadBool(L"Voice", L"FootstepCue",       true, ini_path);
    s.walk_animation      = ReadBool(L"Voice", L"WalkAnimation",     true, ini_path);
    s.nearby_scan_radius    = ReadInt (L"Voice", L"NearbyScanRadius",   1200, ini_path);
    s.scanner_radius        = ReadInt (L"Voice", L"ScannerRadius",      8000, ini_path);
    if (s.scanner_radius < 1000)  s.scanner_radius = 1000;
    if (s.scanner_radius > 40000) s.scanner_radius = 40000;
    s.nearby_scan_max_items = ReadInt (L"Voice", L"NearbyScanMaxItems", 8, ini_path);
    s.hide_empty_containers = ReadBool(L"Voice", L"HideEmptyContainers", true, ini_path);
    s.explore_mode          = ReadBool(L"Voice", L"ExploreMode",        false, ini_path);
    s.explore_radius        = ReadInt (L"Voice", L"ExploreRadius",      900,  ini_path);
    if (s.explore_radius < 200 || s.explore_radius > 4000) s.explore_radius = 900;
    s.audio_desc            = ReadBool(L"Voice", L"AudioDescription",   true,  ini_path);
    s.audio_desc_capture    = ReadBool(L"Voice", L"AudioDescCapture",   false, ini_path);
    s.engine_path           = ReadBool(L"Voice", L"EnginePath", true, ini_path);
    s.hint_mode             = ReadInt (L"Voice", L"Hints", 1, ini_path);
    s.engine_aim_deg        = ReadInt (L"Voice", L"EngineAim", 15, ini_path);
    if (s.engine_aim_deg < 0)  s.engine_aim_deg = 0;
    if (s.engine_aim_deg > 45) s.engine_aim_deg = 45;
    s.compass_units         = ReadInt (L"Voice", L"CompassUnits",       0, ini_path);
    s.autowalk_turn_gain    = ReadInt (L"Voice", L"AutoWalkTurnGain",   4, ini_path);
    s.activate_pick_radius  = ReadInt (L"Voice", L"ActivatePickRadius", 150, ini_path);

    std::string lang = ReadStr(L"General", L"Language", "auto", ini_path);
    for (auto& ch : lang) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    // pl, en, or the code of a translation file (ru: lang_ru.txt); else
    // "auto": the game's.
    bool code = lang.size() >= 2 && lang.size() <= 3 && lang != "aut";
    for (char ch : lang) if (ch < 'a' || ch > 'z') code = false;
    s.language_auto = !code;
    if (code) s.language = lang;
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
    h.intro_describe   = ReadKey(L"Hotkeys", L"IntroDescribe",   h.intro_describe,   ini_path);
    h.menu_back        = ReadKey(L"Hotkeys", L"MenuBack",        h.menu_back,        ini_path);
    h.restore_defaults = ReadKey(L"Hotkeys", L"RestoreDefaults", h.restore_defaults, ini_path);
    h.lock_enemy       = ReadKey(L"Hotkeys", L"LockEnemy",        h.lock_enemy,       ini_path);
    h.skip_objective   = ReadKey(L"Hotkeys", L"SkipObjective",   h.skip_objective,   ini_path);
    h.vats_body_part   = ReadKey(L"Hotkeys", L"VatsBodyPart",    h.vats_body_part,   ini_path);
    h.center_camera    = ReadKey(L"Hotkeys", L"CenterCamera",    h.center_camera,    ini_path);
    h.scan_next        = ReadKey(L"Hotkeys", L"ScanNext",        h.scan_next,        ini_path);
    h.scan_prev        = ReadKey(L"Hotkeys", L"ScanPrev",        h.scan_prev,        ini_path);
    h.turn_to          = ReadKey(L"Hotkeys", L"TurnTo",          h.turn_to,          ini_path);
    h.guide_beacon     = ReadKey(L"Hotkeys", L"GuideBeacon",     h.guide_beacon,     ini_path);
    h.auto_walk        = ReadKey(L"Hotkeys", L"AutoWalk",        h.auto_walk,        ini_path);
    h.guide_quest      = ReadKey(L"Hotkeys", L"GuideQuest",      h.guide_quest,      ini_path);
    h.activate_target  = ReadKey(L"Hotkeys", L"ActivateTarget",  h.activate_target,  ini_path);
    h.crosshair_info   = ReadKey(L"Hotkeys", L"CrosshairInfo",   h.crosshair_info,   ini_path);
    h.view_toggle      = ReadKey(L"Hotkeys", L"ViewToggle",      h.view_toggle,      ini_path);
    h.item_info        = ReadKey(L"Hotkeys", L"ItemInfo",        h.item_info,        ini_path);
    h.aim_target       = ReadKey(L"Hotkeys", L"AimTarget",       h.aim_target,       ini_path);
    h.attack_key       = ReadKey(L"Hotkeys", L"AttackKey",       h.attack_key,       ini_path);
    h.drop_item        = ReadKey(L"Hotkeys", L"DropItem",        h.drop_item,        ini_path);
    h.sonar            = ReadKey(L"Hotkeys", L"Sonar",           h.sonar,            ini_path);
    h.explore_go       = ReadKey(L"Hotkeys", L"ExploreGo",       h.explore_go,       ini_path);
    h.sweep_summary    = ReadKey(L"Hotkeys", L"SweepSummary",    h.sweep_summary,    ini_path);
    h.sweep_next       = ReadKey(L"Hotkeys", L"SweepNext",       h.sweep_next,       ini_path);
    h.audio_desc_repeat = ReadKey(L"Hotkeys", L"AudioDescRepeat", h.audio_desc_repeat, ini_path);
    h.press_row        = ReadKey(L"Hotkeys", L"PressRow",        h.press_row,        ini_path);
    h.aim_hold         = ReadKey(L"Hotkeys", L"AimHold",         h.aim_hold,         ini_path);
    h.container_side   = ReadKey(L"Hotkeys", L"ContainerSide",   h.container_side,   ini_path);
    h.aim_calibrate    = ReadKey(L"Hotkeys", L"AimCalibrate",    h.aim_calibrate,    ini_path);
    h.aim_up           = ReadKey(L"Hotkeys", L"AimUp",           h.aim_up,           ini_path);
    h.aim_down         = ReadKey(L"Hotkeys", L"AimDown",         h.aim_down,         ini_path);

    if (s.game_text_codepage > 0) g_game_cp.store((unsigned)s.game_text_codepage);
    strings::SetLanguage(s.language, DataDir());
    F3A_INFO("Config loaded. Language='%s' (INI '%s', game '%s', game text code page %u).",
             s.language.c_str(), lang.c_str(),
             s.game_language.empty() ? "unknown" : s.game_language.c_str(), g_game_cp.load());
    F3A_INFO("Hotkeys: toggle=0x%02X silence=0x%02X dump=0x%02X",
             h.toggle_mod, h.silence, h.dump_menu_tree);
    return true;
}

const Settings& Get() { return g_settings; }

unsigned GameTextCodepage() { return g_game_cp.load(); }

void ConfirmGameLanguage(const std::string& yes)
{
    if (yes.empty()) {
        F3A_INFO("Game language: the loaded sYes could not be read; keeping '%s'",
                 g_settings.game_language.c_str());
        return;
    }
    const std::string code = LanguageFromYes(yes);
    char hex[40] = {};
    for (size_t i = 0; i < yes.size() && i < 12; ++i)
        std::snprintf(hex + i * 3, 4, "%02X ", (unsigned char)yes[i]);
    // "Yes" is also what that setting holds before any file is read, so it
    // never overrules the file: only another language is news.
    if (code == "?" || code == "en" || code == g_settings.game_language) {
        F3A_INFO("Game language: the loaded sYes (%s) -> %s; keeping '%s'", hex, code.c_str(),
                 g_settings.game_language.c_str());
        return;
    }
    F3A_INFO("Game language: the loaded game says %s (sYes %s), Fallout3.esm said '%s'",
             code.c_str(), hex, g_settings.game_language.c_str());
    if (g_settings.game_text_codepage <= 0) g_game_cp.store(CodepageFor(code));
    if (g_settings.language_auto) strings::SetLanguage(code, DataDir());
}

void SetEnabled(bool e) { g_enabled.store(e); }
bool IsEnabled()        { return g_enabled.load(); }

} // namespace f3a::config
