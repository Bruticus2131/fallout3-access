// The mod's own category in the game's Settings menu (Fallout3.exe 1.7.0.3).
//
// How the game builds that menu (reverse engineered for this; the Native
// Settings Extender mod adds rows to the game's categories, not a category):
//
//   StartMenu::InitOptions (0x00681D60) fills two arrays once per menu - it is
//   called from 0x00683812 when the first is empty:
//     buttons 0x00F6B3A8  {vtable 0x00E07FFC, caption, callback, menu mask}
//                         Continue, New, Settings... and the four categories
//                         (mask 4: Gameplay 0x00680580, Display, Audio, Controls)
//     options 0x00F6B3B8  the 48-byte rows the ctor 0x0067A6C0 makes: sliders
//                         and lists, mask 8 Gameplay, 0x10 Display, 0x20 Audio,
//                         0x40 Controls
//   A category's callback empties the list (0x0067A7F0 on StartMenu+0xAC) and
//   calls StartMenu::ShowSubmenu(mask) (0x0067F920), which shows every button
//   and option whose mask has that bit - a plain bit test. The game's masks
//   go up to 0x2000 (the yes/no and quit dialogs), so 0x4000 is ours.
//   When the menu opens, every option's callback is called with the values not
//   yet loaded (StartMenu+0x1A0 bit 0x20 clear, 0x0067BD60): it sets the row's
//   value; called again with the bit set, the player has changed it.
//   The menu frees all of them when it closes (0x0067F5C0), so they are made
//   on the game's heap and made again the next time.

#include "f3a/settings_menu.h"
#include "f3a/audio_beacon.h"
#include "f3a/config.h"
#include "f3a/game_access.h"
#include "f3a/hotkeys.h"
#include "f3a/logger.h"
#include "f3a/menu_dispatch.h"
#include "f3a/modules.h"
#include "f3a/strings.h"
#include "f3a/tolk_bridge.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace f3a::settings_menu {
namespace {

constexpr uintptr_t kInitOptionsCall = 0x00683812;   // call StartMenu::InitOptions
constexpr uintptr_t kInitOptions     = 0x00681D60;
constexpr uintptr_t kStartMenu       = 0x010770AC;   // StartMenu*
constexpr uintptr_t kButtons         = 0x00F6B3A8;   // BSSimpleArray<StartMenuButton*>
constexpr uintptr_t kOptions         = 0x00F6B3B8;   // BSSimpleArray<StartMenuOption*>
constexpr uintptr_t kArrayAdd        = 0x005E5350;   // thiscall(array) -> index of a new slot
constexpr uintptr_t kHeap            = 0x01090A78;
constexpr uintptr_t kHeapAlloc       = 0x0086B930;   // thiscall(heap, size)
constexpr uintptr_t kOptionCtor      = 0x0067A6C0;
constexpr uintptr_t kButtonVtable    = 0x00E07FFC;
constexpr uintptr_t kShowSubmenu     = 0x0067F920;   // thiscall(StartMenu*, mask), ret 4
constexpr uintptr_t kListReset       = 0x0067A7F0;   // thiscall(StartMenu+0xAC, bool), ret 4
constexpr uintptr_t kTileGetFloat    = 0x00BECF40;   // thiscall(tile, trait) -> float
constexpr uintptr_t kTileSetFloat    = 0x00BEE190;   // thiscall(tile, trait, value, propagate)
constexpr uintptr_t kTraitId         = 0x00BECD70;   // cdecl(name, -1) -> trait id
constexpr size_t    kStartMenuFlags  = 0x1A0;        // bit 0x20: values loaded
constexpr uint32_t  kMaskSettings    = 0x4;
constexpr uint32_t  kMaskOurs        = 0x4000;
// The menu's own per-category trait for where the options column is centred
// (0x1005 Gameplay ... 0x1008 Controls, read by ShowSubmenu into _center_x);
// ours has none, so the Gameplay one is used.
constexpr uint32_t  kTraitGameplayCenter = 0x1005;

// The game's option row, as its ctor fills it.
struct Option {
    void*        vtbl;
    const char*  caption;       // +04
    uint32_t     unk08;
    uint32_t     mask;          // +0C which menu shows it
    void*        valueType;     // +10
    int32_t      value;         // +14
    int32_t      defaultValue;  // +18
    uint32_t     type;          // +1C 0 = on/off, 1 = list, 2 = slider
    uint32_t     count;         // +20 values (a slider goes 0..count-1)
    uint32_t     width;         // +24
    const char** names;         // +28
    void*        callback;      // +2C cdecl(Option*)
};
static_assert(sizeof(Option) == 0x30, "the game's option is 48 bytes");

struct Button {
    void*        vtbl;
    const char*  caption;
    void (__cdecl* callback)();
    uint32_t     mask;
};
static_assert(sizeof(Button) == 0x10, "the game's button is 16 bytes");

using OptionCallback = void (__cdecl*)(Option*);
using OptionCtorFn   = Option* (__cdecl*)(void* mem, const char* caption, OptionCallback cb,
                                          uint32_t mask, uint32_t type, uint32_t count,
                                          uint32_t width, ...);

bool g_installed = false;

// Captions and value names in the game's own code page (cp1250 for Polish).
// The game keeps the pointers for as long as the menu lives, and the menu is
// built again each time, so these are kept until the next build.
std::deque<std::string> g_text;

const char* GameText(const char* utf8)
{
    UINT cp = config::GameTextCodepage();
    if (!cp) cp = CP_ACP;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(wlen > 0 ? wlen : 1, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), wlen);
    int alen = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(alen > 0 ? alen : 1, '\0');
    if (alen > 0) WideCharToMultiByte(cp, 0, w.c_str(), -1, a.data(), alen, nullptr, nullptr);
    a.resize(std::strlen(a.c_str()));
    g_text.push_back(std::move(a));
    return g_text.back().c_str();
}

