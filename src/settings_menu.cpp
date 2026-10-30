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
#include "f3a/logger.h"
#include "f3a/modules.h"
#include "f3a/strings.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

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
void __cdecl OpenCategory()
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
    F3A_INFO("Settings: the Accessibility category added (%d options)", made);
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
