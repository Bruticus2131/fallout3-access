#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"
#include "f3a/strings.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"
#include "f3a/polling_loop.h"

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
// publicly), and they are what a sighted child sees on the pages:
//   * the attribute NAMES match the game's own wording, checked against a
//     Pip-Boy dump rather than recalled;
//   * each attribute's page carries a little RHYME ("S znaczy SIŁA..."),
//     transcribed from the page textures themselves (textures\terminals\
//     babybook01-05.dds: the Polish release ships its own, loose in Data;
//     the English ones are in Fallout - Textures.bsa). The Pip-Boy's
//     descriptions are not in the book, so they are not read here.
//   * the cover's title and the summary page's heading are the pages' own.
// The VALUES are always read live from the player's stats, never guessed.
constexpr int kNumSpecials = 7;

const strings::Key kNameKeys[kNumSpecials] = {
    strings::Key::SpecialStrength,     strings::Key::SpecialPerception,
    strings::Key::SpecialEndurance,    strings::Key::SpecialCharisma,
    strings::Key::SpecialIntelligence, strings::Key::SpecialAgility,
    strings::Key::SpecialLuck
};

// The rhyme on each attribute's page, as printed in the book (the Polish
// page textures drop the accents on "dźwięczy" and "mówią" - their font has
// no such letters; spelt right here so the voice says the word). Capitals in
// the book are lowered: a synthesizer spells out a word in capitals.
const char* const kRhymes[kNumSpecials][2] = {
    { "S znaczy siła i silny jestem wielce. Mogę rzucać dalej i podnosić więcej!",
      "S is for strength, and that means I am strong! I can carry more toys and swing stuff all day long!" },
    { "P jest jak percepcja - wciąż dźwięczy mi w uchu! Oznacza zmysły węchu, wzroku, dotyku i słuchu!",
      "P is for Perception, a long funny word! It means what I tasted, smelled, saw and heard!" },
    { "E jak energia do zabaw, której u mnie dużo. Zdrów jestem, wytrzymały - dni mi się nie dłużą.",
      "E is for Endurance, and that's how long I can play! I'm always really healthy, and have energy all day!" },
    { "C to jest charyzma, wszyscy lubią mnie! W moim towarzystwie chcą spędzać całe dnie!",
      "C is for Charisma, it's why people think I'm great! I make my friends all laugh and smile, and never want to hate!" },
    { "J jak jasny umysł i inteligencja. Starsi mówią na to - rozumu potencja!",
      "I is for Intelligence, it means I'm really smart! I use my brain for lots of stuff, like science, math and art!" },
    { "A bo akrobata ze mnie przewspaniały. Cichy, szybki, zwinny - oto jam jest cały!",
      "A is for Agility, that's how I get around! I move real fast and easy, and I never make a sound!" },
    { "Ł to jak łut szczęścia i nikt nie zaprzeczy! Przytrafiają mi się same dobre rzeczy!",
      "L is for luck, and it's simple, you see! It means that good things always happen to me!" },
};

bool g_open = false;
int  g_open_wait = 0;    // ticks spent waiting for the first value sample
// The book's own state, as last spoken (see Tick): the page and the attribute
// picked on the summary page.
int  g_page = -2;
int  g_book_index = -1;
bool g_up = false, g_down = false, g_left = false, g_right = false, g_enter = false;

// Where the highlight sits. With no text in the menu this follows the player's
// key presses — the same keys the game acts on — and is CORRECTED whenever a
// value changes, since only the selected attribute can change. That correction
// is what stops the counter from quietly drifting out of step.
int g_index = 0;
int g_values[kNumSpecials] = { -1, -1, -1, -1, -1, -1, -1 };

std::string Name(int i) { return strings::Render(kNameKeys[i]); }

// What the attribute's page says: its rhyme.
std::string Description(int i)
{
    if (i < 0 || i >= kNumSpecials) return {};
    return strings::T(kRhymes[i][0], kRhymes[i][1]);
}