void* GameAlloc(size_t size)
{
    using AllocFn = void* (__thiscall*)(void* heap, size_t size);
    return reinterpret_cast<AllocFn>(kHeapAlloc)(reinterpret_cast<void*>(kHeap), size);
}

// A new slot at the end of one of the menu's arrays (BSSimpleArray: data at +4).
void AppendTo(uintptr_t array, void* item)
{
    using AddFn = uint32_t (__thiscall*)(void* array);
    const uint32_t index = reinterpret_cast<AddFn>(kArrayAdd)(reinterpret_cast<void*>(array));
    void** data = *reinterpret_cast<void***>(array + 4);
    data[index] = item;
}

bool ValuesLoaded()
{
    auto* sm = *reinterpret_cast<uint8_t**>(kStartMenu);
    return sm && !IsBadReadPtr(sm + kStartMenuFlags, 4) &&
           (*reinterpret_cast<uint32_t*>(sm + kStartMenuFlags) & 0x20) != 0;
}

// ---- The options -----------------------------------------------------------
//
// Every row is a [Voice] key of the INI, read live by the code that uses it:
// a change takes effect at once and is written back, so it holds after a
// restart. The game calls a row's callback with the row alone, so one
// callback serves them all and finds its row in g_rows.

using config::Settings;

// Words in both languages, said through strings::T when the row is made.
struct Text { const char* pl; const char* en; };

// A setting shown as named values (type 0: on/off, or more names) or as a
// slider (type 2) of `count` steps, value = low + step * position.
struct Row {
    const char* key;
    Text caption;
    bool Settings::* flag;       // an on/off row...
    int  Settings::* number;     // ...or a number: named values or a slider
    int  low, step, count;       // number = low + step * position
    int  def;                    // the default, as the setting's own value
    int  type;                   // 0 named values, 2 slider
    Text names[3];
    void (*changed)(int value);  // after the setting is set (may be null)
    const char* (*say)(int value, char* buf, size_t size);   // a slider's value aloud
};

const char* SayHz(int v, char* buf, size_t n)
{
    std::snprintf(buf, n, "%d Hz", v);
    return buf;
}

const char* SayDegrees(int v, char* buf, size_t n)
{
    if (v <= 0) return strings::T("wyłączona", "off");
    std::snprintf(buf, n, strings::T("%d stopni", "%d degrees"), v);
    return buf;
}

const char* SayDistance(int v, char* buf, size_t n)
{
    std::snprintf(buf, n, "%s", strings::FormatDistance((float)v).c_str());
    return buf;
}

const char* SayRange(int v, char* buf, size_t n)
{
    if (v <= 0) return strings::T("bez limitu", "no limit");
    return SayDistance(v, buf, n);
}

void CueChanged(int hz)        { audio::Cue(hz, 60); }
void ExploreChanged(int on)    { modules::explore::SetOn(on != 0); }
void AudioDescChanged(int on)  { modules::audiodesc::SetEnabled(on != 0); }

#define F3A_OFF_ON { { "Wyłączone", "Off" }, { "Włączone", "On" } }

