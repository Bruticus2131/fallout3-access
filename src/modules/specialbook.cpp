#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"
#include "f3a/strings.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"

#include <windows.h>
#include <string>
#include <cstdio>

namespace f3a::modules::specialbook {
namespace {

// The vault's SPECIAL book ("Zgadnij, kim zostaniesz"), where the child assigns
// their starting attributes.
//
// A menu dump settled how this screen is built: it holds ONLY buttons —
// exit_menu, index_up, index_down, next_page, previous_page, increase_value,
// decrease_value — and not one line of text. The names, ranks and descriptions
// are baked into images, exactly as they are in New Vegas, where the same wall
// forced that mod's author to transcribe them by hand.
//
// So the words come from our own strings (both languages — this mod ships
// publicly). Two things keep that honest:
//   * the attribute NAMES match the game's own wording, checked against a
//     Pip-Boy dump rather than recalled;
//   * the DESCRIPTIONS start as ours, but are REPLACED by the game's real text
//     as soon as the Pip-Boy's SPECIAL page has shown it, so a player who has
//     opened their Pip-Boy hears Bethesda's wording in their own language.
// The VALUES are always read live from the player's stats, never guessed.
constexpr int kNumSpecials = 7;

const strings::Key kNameKeys[kNumSpecials] = {
    strings::Key::SpecialStrength,     strings::Key::SpecialPerception,
    strings::Key::SpecialEndurance,    strings::Key::SpecialCharisma,
    strings::Key::SpecialIntelligence, strings::Key::SpecialAgility,
    strings::Key::SpecialLuck
};
const strings::Key kDescKeys[kNumSpecials] = {
    strings::Key::SpecialDescStrength,     strings::Key::SpecialDescPerception,
    strings::Key::SpecialDescEndurance,    strings::Key::SpecialDescCharisma,
    strings::Key::SpecialDescIntelligence, strings::Key::SpecialDescAgility,
    strings::Key::SpecialDescLuck
};

// Descriptions learned from the game's own Pip-Boy page, when available.
std::string g_learned[kNumSpecials];

bool g_open = false;
bool g_up = false, g_down = false, g_left = false, g_right = false, g_enter = false;

// Where the highlight sits. With no text in the menu this follows the player's
// key presses — the same keys the game acts on — and is CORRECTED whenever a
// value changes, since only the selected attribute can change. That correction
// is what stops the counter from quietly drifting out of step.
int g_index = 0;
int g_values[kNumSpecials] = { -1, -1, -1, -1, -1, -1, -1 };

std::string Name(int i) { return strings::Render(kNameKeys[i]); }

std::string Description(int i)
{
    if (!g_learned[i].empty()) return g_learned[i];   // the game's own words
    return strings::Render(kDescKeys[i]);
}

int ReadValue(int i)
{
    float v = game::GetPlayerAV(5 + i);   // 5..11 = SPECIAL
    return v < 0.0f ? -1 : (int)(v + 0.5f);
}

std::string Describe(int i, bool withDescription)
{
    if (i < 0 || i >= kNumSpecials) return {};
    int v = ReadValue(i);
    std::string s = Name(i);
    if (v >= 0) { s += " "; s += std::to_string(v); }
    if (withDescription) { s += ". "; s += Description(i); }
    return s;
}

std::string AllValues()
{
    std::string s;
    for (int i = 0; i < kNumSpecials; ++i) {
        int v = ReadValue(i);
        if (v < 0) continue;
        if (!s.empty()) s += ", ";
        s += Name(i) + " " + std::to_string(v);
    }
    return s;
}

// A changed value means the player was standing on THAT attribute — snap to it
// and read the new number.
bool SyncIndexFromValues()
{
    bool changed = false;
    for (int i = 0; i < kNumSpecials; ++i) {
        int v = ReadValue(i);
        if (v < 0 || g_values[i] < 0) { g_values[i] = v; continue; }
        if (v != g_values[i]) {
            g_values[i] = v;
            g_index = i;
            tolk::Speak(Name(i) + " " + std::to_string(v), tolk::Priority::Ui, true);
            changed = true;
        }
    }
    return changed;
}

bool KeyEdge(int vk, bool* prev)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !*prev;
    *prev = down;
    return edge;
}

void SeedKeys()
{
    g_up    = (GetAsyncKeyState(VK_UP)     & 0x8000) != 0;
    g_down  = (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0;
    g_left  = (GetAsyncKeyState(VK_LEFT)   & 0x8000) != 0;
    g_right = (GetAsyncKeyState(VK_RIGHT)  & 0x8000) != 0;
    g_enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
}

} // namespace

// Called while the Pip-Boy's SPECIAL page is up: it shows the selected
// attribute's real description in `stats_description`. Whichever attribute the
// text names, we keep that description and use it in the book from then on —
// which is how the player ends up hearing the game's own wording, in the game's
// own language, instead of ours.
void LearnFromStatsPage()
{
    constexpr uint32_t kStatsMenu = 0x3EB;            // FOSE's kMenuType_Stats
    auto lines = game::GetMenuTextLines(kStatsMenu);
    if (lines.empty()) return;

    // The description is the long line; the attribute it belongs to is whichever
    // name it starts with (the game's texts open with the attribute's name).
    for (const auto& line : lines) {
        if (line.size() < 40) continue;               // not a description
        for (int i = 0; i < kNumSpecials; ++i) {
            std::string n = Name(i);
            if (n.empty() || line.compare(0, n.size(), n) != 0) continue;
            if (g_learned[i] != line) {
                g_learned[i] = line;
                F3A_INFO("SpecialBook: learned description for %s from the game",
                         n.c_str());
            }
            break;
        }
    }
}

void Tick(float)
{
    // Pick up the game's own descriptions whenever the Pip-Boy shows them.
    if (menu::ActiveMenu() == menu::Id::Stats) LearnFromStatsPage();

    if (menu::ActiveMenu() != menu::Id::SpecialBookMenu) {
        if (g_open) g_open = false;
        return;
    }

    if (!g_open) {
        g_open  = true;
        g_index = 0;
        for (int i = 0; i < kNumSpecials; ++i) g_values[i] = ReadValue(i);
        SeedKeys();                      // a held key on open must not fire
        F3A_INFO("SpecialBook: opened (menu carries no text)");
        tolk::Speak(strings::RenderArgs(strings::Key::SpecialBookIntroFmt,
                                        AllValues().c_str()),
                    tolk::Priority::Ui, true);
        return;
    }

    // A value change is the ground truth about where the highlight is, and it
    // announces itself — that comes before any key handling.
    if (SyncIndexFromValues()) return;

    // Left/right pick the attribute; up/down change its value (the game does
    // that part itself, and the new number is announced by the value sync above).
    bool prev = KeyEdge(VK_LEFT,  &g_left);
    bool next = KeyEdge(VK_RIGHT, &g_right);
    KeyEdge(VK_UP,   &g_up);     // tracked only so a held key can't misfire
    KeyEdge(VK_DOWN, &g_down);

    if (prev || next) {
        g_index += next ? 1 : -1;
        if (g_index < 0) g_index = kNumSpecials - 1;
        if (g_index >= kNumSpecials) g_index = 0;
        tolk::Speak(Describe(g_index, /*withDescription=*/true),
                    tolk::Priority::Ui, true);
        return;
    }
    if (KeyEdge(VK_RETURN, &g_enter)) {
        tolk::Speak(AllValues() + ". " + Describe(g_index, true),
                    tolk::Priority::Ui, true);
    }
}

void Init() { F3A_INFO("SPECIAL book module ready."); }
void Shutdown() {}

} // namespace f3a::modules::specialbook
