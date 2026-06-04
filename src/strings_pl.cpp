#include "f3a/strings.h"
#include "f3a/config.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace f3a::strings {
namespace {

// 1 metr ~ 64 jednostek gry w silniku Gamebryo.
constexpr float UNITS_PER_METER = 64.0f;

struct Entry { Key k; const char* pl; const char* en; };

constexpr std::array<Entry, 41> kTable = { {
    { Key::ModEnabled,          "Mod dostępności włączony.",
                                "Accessibility mod enabled." },
    { Key::ModDisabled,         "Mod dostępności wyłączony.",
                                "Accessibility mod disabled." },
    { Key::ModLoaded,           "Fallout 3 Access załadowany.",
                                "Fallout 3 Access loaded." },
    { Key::NoScreenReader,      "Nie wykryto czytnika ekranu. Używam SAPI.",
                                "No screen reader detected. Using SAPI." },
    { Key::ScreenReaderActive,  "Aktywny czytnik: %s.",
                                "Active reader: %s." },
    { Key::MenuOpened,          "%s otwarte.",
                                "%s opened." },
    { Key::MenuClosed,          "Menu zamknięte.",
                                "Menu closed." },
    { Key::SelectionFmt,        "%s. %s",
                                "%s. %s" },
    { Key::EmptyList,           "Lista pusta.",
                                "List empty." },
    { Key::Loading,             "Wczytywanie.",
                                "Loading." },
    { Key::LoadingDone,         "Wczytane.",
                                "Loaded." },
    { Key::LowHealth,           "Uwaga, niskie zdrowie.",
                                "Warning, low health." },
    { Key::Crippled,            "Okaleczona część: %s.",
                                "Crippled limb: %s." },
    { Key::InCombat,            "Walka.",
                                "In combat." },
    { Key::CombatEnded,         "Koniec walki.",
                                "Combat ended." },
    { Key::QuestTargetFmt,      "Cel zadania: %s. %s, %s.",
                                "Quest target: %s. %s, %s." },
    { Key::NoQuestTarget,       "Brak aktywnego celu zadania.",
                                "No active quest target." },
    { Key::CompassFmt,          "Zwrócony na %s.",
                                "Facing %s." },
    { Key::PlayerStatusFmt,     "Zdrowie %s, akcje %s, promieniowanie %s, capsy %s.",
                                "Health %s, action %s, radiation %s, caps %s." },
    { Key::NoNearbyEntities,    "Nikogo i niczego w pobliżu.",
                                "Nothing nearby." },
    { Key::NearbyHostileFmt,    "Wróg: %s, %s, %s.",
                                "Hostile: %s, %s, %s." },
    { Key::NearbyFriendlyFmt,   "%s, %s, %s.",
                                "%s, %s, %s." },
    { Key::NearbyContainerFmt,  "Kontener: %s, %s, %s.",
                                "Container: %s, %s, %s." },
    { Key::NearbyDoorFmt,       "Drzwi: %s, %s, %s.",
                                "Door: %s, %s, %s." },
    { Key::NearbyItemFmt,       "Przedmiot: %s, %s, %s.",
                                "Item: %s, %s, %s." },
    { Key::BarterTotalFmt,      "Ty oddajesz %s, handlarz oddaje %s. Bilans: %s.",
                                "You give %s, vendor gives %s. Net: %s." },
    { Key::BarterLossWarn,      "Uwaga, tracisz %s capsli.",
                                "Warning, losing %s caps." },
    { Key::LockpickSweetSpot,   "Trafiony punkt.",
                                "Sweet spot found." },
    { Key::LockpickBrokenSoon,  "Wytrych zaraz pęknie.",
                                "Pick is about to break." },
    { Key::LockpickBroken,      "Wytrych pękł.",
                                "Pick broken." },
    { Key::LockpickUnlocked,    "Otwarte.",
                                "Unlocked." },
    { Key::VatsTargetFmt,       "%s, %s procent, koszt %s.",
                                "%s, %s percent, cost %s." },
    { Key::VatsTargetSwitched,  "Cel zmieniony.",
                                "Target switched." },
    { Key::VatsQueueFull,       "Kolejka pełna.",
                                "Queue full." },
    { Key::VatsNoTargets,       "Brak celów.",
                                "No targets." },
    { Key::DialogOptionFmt,     "Opcja %s: %s. %s",
                                "Option %s: %s. %s" },
    { Key::DialogNoOptions,     "Brak opcji odpowiedzi.",
                                "No response options." },
    { Key::NpcSpeechFmt,        "%s: %s",
                                "%s: %s" },
    { Key::ItemWeightFmt,       "waga %s",
                                "weight %s" },
    { Key::ItemValueFmt,        "wartość %s",
                                "value %s" },
    { Key::LevelUp,             "Awans na nowy poziom.",
                                "Level up." },
}};

const Entry* Find(Key k)
{
    for (const auto& e : kTable) if (e.k == k) return &e;
    return nullptr;
}

bool IsPolish()
{
    return config::Get().language == "pl";
}

} // namespace

const char* Fmt(Key k)
{
    const Entry* e = Find(k);
    if (!e) return "";
    return IsPolish() ? e->pl : e->en;
}

std::string Render(Key k) { return Fmt(k); }

std::string RenderArgs(Key k, const char* a, const char* b,
                       const char* c, const char* d)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), Fmt(k),
                  a ? a : "",
                  b ? b : "",
                  c ? c : "",
                  d ? d : "");
    return buf;
}

std::string CompassDirection(float yaw_deg)
{
    // Normalize to [0, 360)
    while (yaw_deg < 0)    yaw_deg += 360.0f;
    while (yaw_deg >= 360) yaw_deg -= 360.0f;

    static const char* pl[] = {
        "północ", "północny wschód", "wschód", "południowy wschód",
        "południe", "południowy zachód", "zachód", "północny zachód"
    };
    static const char* en[] = {
        "north", "northeast", "east", "southeast",
        "south", "southwest", "west", "northwest"
    };
    int idx = (int)((yaw_deg + 22.5f) / 45.0f) % 8;
    return IsPolish() ? pl[idx] : en[idx];
}

std::string ClockDirection(float relative_yaw_deg)
{
    if (config::Get().compass_units == 1) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d°", (int)std::round(relative_yaw_deg));
        return buf;
    }

    // Map -180..180 to clock hour 1..12, 12 = directly forward (0°), 3 = right (+90°).
    float normalized = relative_yaw_deg;
    while (normalized < 0)    normalized += 360.0f;
    while (normalized >= 360) normalized -= 360.0f;
    int hour = (int)std::round(normalized / 30.0f);
    if (hour == 0) hour = 12;

    char buf[48];
    if (IsPolish()) {
        std::snprintf(buf, sizeof(buf), "godzina %d", hour);
    } else {
        std::snprintf(buf, sizeof(buf), "%d o'clock", hour);
    }
    return buf;
}

std::string FormatDistance(float game_units)
{
    float meters = game_units / UNITS_PER_METER;
    char buf[48];
    if (meters < 1.5f) {
        std::snprintf(buf, sizeof(buf), IsPolish() ? "tuż obok" : "right next to you");
    } else if (meters < 100.0f) {
        std::snprintf(buf, sizeof(buf), IsPolish() ? "%d metrów" : "%d meters",
                      (int)std::round(meters));
    } else {
        std::snprintf(buf, sizeof(buf), IsPolish() ? "ponad sto metrów" : "over a hundred meters");
    }
    return buf;
}

} // namespace f3a::strings