const Row kRows[] = {
    { "SpeakOnMenuOpen", { "Nazwa menu po otwarciu", "Menu name when it opens" },
      &Settings::speak_on_menu_open, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "ItemCard", { "Opis przedmiotu po nazwie", "Item details after the name" },
      &Settings::item_card, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "HudReader", { "Komunikaty i samouczki", "Messages and tutorials" },
      &Settings::hud_reader, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "Hints", { "Podpowiedzi sterowania", "Control hints" },
      nullptr, &Settings::hint_mode, 0, 1, 3, 1, 0,
      { { "Nigdy", "Never" }, { "Raz", "Once" }, { "Zawsze", "Always" } }, nullptr, nullptr },
    { "CrosshairNames", { "Nazwa pod celownikiem", "Name under the crosshair" },
      &Settings::crosshair_names, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "CrosshairDistance", { "Odległość przy celowniku", "Distance with the crosshair" },
      &Settings::crosshair_distance, nullptr, 0, 1, 2, 0, 0, F3A_OFF_ON, nullptr, nullptr },
    { "CompassUnits", { "Kierunki", "Directions" },
      nullptr, &Settings::compass_units, 0, 1, 2, 0, 0,
      { { "Godziny zegara", "Clock" }, { "Stopnie", "Degrees" } }, nullptr, nullptr },
    { "TargetCue", { "Sygnał celownika", "Aim cue" },
      &Settings::target_cue, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "TargetCueHz", { "Wysokość sygnału celownika", "Pitch of the aim cue" },
      nullptr, &Settings::target_cue_hz, 400, 100, 16, 880, 2, {}, &CueChanged, &SayHz },
    { "AimHelp", { "Pomoc w celowaniu", "Aim help" },
      &Settings::aim_help, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "EngineAim", { "Korekta pocisku", "Shot correction" },
      nullptr, &Settings::engine_aim_deg, 0, 5, 10, 15, 2, {}, nullptr, &SayDegrees },
    { "AimMouseTakeover", { "Mysz przerywa celowanie", "The mouse breaks the aim" },
      &Settings::aim_mouse_takeover, nullptr, 0, 1, 2, 0, 0, F3A_OFF_ON, nullptr, nullptr },
    { "ExploreMode", { "Tryb eksploracji", "Exploration mode" },
      &Settings::explore_mode, nullptr, 0, 1, 2, 0, 0, F3A_OFF_ON, &ExploreChanged, nullptr },
    { "HideEmptyContainers", { "Ukrywaj puste pojemniki", "Hide empty containers" },
      &Settings::hide_empty_containers, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "NoteSeries", { "Notatki z serii w obiektach zadań", "Series notes in quest objects" },
      &Settings::note_series, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "ScannerRadius", { "Zasięg skanera", "Scanner range" },
      nullptr, &Settings::scanner_radius, 2000, 1000, 15, 8000, 2, {}, nullptr, &SayDistance },
    { "ActivateRange", { "Zasięg użycia klawiszem End", "Reach of the End key" },
      nullptr, &Settings::activate_range, 0, 100, 16, 300, 2, {}, nullptr, &SayRange },
    { "SkipStage", { "Pomijanie etapów zadań (=)", "Skipping quest stages (=)" },
      &Settings::skip_stage, nullptr, 0, 1, 2, 0, 0, F3A_OFF_ON, nullptr, nullptr },
    { "AudioDescription", { "Audiodeskrypcja", "Audio description" },
      &Settings::audio_desc, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, &AudioDescChanged, nullptr },
    { "AudioDescEvents", { "Co robią postacie", "What people do" },
      nullptr, &Settings::audio_desc_events, 0, 1, 3, 1, 0,
      { { "Nigdy", "Never" }, { "W scenach", "In scenes" }, { "Zawsze", "Always" } }, nullptr, nullptr },
    { "AudioDescPeople", { "Wygląd postaci", "How people look" },
      &Settings::audio_desc_people, nullptr, 0, 1, 2, 0, 0, F3A_OFF_ON, nullptr, nullptr },
    { "CutsceneCue", { "Sygnał przerywnika", "Cutscene cue" },
      &Settings::cutscene_cue, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
    { "VatsTutorial", { "Samouczek VATS", "VATS tutorial" },
      &Settings::vats_tutorial, nullptr, 0, 1, 2, 1, 0, F3A_OFF_ON, nullptr, nullptr },
};

#undef F3A_OFF_ON

constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// The game's rows of this menu, by our row: they are made anew each time.
Option* g_rows[kRowCount] = {};

int Position(const Row& r, int value)
{
    int p = r.step > 0 ? (value - r.low + r.step / 2) / r.step : 0;
    return p < 0 ? 0 : p >= r.count ? r.count - 1 : p;
}

void __cdecl RowChanged(Option* o)
{
    size_t i = 0;
    while (i < kRowCount && g_rows[i] != o) ++i;
    if (i == kRowCount) return;
    const Row& r = kRows[i];
    auto& s = config::Mutable();
    if (!ValuesLoaded()) {
        o->value = r.flag ? (s.*r.flag ? 1 : 0) : Position(r, s.*r.number);
        o->defaultValue = Position(r, r.def);
        return;
    }
    const int pos = o->value < 0 ? 0 : o->value >= r.count ? r.count - 1 : o->value;
    const int value = r.low + r.step * pos;
    if (r.flag) s.*r.flag = value != 0;
    else        s.*r.number = value;
    config::WriteIniInt("Voice", r.key, value);
    if (r.changed) r.changed(value);
    F3A_INFO("Settings: %s = %d", r.key, value);
}

// ---- The category ----------------------------------------------------------

// Its button in the Settings list: what the game's own category buttons do,
// with our mask.
// The category's two screens share its mask: the options with the "Controls
// remapping" button, or the mod's keys (see SetKeysMode, below).
void SetKeysMode(bool keys);

// Show what has the category's mask now, as a category button does.
void ShowOurs()
{
    auto* sm = *reinterpret_cast<uint8_t**>(kStartMenu);
    if (!sm) return;
    using ResetFn = void (__thiscall*)(void* list, bool flag);
    using ShowFn  = void (__thiscall*)(void* menu, uint32_t mask);
    reinterpret_cast<ResetFn>(kListReset)(sm + 0xAC, false);
    reinterpret_cast<ShowFn>(kShowSubmenu)(sm, kMaskOurs);
    // Centre the column where the Gameplay one is.
    void* tile = *reinterpret_cast<void**>(sm + 4);
    if (tile && !IsBadReadPtr(tile, 0x20)) {
        using GetFn = float (__thiscall*)(void* tile, uint32_t trait);
        using SetFn = void (__thiscall*)(void* tile, uint32_t trait, float value, int propagate);
        using IdFn  = uint32_t (__cdecl*)(const char* name, int);
        const float center = reinterpret_cast<GetFn>(kTileGetFloat)(tile, kTraitGameplayCenter);
        const uint32_t id = reinterpret_cast<IdFn>(kTraitId)("_center_x", -1);
        if (center > 0.0f) reinterpret_cast<SetFn>(kTileSetFloat)(tile, id, center, 1);
    }
}

void __cdecl OpenCategory()
{
    SetKeysMode(false);
    ShowOurs();
    F3A_INFO("Settings: the Accessibility category opened");
}

Option* AddOption(const char* caption, OptionCallback cb, uint32_t type, uint32_t count,
                  uint32_t width, const char* name0 = nullptr, const char* name1 = nullptr,
                  const char* name2 = nullptr)
{
    void* mem = GameAlloc(sizeof(Option));
    if (!mem) return nullptr;
    auto ctor = reinterpret_cast<OptionCtorFn>(kOptionCtor);
    Option* o = name0 ? ctor(mem, caption, cb, kMaskOurs, type, count, width, name0, name1, name2)
                      : ctor(mem, caption, cb, kMaskOurs, type, count, width);
    if (o) AppendTo(kOptions, o);
    return o;
}

const char* Say(const Text& t) { return t.pl ? strings::T(t.pl, t.en) : nullptr; }

// ---- The mod's keys --------------------------------------------------------
//
// One row per key of the mod ([Hotkeys] in the INI), after the options (user,
// 2026-10-31). Its value is the key, from a list of the keys that can be
// given: Left and Right step through it, as through any row's values, and F2
// on the row takes the next key pressed, as F2 does on the game's Controls
// page. A key another of the mod's keys has is not taken (that one would stop
// working); a key the game itself uses is, with a warning. The menu, Enter,
// Escape, Tab, Backspace, the arrows and the modifiers stay as they are.
using config::Hotkeys;
struct KeyRow { const char* key; Text caption; uint32_t Hotkeys::* field; };

const KeyRow kKeyRows[] = {
    { "ScanNext",        { "Skaner, następny", "Scanner, next" }, &Hotkeys::scan_next },
    { "ScanPrev",        { "Skaner, poprzedni", "Scanner, previous" }, &Hotkeys::scan_prev },
    { "ScanCategoryNext", { "Skaner, następna kategoria", "Scanner, next category" }, &Hotkeys::scan_cat_next },
    { "ScanCategoryPrev", { "Skaner, poprzednia kategoria", "Scanner, previous category" }, &Hotkeys::scan_cat_prev },
    { "ActivateTarget",  { "Użyj wybranego obiektu", "Use the selected object" }, &Hotkeys::activate_target },
    { "CenterCamera",    { "Celownik na wybrany obiekt", "Crosshair on the selected object" }, &Hotkeys::center_camera },
    { "TrackTarget",     { "Śledź cel", "Track the target" }, &Hotkeys::track_target },
    { "Teleport",        { "Teleport do wybranego obiektu", "Teleport to the selected object" }, &Hotkeys::teleport },
    { "TurnTo",          { "Obróć do wybranego obiektu", "Turn to the selected object" }, &Hotkeys::turn_to },
    { "AutoWalk",        { "Autowalk do celu", "Auto-walk to the target" }, &Hotkeys::auto_walk },
    { "GuideBeacon",     { "Sygnał prowadzący do celu", "Guide beacon to the target" }, &Hotkeys::guide_beacon },
    { "GuideQuest",      { "Sygnał prowadzący do zadania", "Guide beacon to the quest" }, &Hotkeys::guide_quest },
    { "QuestTarget",     { "Kierunek do celu zadania", "Direction to the quest target" }, &Hotkeys::quest_target },
    { "ScanNearby",      { "Co jest w pobliżu", "What is nearby" }, &Hotkeys::scan_nearby },
    { "ScanHostiles",    { "Wrogowie w pobliżu", "Enemies nearby" }, &Hotkeys::scan_hostiles },
    { "LockEnemy",       { "Obróć do najbliższego wroga", "Turn to the nearest enemy" }, &Hotkeys::lock_enemy },
    { "CrosshairInfo",   { "Co jest pod celownikiem", "What is under the crosshair" }, &Hotkeys::crosshair_info },
    { "WhereAmI",        { "Gdzie jestem", "Where am I" }, &Hotkeys::where_am_i },
    { "DescribeCompass", { "Kompas", "Compass" }, &Hotkeys::describe_compass },
    { "PlayerStatus",    { "Stan postaci", "Your status" }, &Hotkeys::player_status },
    { "LevelStatus",     { "Poziom i doświadczenie", "Level and experience" }, &Hotkeys::level_status },
    { "Sonar",           { "Sonar", "Sonar" }, &Hotkeys::sonar },
    { "SweepSummary",    { "Co zostało do przeszukania", "What is left to search" }, &Hotkeys::sweep_summary },
    { "SweepNext",       { "Prowadź do następnej rzeczy", "Lead to the next thing" }, &Hotkeys::sweep_next },
    { "ExploreGo",       { "Idź do znaleziska", "Go to the find" }, &Hotkeys::explore_go },
    { "ExploreToggle",   { "Tryb eksploracji, włącz lub wyłącz", "Exploration mode on or off" }, &Hotkeys::explore_toggle },
    { "ItemInfo",        { "Opis przedmiotu", "Item details" }, &Hotkeys::item_info },
    { "DropItem",        { "Upuść przedmiot", "Drop the item" }, &Hotkeys::drop_item },
    { "RepeatLast",      { "Powtórz ostatni komunikat", "Repeat the last message" }, &Hotkeys::repeat_last },
    { "Silence",         { "Wycisz mowę", "Silence speech" }, &Hotkeys::silence },
    { "AudioDescRepeat", { "Powtórz audiodeskrypcję", "Repeat the audio description" }, &Hotkeys::audio_desc_repeat },
    { "AudioDescToggle", { "Audiodeskrypcja, włącz lub wyłącz", "Audio description on or off" }, &Hotkeys::audio_desc_toggle },
    { "IntroDescribe",   { "Audiodeskrypcja filmów", "Film audio description" }, &Hotkeys::intro_describe },
    { "SkipObjective",   { "Pomiń etap zadania", "Skip a quest stage" }, &Hotkeys::skip_objective },
    { "DumpMenuTree",    { "Zrzut do zgłoszenia błędu", "Bug report dump" }, &Hotkeys::dump_menu_tree },
    { "ToggleMod",       { "Włącz lub wyłącz mod", "Mod on or off" }, &Hotkeys::toggle_mod },
};
constexpr size_t kKeyRowCount = sizeof(kKeyRows) / sizeof(kKeyRows[0]);
// Every member is a key: a key another one has is found by walking them all.
static_assert(sizeof(Hotkeys) % sizeof(uint32_t) == 0, "[Hotkeys] holds keys only");

Option* g_key_opts[kKeyRowCount] = {};
Button* g_keys_button = nullptr;        // "Controls remapping", on the options screen
bool    g_keys_mode   = false;          // the keys screen is the one shown
std::vector<uint32_t>    g_keys;        // the values a row can take; [0] = none
size_t g_key_capacity = 0;              // room in each row's names array
constexpr size_t kKeySpare = 16;        // for shortcuts chosen while the menu is open
std::vector<std::string> g_key_names;   // their names, UTF-8

// A key's name as Windows gives it, in the player's own language; a
// shortcut's with its modifiers first ("Shift+Home").
std::string KeyName(uint32_t value)
{
    if (!(value & 0xFF)) return strings::T("brak", "none");
    return hotkeys::KeyText(value);
}

// Keys a mod key may sit on: not the menus' own (Esc, Backspace, Tab, Enter,
// the arrows), not a modifier, not the keypad (its keys change with Num Lock).
bool Assignable(uint32_t dik)
{
    switch (dik) {
    case 0x01: case 0x0E: case 0x0F: case 0x1C:
    case 0x1D: case 0x2A: case 0x36: case 0x38: case 0x3A:
    case 0x37: case 0x45: case 0x46: case 0x4A: case 0x4E:
        return false;
    }
    if (dik >= 0x47 && dik <= 0x53) return false;
    if (dik >= 0x80)
        return dik == 0xC7 || dik == 0xC9 || dik == 0xCF || dik == 0xD1 || dik == 0xD2 || dik == 0xD3;
    return dik != 0;
}

int KeyIndex(uint32_t value)
{
    for (size_t k = 0; k < g_keys.size(); ++k) if (g_keys[k] == value) return (int)k;
    return -1;
}

void BuildKeyList()
{
    g_keys.assign(1, 0);
    g_key_names.assign(1, KeyName(0));
    std::vector<int> vks;
    auto add = [&](uint32_t value) {
        const int vk = hotkeys::VkForDik(value);
        if (!vk) return;
        // One entry a plain key; a shortcut is its own entry.
        if (!(value & hotkeys::kModMask)) {
            if (std::find(vks.begin(), vks.end(), vk) != vks.end()) return;
            vks.push_back(vk);
        }
        std::string name = KeyName(value);
        if (name.empty()) return;
        g_keys.push_back(value);
        g_key_names.push_back(std::move(name));
    };
    for (uint32_t d = 0x02; d < 0x80; ++d) if (Assignable(d)) add(d);
    for (uint32_t d : { 0xC7u, 0xC9u, 0xCFu, 0xD1u, 0xD2u, 0xD3u }) add(d);
    // What the rows have now and the list has not - a shortcut, or a key set
    // by hand in the INI - is listed too, so each row shows its own.
    for (const KeyRow& r : kKeyRows) {
        const uint32_t v = config::Get().hotkeys.*r.field;
        if (v && KeyIndex(v) < 0) add(v);
    }
}

std::string Action(const KeyRow& r) { return Say(r.caption); }

// The row of the mod's keys whose field this is, or null.
const KeyRow* RowOf(const uint32_t* field)
{
    const auto& hk = config::Get().hotkeys;
    for (const KeyRow& r : kKeyRows)
        if (&(hk.*r.field) == field) return &r;
    return nullptr;
}

// Give the row its key or shortcut. False when another of the mod's keys has it.
bool SetKey(size_t row, uint32_t value, bool captured)
{
    const KeyRow& r = kKeyRows[row];
    auto& hk = config::Mutable().hotkeys;
    uint32_t& field = hk.*r.field;
    const std::string name = KeyName(value);
    if (field == value) {
        if (captured) tolk::Speak(Action(r) + ": " + name + ".", tolk::Priority::Ui, true);
        return true;
    }
    const uint32_t* all = reinterpret_cast<const uint32_t*>(&hk);
    const size_t count = sizeof(Hotkeys) / sizeof(uint32_t);
    field = value;
    config::WriteIniKey("Hotkeys", r.key, value);
    hotkeys::Remap(&field, value);
    F3A_INFO("Settings: %s = 0x%03X", r.key, value);
    std::string say = captured ? Action(r) + ": " + name + "." : std::string();
    auto add = [&](const std::string& s) { say += (say.empty() ? "" : " ") + s; };
    // Another action has it too: given anyway - the player moves keys round
    // one at a time (autowalk onto Shift+Home, then tracking off it) - and
    // both fire until the other is given another key.
    if (value) {
        for (size_t k = 0; k < count; ++k) {
            if (&all[k] == &field || all[k] != value) continue;
            const KeyRow* o = RowOf(&all[k]);
            const std::string other = o ? Action(*o)
                                        : std::string(strings::T("inna funkcja moda", "another key of the mod"));
            F3A_INFO("Settings: 0x%03X for %s - %s has it too", value, r.key, other.c_str());
            add(strings::T("Uwaga: ", "Note: ") + name + strings::T(" ma też: ", " also has: ") + other +
                strings::T(". Oba działają, dopóki tamtemu nie zmienisz klawisza.",
                           ". Both work until you give that one another key."));
        }
    }
    // A shortcut on a key another of the mod's keys has alone: the shortcut
    // comes first when its modifiers are held.
    const uint32_t base = value & 0xFF;
    if (value & hotkeys::kModMask) {
        for (size_t k = 0; k < count; ++k) {
            if (&all[k] == &field || all[k] != base) continue;
            const KeyRow* o = RowOf(&all[k]);
            add(strings::T("Uwaga: klawisz ", "Note: the key ") + KeyName(base) +
                strings::T(" ma też: ", " also has: ") +
                (o ? Action(*o) : std::string(strings::T("inna funkcja moda", "another key of the mod"))) +
                strings::T(". Ten skrót ma pierwszeństwo.", ". This shortcut comes first."));
            break;
        }
    }
    if (base && game::GameControlOnKey(static_cast<uint8_t>(base)) >= 0)
        add(strings::T("Uwaga: tego klawisza używa też gra.", "Note: the game uses this key too."));
    if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, captured);
    return true;
}

