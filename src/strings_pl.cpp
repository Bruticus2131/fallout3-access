#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace f3a::strings {
namespace {

// 1 metr ~ 64 jednostek gry w silniku Gamebryo.
constexpr float UNITS_PER_METER = 64.0f;

struct Entry { Key k; const char* pl; const char* en; };

constexpr std::array<Entry, 64> kTable = { {
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
                                "Health %s, Action Points %s, radiation %s, caps %s." },
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
    { Key::SpecialBookIntroFmt, "Książka Specjał dla ciebie. %s. Enter czyta wszystko.",
                                "The You're Special! book. %s. Enter reads everything." },
    { Key::SpecialStrength,     "Siła", "Strength" },
    { Key::SpecialPerception,   "Percepcja", "Perception" },
    { Key::SpecialEndurance,    "Wytrzymałość", "Endurance" },
    { Key::SpecialCharisma,     "Charyzma", "Charisma" },
    { Key::SpecialIntelligence, "Inteligencja", "Intelligence" },
    { Key::SpecialAgility,      "Zręczność", "Agility" },
    { Key::SpecialLuck,         "Szczęście", "Luck" },
    { Key::SpecialDescStrength,
        "Miara siły fizycznej. Wpływa na obrażenia broni białej i udźwig.",
        "A measure of raw physical strength. Affects melee damage and how much you can carry." },
    { Key::SpecialDescPerception,
        "Bystrość zmysłów. Wpływa na broń energetyczną, materiały wybuchowe i otwieranie zamków.",
        "How keen your senses are. Affects energy weapons, explosives and lockpicking." },
    { Key::SpecialDescEndurance,
        "Zdrowie i kondycja. Wpływa na punkty życia, odporność na promieniowanie i walkę wręcz.",
        "Health and physical fitness. Affects hit points, radiation resistance and unarmed combat." },
    { Key::SpecialDescCharisma,
        "Atrakcyjność i umiejętność przekonywania. Wpływa na handel i mowę.",
        "Likeability and persuasiveness. Affects barter and speech." },
    { Key::SpecialDescIntelligence,
        "Intelekt i zdolność myślenia. Wpływa na medycynę, naprawę i nauki ścisłe.",
        "Intellect and reasoning. Affects medicine, repair and science." },
    { Key::SpecialDescAgility,
        "Szybkość i zwinność. Wpływa na broń lekką, skradanie się i punkty akcji.",
        "Speed and nimbleness. Affects small guns, sneaking and action points." },
    { Key::SpecialDescLuck,
        "Jak często przypadek ci sprzyja. Wpływa po trochu na wszystko, zwłaszcza na trafienia krytyczne.",
        "How often chance favours you. Affects everything a little, critical hits most of all." },
    { Key::SpecialPageFmt,      "Strona %d", "Page %d" },
    { Key::LockpickSweetSpot,   "Trafiony punkt.",
                                "Sweet spot found." },
    { Key::LockpickBrokenSoon,  "Wytrych zaraz pęknie.",
                                "Bobby pin is about to break." },
    { Key::LockpickBroken,      "Wytrych pękł.",
                                "Bobby pin broke." },
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
    { Key::LootContainerFmt,    "%s.",
                                "%s." },
    { Key::LootItemFmt,         "%s, %s",
                                "%s, %s" },
    { Key::LootEquipped,        "założone",
                                "equipped" },
    { Key::LootStealing,        "kradzież",
                                "stealing" },
    { Key::LootWeightFmt,       "Udźwig %s.",
                                "Carry weight %s." },
    { Key::LootPositionFmt,     "%s z %s",
                                "%s of %s" },
    { Key::LootClosed,          "Zamknięto listę.",
                                "List closed." },
    { Key::LootTakenFmt,        "Zabrano: %s.",
                                "Taken: %s." },
}};

const Entry* Find(Key k)
{
    for (const auto& e : kTable) if (e.k == k) return &e;
    return nullptr;
}

bool IsPolish() { return Polish(); }

// The language being spoken. Never freed once made, so a text handed out by
// T() stays valid whatever the language is switched to later, on any thread.
struct LangState {
    std::string code;
    bool        polish = false;
    std::unordered_map<std::string, std::string> table;   // English -> this language
};
const LangState* NewState(const std::string& code, bool polish)
{
    auto* s = new LangState;
    s->code = code;
    s->polish = polish;
    return s;
}
std::atomic<const LangState*> g_lang{ nullptr };

const LangState* Lang()
{
    const LangState* l = g_lang.load(std::memory_order_acquire);
    if (l) return l;
    static const LangState* kEnglish = NewState("en", false);   // before the config is read
    return kEnglish;
}

// A text not in the translation file, logged once so it can be added.
std::mutex                      g_missing_mx;
std::unordered_set<std::string> g_missing;
void NoteMissing(const LangState* l, const char* en)
{
    std::lock_guard<std::mutex> lk(g_missing_mx);
    if (g_missing.size() < 2000 && g_missing.insert(en).second)
        F3A_INFO("Language %s: no translation for \"%s\"", l->code.c_str(), en);
}