// Sampled on the main thread (poll::PlayerSpecial): this module runs on the
// poll thread, and GetActorValue is a virtual call into the game.
int ReadValue(int i)
{
    return poll::PlayerSpecial(i);
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

// Points still to share out: the book's total less what the seven hold.
// -1 when the book's total couldn't be read.
int PointsLeft(int maxPoints)
{
    if (maxPoints <= 0) return -1;
    int sum = 0;
    for (int i = 0; i < kNumSpecials; ++i) {
        int v = ReadValue(i);
        if (v < 0) return -1;
        sum += v;
    }
    return maxPoints - sum;
}

std::string PointsLeftText(int maxPoints)
{
    int left = PointsLeft(maxPoints);
    if (left < 0) return {};
    return strings::T(", pozostało punktów: ", ", points left: ") + std::to_string(left);
}

// The attribute a page shows (0-6), as the game maps it (0x00675BE0): pages
// 1-7 are the attributes in order, page 8 is the summary page with the
// attribute picked there. -1 for the cover.
int StatForPage(int page, int indexStat)
{
    if (page >= 1 && page <= 7) return page - 1;
    if (page == 8 && indexStat >= 1 && indexStat <= 7) return indexStat - 1;
    return -1;
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

void Tick(float)
{
    if (menu::ActiveMenu() != menu::Id::SpecialBookMenu) {
        if (g_open) g_open = false;
        g_open_wait = 0;
        return;
    }

    int page = -1, indexStat = 0, maxPoints = 0;
    const bool book = poll::SpecialBookState(&page, &indexStat, &maxPoints);

    if (!g_open) {
        // The values and the page come from the main thread's sample, which
        // may not have been taken yet on the first tick: wait for it
        // (briefly), so the opening line carries the numbers.
        if ((ReadValue(0) < 0 || !book) && ++g_open_wait < 6) return;
        g_open_wait = 0;
        g_open  = true;
        g_index = 0;
        g_page  = -2;
        g_book_index = -1;
        for (int i = 0; i < kNumSpecials; ++i) g_values[i] = ReadValue(i);
        SeedKeys();                      // a held key on open must not fire
        F3A_INFO("SpecialBook: opened (book state %s)", book ? "readable" : "NOT readable");
        tolk::Speak(strings::RenderArgs(strings::Key::SpecialBookIntroFmt,
                        (AllValues() + PointsLeftText(maxPoints)).c_str()),
                    tolk::Priority::Ui, true);
        if (!book) return;
    }

    // THE BOOK SAYS WHERE IT IS (as in the FNV mod): the page it shows and
    // the attribute picked on its summary page are read from the menu itself.
    // The old way followed our own guess of which keys move the selection,
    // and the game's keys are not those — the mod said "Percepcja" while the
    // value the keys changed was Strength.
    if (book) {
        if (KeyEdge(VK_RETURN, &g_enter)) {
            int st = StatForPage(page, indexStat);
            std::string say = AllValues() + PointsLeftText(maxPoints);
            // The rhyme only where the page shows one: not on the summary page.
            if (st >= 0) say += ". " + Describe(st, page >= 1 && page <= 7);
            tolk::Speak(say, tolk::Priority::Ui, true);
        }
        bool queue = false;              // after the opening line, not over it
        if (g_page == -2) queue = true;
        if (page != g_page || (page == 8 && indexStat != g_book_index)) {
            bool newPage = page != g_page;
            g_page = page;
            g_book_index = indexStat;
            for (int i = 0; i < kNumSpecials; ++i) g_values[i] = ReadValue(i);
            int st = StatForPage(page, indexStat);
            std::string say;
            // The pages' own words: the cover's title, the summary's heading
            // and its "done" corner, each attribute's rhyme.
            if (page == 0) say = strings::T("Okładka: Specjał dla ciebie", "Cover: You're Special!");
            else if (page == 8 && newPage)
                say = strings::T("Podsumujmy! W rogu: Gotowe! ", "Let's Review! In the corner: All Done! ");
            if (st >= 0) {
                g_index = st;
                say += Describe(st, /*withDescription=*/false) + PointsLeftText(maxPoints);
                if (page >= 1 && page <= 7) say += ". " + Description(st);
            }
            F3A_INFO("SpecialBook: page %d, summary pick %d -> %s", page, indexStat,
                     st >= 0 ? Name(st).c_str() : "-");
            if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, !queue);
            return;
        }
        // A value changed: say which attribute and its new number, so it is
        // never a guess which one the keys moved.
        for (int i = 0; i < kNumSpecials; ++i) {
            int v = ReadValue(i);
            if (v < 0 || v == g_values[i]) continue;
            g_values[i] = v;
            tolk::Speak(Name(i) + " " + std::to_string(v) + PointsLeftText(maxPoints),
                        tolk::Priority::Ui, true);
            break;
        }
        return;
    }

    // The book's state could not be read (another build): the old guesswork.

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