// A key being chosen with F2: the row, and its state (the poll thread's).
std::atomic<int> g_cap_row{ -1 };
DWORD g_cap_since = 0;
bool  g_cap_held[256] = {};

void __cdecl KeyRowChanged(Option* o)
{
    size_t i = 0;
    while (i < kKeyRowCount && g_key_opts[i] != o) ++i;
    if (i == kKeyRowCount || g_keys.empty()) return;
    const KeyRow& r = kKeyRows[i];
    if (!ValuesLoaded()) {
        const int cur = KeyIndex(config::Get().hotkeys.*r.field);
        const int def = KeyIndex(Hotkeys{}.*r.field);
        o->value = cur < 0 ? 0 : cur;
        o->defaultValue = def < 0 ? 0 : def;
        return;
    }
    if (g_cap_row.load() >= 0) return;        // a key is being chosen: that decides
    const int n = (int)g_keys.size();
    const int pos = o->value < 0 ? 0 : o->value >= n ? n - 1 : o->value;
    SetKey(i, g_keys[pos], /*captured*/false);
}

// Which screen of the category is shown: its rows carry its mask or none.
void SetKeysMode(bool keys)
{
    g_keys_mode = keys;
    for (Option* o : g_rows)     if (o) o->mask = keys ? 0 : kMaskOurs;
    for (Option* o : g_key_opts) if (o) o->mask = keys ? kMaskOurs : 0;
    if (g_keys_button) g_keys_button->mask = keys ? 0 : kMaskOurs;
}