// \n, \t and \\ in a translation file stand for themselves.
std::string Unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'n')  { out += '\n'; ++i; continue; }
            if (c == 't')  { out += '\t'; ++i; continue; }
            if (c == '\\') { out += '\\'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

// One value from a line "en: text" / "ru: text": the text after the colon and
// one space; a text in double quotes loses them (so it can start or end with
// a space).
std::string LineValue(const std::string& line, size_t colon)
{
    std::string v = line.substr(colon + 1);
    if (!v.empty() && v[0] == ' ') v.erase(0, 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return Unescape(v);
}

// lang_<code>.txt: pairs of lines, "en: English" then "<code>: translation";
// '#' starts a comment line. UTF-8.
// The printf directives of a text, in order ("%d", "%.0f", "%%"...), with
// their flags and widths: what a translation must keep as it is.
std::string FormatDirectives(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        size_t j = i + 1;
        while (j < s.size() && std::strchr("-+ #0123456789.lhzjt", s[j])) ++j;
        if (j < s.size()) { out.append(s, i, j - i + 1); out += '|'; }
        i = j;
    }
    return out;
}

void LoadTable(LangState* s, const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        F3A_INFO("Language %s: no translation file (%S) - speaking English", s->code.c_str(),
                 path.c_str());
        return;
    }
    std::string key, line;
    bool have_key = false;
    char buf[4096];
    int lines = 0;
    while (std::fgets(buf, sizeof(buf), f)) {
        line = buf;
        if (++lines == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // BOM
        if (line.empty() || line[0] == '#') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon > 4) continue;
        std::string tag = line.substr(0, colon);
        if (tag == "en") {
            key = LineValue(line, colon);
            have_key = true;
        } else if (tag == s->code && have_key) {
            std::string v = LineValue(line, colon);
            // Many texts are printf formats: a translation whose %-directives
            // differ from the English would read the wrong arguments (a %s
            // for a %d crashes the game), so it is left out and logged.
            if (!key.empty() && !v.empty()) {
                if (FormatDirectives(key) == FormatDirectives(v)) s->table[key] = v;
                else F3A_WARN("Language %s: \"%s\" skipped - its %% directives differ from the English",
                              s->code.c_str(), key.c_str());
            }
            have_key = false;
        }
    }
    std::fclose(f);
    F3A_INFO("Language %s: %u translated texts from %S", s->code.c_str(),
             (unsigned)s->table.size(), path.c_str());
}

} // namespace

bool Polish() { return Lang()->polish; }

std::string Language() { return Lang()->code; }

void SetLanguage(const std::string& code, const std::wstring& dataDir)
{
    const LangState* cur = g_lang.load(std::memory_order_acquire);
    if (cur && cur->code == code) return;
    const LangState* next = nullptr;
    if (code == "pl")      next = NewState("pl", true);
    else if (code == "en") next = NewState("en", false);
    else {
        auto* s = const_cast<LangState*>(NewState(code, false));
        if (!dataDir.empty()) {
            std::wstring wcode(code.begin(), code.end());
            LoadTable(s, dataDir + L"\\lang_" + wcode + L".txt");
        }
        next = s;
    }
    g_lang.store(next, std::memory_order_release);
}

const char* T(const char* pl, const char* en)
{
    const LangState* l = Lang();
    if (l->polish) return pl;
    if (l->table.empty() || !en || !*en) return en;
    auto it = l->table.find(en);
    if (it != l->table.end()) return it->second.c_str();
    NoteMissing(l, en);
    return en;
}

int PluralForm(long long n)
{
    if (n < 0) n = -n;
    const LangState* l = Lang();
    const long long n10 = n % 10, n100 = n % 100;
    const bool few = n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14);
    if (l->polish)       return n == 1 ? 0 : few ? 1 : 2;
    if (l->code == "ru" || l->code == "uk")
        return (n10 == 1 && n100 != 11) ? 0 : few ? 1 : 2;
    return n == 1 ? 0 : 2;
}

std::string Counted(long long n, const char* const pl[3], const char* const ru[3],
                    const char* enOne, const char* enMany)
{
    const LangState* l = Lang();
    // Russian words only when Russian is really spoken (its file is there);
    // otherwise English, with the English rule.
    const char* word = l->polish ? pl[PluralForm(n)]
                     : (l->code == "ru" && !l->table.empty()) ? ru[PluralForm(n)]
                     : ((n == 1 || n == -1) ? enOne : enMany);
    return std::to_string(n) + " " + word;
}

const char* Fmt(Key k)
{
    const Entry* e = Find(k);
    if (!e) return "";
    return T(e->pl, e->en);
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
    return T(pl[idx], en[idx]);
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

    char buf[64];
    const LangState* l = Lang();
    if (l->code == "ru" && !l->table.empty()) {      // на 1 час, на 3 часа, на 12 часов
        static const char* const kForms[] = { "час", "часа", "часов" };
        std::snprintf(buf, sizeof(buf), "на %d %s", hour, kForms[PluralForm(hour)]);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), T("godzina %d", "%d o'clock"), hour);
    return buf;
}

std::string FormatDistance(float game_units)
{
    // Report in raw GAME UNITS (not meters) — the native FO3 distance unit the
    // player thinks in. ~100 units ≈ 1.5 m, so under that = "right next to you".
    char buf[64];
    if (game_units < 100.0f) {
        std::snprintf(buf, sizeof(buf), "%s", T("tuż obok", "right next to you"));
    } else {
        const int n = (int)std::round(game_units);
        const LangState* l = Lang();
        if (l->code == "ru" && !l->table.empty()) {   // 1 единица, 2 единицы, 5 единиц
            static const char* const kForms[] = { "единица", "единицы", "единиц" };
            std::snprintf(buf, sizeof(buf), "%d %s", n, kForms[PluralForm(n)]);
        } else {
            std::snprintf(buf, sizeof(buf), T("%d jednostek", "%d units"), n);
        }
    }
    return buf;
}

} // namespace f3a::strings