// "Controls remapping" (Enter): the keys screen.
void __cdecl OpenKeys()
{
    SetKeysMode(true);
    ShowOurs();
    F3A_INFO("Settings: the keys screen opened");
    tolk::Speak(strings::T("Przypisanie klawiszy. F2 na wierszu zmienia jego klawisz albo skrót, "
                           "na przykład Shift plus Home. Backspace wraca.",
                           "Controls remapping. F2 on a row changes its key or shortcut, "
                           "for example Shift plus Home. Backspace goes back."),
                tolk::Priority::Ui, false);
}

// The row made to show its new key: put one step short of it, and the game's
// own Right (Left at the first) steps onto it - the game redraws the value
// and says it as for any change.
WORD  g_inject_vk = 0;
DWORD g_inject_up = 0;

void PressArrow(WORD vk)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(INPUT));
    g_inject_vk = vk;
    g_inject_up = (GetTickCount() + 90) | 1;
}

void ReleaseArrow()
{
    if (!g_inject_vk || (LONG)(GetTickCount() - g_inject_up) < 0) return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyW(g_inject_vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
    g_inject_vk = 0;
}

void ShowKey(size_t row, uint32_t value)
{
    Option* o = g_key_opts[row];
    if (!o) return;
    int pos = KeyIndex(value);
    // A new shortcut is not in the list yet: it goes into the spare room every
    // row's list was made with (nothing is allocated here, off the game's
    // thread), so each row can show it from now on.
    if (pos < 0) {
        if (g_keys.size() >= g_key_capacity) return;
        g_keys.push_back(value);
        g_key_names.push_back(KeyName(value));
        const char* name = GameText(g_key_names.back().c_str());
        pos = (int)g_keys.size() - 1;
        for (Option* k : g_key_opts) {
            if (!k) continue;
            const_cast<const char**>(k->names)[pos] = name;
            k->count = static_cast<uint32_t>(g_keys.size());
        }
    }
    if (g_keys.size() < 2) return;
    // Only while that row is still the selected one: the key just pressed went
    // to the game too, and one that moves the selection (Page Down) would
    // have the arrow change another row.
    const char* c = Say(kKeyRows[row].caption);
    auto sel = game::GetKeyboardSelectionText(false);
    if (!sel || sel->compare(0, std::strlen(c), c) != 0) {
        F3A_INFO("Settings: the key row is no longer selected - its value is shown next time");
        return;
    }
    if (pos > 0) { o->value = pos - 1; PressArrow(VK_RIGHT); }
    else         { o->value = 1;       PressArrow(VK_LEFT); }
}

void EndCapture(const char* say)
{
    g_cap_row.store(-1);
    g_cap_since = 0;
    hotkeys::SetCapturing(false);
    if (say) tolk::Speak(say, tolk::Priority::Ui, true);
}

// Keys that are not an answer: modifiers (Insert and Caps Lock are the screen
// reader's), the mouse buttons, locks.
bool NotAnAnswer(int vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_CAPITAL: case VK_INSERT:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_NUMLOCK: case VK_SCROLL: case VK_PACKET:
        return true;
    }
    return vk < 0x08;
}

void AddCategory()
{
    g_text.clear();
    for (auto& r : g_rows) r = nullptr;
    auto* b = static_cast<Button*>(GameAlloc(sizeof(Button)));
    if (!b) return;
    b->vtbl     = reinterpret_cast<void*>(kButtonVtable);
    b->caption  = GameText(strings::T("Dostępność", "Accessibility"));
    b->callback = &OpenCategory;
    b->mask     = kMaskSettings;
    AppendTo(kButtons, b);

    // As the game makes its own rows: named values are type 0 (two names:
    // off, on; three with a wider column, as its own three-way rows), a
    // slider type 2 with steps 0..count-1.
    int made = 0;
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        const char* caption = GameText(Say(r.caption));
        if (r.type == 2) {
            g_rows[i] = AddOption(caption, &RowChanged, 2, r.count, 0);
        } else {
            const char* n[3] = {};
            for (int k = 0; k < r.count && k < 3; ++k)
                n[k] = r.names[k].pl ? GameText(Say(r.names[k])) : nullptr;
            g_rows[i] = AddOption(caption, &RowChanged, 0, r.count, r.count > 2 ? 0x37 : 0,
                                  n[0], n[1], n[2]);
        }
        if (g_rows[i]) ++made;
    }

    // The mod's keys: a row of named values each, all the keys that can be
    // given. The game's ctor takes the names as arguments; made with one, the
    // row is then handed the whole list (on the game's heap, as its own is),
    // with room for shortcuts chosen while the menu is open. They are on the
    // category's second screen, behind the "Controls remapping" button.
    for (auto& o : g_key_opts) o = nullptr;
    g_keys_button = nullptr;
    BuildKeyList();
    std::vector<const char*> names;
    for (const auto& n : g_key_names) names.push_back(GameText(n.c_str()));
    g_key_capacity = names.size() + kKeySpare;
    int keys = 0;
    for (size_t i = 0; i < kKeyRowCount && names.size() >= 2; ++i) {
        Option* o = AddOption(GameText(Say(kKeyRows[i].caption)), &KeyRowChanged, 0, 1, 0x37, names[0]);
        if (!o) continue;
        auto** list = static_cast<const char**>(GameAlloc(g_key_capacity * sizeof(const char*)));
        if (!list) continue;
        for (size_t k = 0; k < g_key_capacity; ++k) list[k] = k < names.size() ? names[k] : names[0];
        o->names = list;
        o->count = static_cast<uint32_t>(names.size());
        g_key_opts[i] = o;
        ++keys;
    }
    if (keys) {
        auto* kb = static_cast<Button*>(GameAlloc(sizeof(Button)));
        if (kb) {
            kb->vtbl     = reinterpret_cast<void*>(kButtonVtable);
            kb->caption  = GameText(strings::T("Przypisanie klawiszy", "Controls remapping"));
            kb->callback = &OpenKeys;
            kb->mask     = kMaskOurs;
            AppendTo(kButtons, kb);
            g_keys_button = kb;
        }
    }
    SetKeysMode(false);
    F3A_INFO("Settings: the Accessibility category added (%d options, %d keys of %u)", made, keys,
             (unsigned)names.size());
}

void __cdecl InitOptions_Hook()
{
    reinterpret_cast<void (__cdecl*)()>(kInitOptions)();
    AddCategory();
}

// Point the CALL at `site` (which must call `expected` now) at `hook`.
bool RedirectCall(uintptr_t site, uintptr_t expected, const void* hook)
{
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return false;
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, 4);
    if (site + 5 + rel != expected) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    std::memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// The functions called, by their first bytes as disassembled.
bool CodeMatches()
{
    struct Sig { uintptr_t at; unsigned char bytes[8]; };
    static const Sig kSigs[] = {
        { kOptionCtor,   { 0x6A, 0xFF, 0x68, 0x48, 0x76, 0xC6, 0x00, 0x64 } },
        { kShowSubmenu,  { 0x83, 0xEC, 0x18, 0x55, 0x8B, 0xE9, 0x8B, 0x85 } },
        { kListReset,    { 0x6A, 0xFF, 0x68, 0x7E, 0x76, 0xC6, 0x00, 0x64 } },
        { kHeapAlloc,    { 0x83, 0xEC, 0x08, 0x8B, 0xC1, 0x80, 0x38, 0x00 } },
        { kArrayAdd,     { 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x08, 0x8B, 0x46 } },
        { kTileGetFloat, { 0x8B, 0x44, 0x24, 0x04, 0x50, 0xE8, 0x46, 0xFF } },
        { kTraitId,      { 0x51, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8D, 0x44 } },
    };
    for (const auto& s : kSigs) {
        const void* p = reinterpret_cast<const void*>(s.at);
        if (IsBadReadPtr(p, 8) || std::memcmp(p, s.bytes, 8) != 0) {
            F3A_INFO("Settings: code at 0x%08X differs - category not added", (unsigned)s.at);
            return false;
        }
    }
    return true;
}

} // namespace

bool SliderText(const std::string& caption, int position, std::string* out)
{
    if (!g_installed || !out) return false;
    for (const Row& r : kRows) {
        if (r.type != 2 || !r.say) continue;
        const char* c = Say(r.caption);
        if (caption.compare(0, std::strlen(c), c) != 0) continue;
        const int pos = position < 0 ? 0 : position >= r.count ? r.count - 1 : position;
        char buf[64];
        *out = r.say(r.low + r.step * pos, buf, sizeof(buf));
        return true;
    }
    return false;
}

bool BackFromKeys()
{
    if (!g_installed || !g_keys_mode || !menu::IsOpen(menu::Id::Start)) return false;
    // Only while the keys screen is what is shown: a key row is selected.
    auto sel = game::GetKeyboardSelectionText(false);
    bool onKeys = false;
    for (const KeyRow& r : kKeyRows) {
        const char* c = Say(r.caption);
        if (sel && sel->compare(0, std::strlen(c), c) == 0) { onKeys = true; break; }
    }
    if (!onKeys) return false;
    if (g_cap_row.load() >= 0) EndCapture(nullptr);
    SetKeysMode(false);
    ShowOurs();
    F3A_INFO("Settings: back from the keys screen");
    return true;
}

bool StartKeyCapture()
{
    if (!g_installed || g_cap_row.load() >= 0) return false;
    auto sel = game::GetKeyboardSelectionText(false);
    if (!sel) return false;
    for (size_t i = 0; i < kKeyRowCount; ++i) {
        const char* c = Say(kKeyRows[i].caption);
        const size_t n = std::strlen(c);
        if (sel->compare(0, n, c) != 0 || (sel->size() > n && (*sel)[n] != ',')) continue;
        if (!g_key_opts[i]) return false;
        g_cap_since = 0;
        g_cap_row.store((int)i);
        hotkeys::SetCapturing(true);
        F3A_INFO("Settings: choosing a key for %s", kKeyRows[i].key);
        tolk::Speak(strings::T("Naciśnij nowy klawisz dla: ", "Press the new key for: ") +
                        Action(kKeyRows[i]) +
                        strings::T(". Escape anuluje.", ". Escape cancels."),
                    tolk::Priority::System, true);
        return true;
    }
    return false;
}

void Tick()
{
    ReleaseArrow();
    const int row = g_cap_row.load();
    if (row < 0) return;
    const DWORD now = GetTickCount();
    if (!g_cap_since) {
        // The keys down now (F2 itself) are not the answer; reading them also
        // clears their "pressed since" bit.
        g_cap_since = now ? now : 1;
        for (int vk = 0; vk < 256; ++vk) g_cap_held[vk] = (GetAsyncKeyState(vk) & 0x8001) != 0;
        return;
    }
    // The menu closed under it: its rows are gone, nothing to set.
    if (!menu::IsOpen(menu::Id::Start)) { EndCapture(nullptr); return; }
    if (now - g_cap_since > 15000) {
        EndCapture(strings::T("Anulowano.", "Cancelled."));
        return;
    }
    for (int vk = 0x08; vk < 256; ++vk) {
        if (NotAnAnswer(vk)) continue;
        const SHORT st = GetAsyncKeyState(vk);
        const bool down = (st & 0x8000) != 0;
        if (g_cap_held[vk]) { if (!down) g_cap_held[vk] = false; continue; }
        if (!down && !(st & 1)) continue;
        if (vk == VK_ESCAPE) { EndCapture(strings::T("Anulowano.", "Cancelled.")); return; }
        uint32_t dik = 0;
        for (size_t k = 1; k < g_keys.size() && !dik; ++k)
            if (Assignable(g_keys[k]) && hotkeys::VkForDik(g_keys[k]) == vk) dik = g_keys[k];
        if (!dik) {
            EndCapture(strings::T("Tego klawisza nie można przypisać.", "That key can't be assigned."));
            return;
        }
        // Held with it: a shortcut (Shift+Home).
        auto held = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
        if (held(VK_SHIFT))   dik |= hotkeys::kModShift;
        if (held(VK_CONTROL)) dik |= hotkeys::kModCtrl;
        if (held(VK_MENU))    dik |= hotkeys::kModAlt;
        if (SetKey((size_t)row, dik, /*captured*/true)) ShowKey((size_t)row, dik);
        EndCapture(nullptr);
        return;
    }
}

bool Install()
{
    if (g_installed) return true;
    g_installed = CodeMatches() &&
                  RedirectCall(kInitOptionsCall, kInitOptions, &InitOptions_Hook);
    F3A_INFO("Settings: Accessibility category %s.",
             g_installed ? "hooked into the Settings menu" : "NOT installed (code differs)");
    return g_installed;
}

} // namespace f3a::settings_menu
