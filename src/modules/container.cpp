#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"
#include "f3a/fose_runtime.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/polling_loop.h"

#include <windows.h>
#include <string>
#include <cstring>
#include <cstdio>

namespace f3a::modules::container {
namespace {

std::string g_last;
const void* g_last_box = nullptr;   // listbox of the last announcement
bool        g_on_container = false; // which side the focus was last put on

// Category filter, worked with Shift+Left and Shift+Right.
int  g_filter_say   = 0;      // ticks left before the new category is spoken
bool g_filter_queue = false;  // the row after a switch queues behind it
bool g_kShiftLeft = false, g_kShiftRight = false;
// Column switch, worked with plain Left and Right.
bool g_kLeft = false, g_kRight = false;
// After a switch: ticks left to hear the highlight land in the new column.
// If it never visibly moves (already there, or the row text is unchanged),
// the column is named anyway so the key press is never met with silence.
int  g_side_wait   = 0;
bool g_side_target = false;
// The column the PLAYER chose (Left/Right). The game's own Up/Down roam your
// column even after a switch, and as soon as they put the mouseover on one of
// your rows the menu hands the focus back to it. So the choice is remembered
// here and enforced: Up/Down in the container's column are ours, and a focus
// that wandered back is taken back.
bool g_want_container = false;
bool g_kUp = false, g_kDown = false;
int  g_restore_wait = 0;      // ticks before another focus restore may be asked
int  g_restore_tries = 0;     // restores asked since the focus was last seen right

const char* SideName(const void* box);   // defined below
std::string CountSuffix(int rows);       // defined below
void        PollFilterKeys();            // defined below
void        AnnounceFilter();            // defined below

void OnTick(float)
{
    PollFilterKeys();
    if (g_side_wait > 0 && --g_side_wait == 0 &&
        game::ContainerFocusSide() == (g_side_target ? 1 : 0)) {
        g_on_container = g_side_target;
        tolk::Speak(g_side_target ? "Pojemnik" : "Twoje przedmioty",
                    tolk::Priority::Ui, true);
    }
    // A category switch is in flight.
    //
    // The engine is rebuilding the column, and the rows that flicker past
    // while it does are not an answer to anything — so nothing is said until
    // the new category can be read. Then it is said, and the row the engine
    // settled on queues behind it.
    if (g_filter_say > 0) {
        if (--g_filter_say == 0) { AnnounceFilter(); g_filter_queue = true; }
        return;
    }

    // Keyboard path first: it tracks the highlight across BOTH columns, so
    // switching sides with Left/Right (which never moves the mouse activeTile)
    // gets announced. We re-speak when the row text changes OR the focused
    // column (listbox) changes — the latter is exactly the Left/Right switch.
    // The container menu's own selected row in its focused column first; the
    // generic highlight scan only if that can't be read. With two columns the
    // scan matched your column's highlight just as well as the container's,
    // so after switching right nothing was read but the word "Pojemnik".
    int focusSide = -1;
    const bool inContainer = menu::ActiveMenu() == menu::Id::Container;
    auto sel = inContainer ? game::GetContainerSelection(&focusSide)
                           : std::optional<game::MenuSelection>{};
    if (inContainer && g_want_container && focusSide == 0) {
        // The game's arrows moved the focus back to your column. Take it back
        // (if the container still has anything) and say nothing about the
        // detour — it was not the player's doing.
        game::ContainerSide mine{}, theirs{};
        if (game::GetContainerSides(&mine, &theirs) && theirs.rows <= 0) {
            g_want_container = false;          // emptied: your column it is
        } else if (g_restore_tries >= 5) {
            // The menu keeps refusing: stop fighting it and read the column
            // the focus is really in.
            F3A_INFO("Container: focus restore given up after %d tries", g_restore_tries);
            g_want_container = false;
        } else {
            if (g_restore_wait > 0) --g_restore_wait;
            else {
                poll::RequestContainerSide(true, /*quiet*/true);
                g_restore_wait = 3;
                ++g_restore_tries;
            }
            return;
        }
    }
    if (focusSide == 1) g_restore_tries = 0;
    // Only in the menus that share this tick (quantity, sleep/wait): in the
    // container screen a generic reader can land on the other column.
    if (!sel && !inContainer) sel = game::GetKeyboardSelection();
    if (sel && (!sel->label.empty() || !sel->value.empty())) {
        std::string text = sel->label;
        if (!sel->value.empty())
            text += (text.empty() ? "" : ", ") + sel->value;
        bool box_changed = sel->container != g_last_box;
        if (text == g_last && !box_changed) return;

        // A container menu has TWO lists — what the container holds and what
        // you are carrying — and which one you are in decides whether a key
        // takes or gives. Sighted players read that from the layout; without
        // the side spoken, the whole menu is a guess. The list's own tile name
        // is what identifies it, so log it here: once the names for this menu
        // are known, they become the spoken labels below.
        if (box_changed && sel->container) {
            const char* side = SideName(sel->container);
            if (!side && focusSide >= 0)
                side = focusSide == 1 ? "Pojemnik" : "Twoje przedmioty";
            if (side) {
                text = std::string(side) + ": " + text;
                g_on_container = (std::strcmp(side, "Pojemnik") == 0);
            }
        }

        if (box_changed) g_side_wait = 0;   // the switch announced itself
        g_last     = text;
        g_last_box = sel->container;
        // Straight after a category switch the row QUEUES behind the category
        // line instead of cutting it off: both are the answer to one press.
        tolk::Speak(text, tolk::Priority::Ui, !g_filter_queue);
        g_filter_queue = false;
        return;
    }

    // Fallback: the mouse/activeTile label — never in the container screen,
    // where the active tile can sit in the column the player is not using.
    if (inContainer) return;
    auto m = game::GetActiveMenuSelectionText();
    if (!m || m->empty() || *m == g_last) return;
    g_last     = *m;
    g_last_box = nullptr;
    tolk::Speak(*m, tolk::Priority::Ui, true);
}

// Which list holds the focus, by the list tile's own name.
const char* SideName(const void* box)
{
    if (!box) return nullptr;
    const char* nm = fose_rt::TileName(
        reinterpret_cast<Tile*>(const_cast<void*>(box)));
    if (!nm) return nullptr;
    if (std::strstr(nm, "CM_Container_")) return "Pojemnik";
    if (std::strstr(nm, "CM_Items_"))     return "Twoje przedmioty";
    return nullptr;
}

// ", 4 przedmioty" / ", brak przedmiotów" — the tail that says how much is
// in a list.
//
// Shared by the container's own announcement and the category switch so the
// two cannot drift apart. The old wording was ", pusty", which agrees with
// nothing it was ever attached to: "Szafka, pusty", "Amunicja, pusty". An
// absent count has no gender.
std::string CountSuffix(int rows)
{
    if (rows <= 0) return ", brak przedmiotów";
    // Polish counts: 1 przedmiot, 2-4 przedmioty, 5+ przedmiotow.
    int n10 = rows % 10, n100 = rows % 100;
    const char* word = (rows == 1) ? "przedmiot"
                     : (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14))
                       ? "przedmioty" : "przedmiotów";
    char buf[48];
    std::snprintf(buf, sizeof(buf), ", %d %s", rows, word);
    return buf;
}

// Shift+Left / Shift+Right: step the category filter on the focused side.
//
// A looted locker holds forty rows and the player wants four of them. The
// menu can already do that — each column filters by item category — but the
// engine gave the arrows that drive it to the mouse alone. We press them the
// way we press every other button in this mod: through the menu's own
// HandleClick, on the game's own thread, with no cursor moved and no key
// injected anywhere.
//
// The game still sees the bare arrow key under the Shift — we read key state,
// we do not swallow it — so if the engine turns out to answer Left and Right
// in this menu itself, both things will happen at once. The log below says
// which side ended up focused and what it now holds, which is what settles
// that question.
//
// Plain Left / Right switch COLUMNS: Left = your items, Right = the
// container's — the order they sit on screen. The menu itself has that
// switch (its Shift+Left/Right handler, see game::SwitchContainerSide); we run
// it on the game's thread, so it works whether or not the keyboard reaches it.
void PollFilterKeys()
{
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
              || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool keyL  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
    bool keyR  = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    bool left  = shift && keyL;
    bool right = shift && keyR;
    bool edgeL = left  && !g_kShiftLeft;
    bool edgeR = right && !g_kShiftRight;
    g_kShiftLeft  = left;
    g_kShiftRight = right;
    bool plainL = !shift && !ctrl && keyL;
    bool plainR = !shift && !ctrl && keyR;
    bool sideL  = plainL && !g_kLeft;
    bool sideR  = plainR && !g_kRight;
    g_kLeft  = plainL;
    g_kRight = plainR;
    bool up    = !shift && !ctrl && (GetAsyncKeyState(VK_UP)   & 0x8000) != 0;
    bool down  = !shift && !ctrl && (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
    bool stepU = up   && !g_kUp;
    bool stepD = down && !g_kDown;
    g_kUp   = up;
    g_kDown = down;
    if (menu::ActiveMenu() != menu::Id::Container) return;

    // Up/Down in the CONTAINER's column are done by us — the game's own only
    // ever move your column. In your column the game's own keys work.
    if ((stepU || stepD) && g_want_container) {
        poll::RequestContainerStep(true, stepD ? +1 : -1);
        g_restore_wait = 3;       // the step takes the focus back itself
        return;
    }

    if (sideL || sideR) {
        g_want_container = sideR;
        g_restore_tries = 0;
        poll::RequestContainerSide(sideR);
        g_side_target = sideR;
        g_side_wait   = 6;    // ~0.5 s
        return;
    }
    if (!edgeL && !edgeR) return;
    // The side the MENU has focused, when it can tell us; our own guess only
    // as a fallback.
    int focus = game::ContainerFocusSide();
    if (focus >= 0) g_on_container = (focus == 1);
    poll::RequestContainerFilter(g_on_container, edgeR);
    g_filter_say = 10;        // ~0.25 s for the engine to rebuild the column
}

// Read the new category back FROM THE GAME.
//
// Never from a list of our own: the categories are named by the player's own
// language file and the title tile above the column is where the engine
// writes the current one. On the container's side that title is the
// container's NAME, not a category — there the count is the honest answer,
// and it is the count that tells the player the filter did anything at all.
void AnnounceFilter()
{
    game::ContainerSide mine{}, theirs{};
    if (!game::GetContainerSides(&mine, &theirs)) return;
    const game::ContainerSide& s = g_on_container ? theirs : mine;
    std::string say = s.title.empty()
                    ? std::string(g_on_container ? "Pojemnik"
                                                 : "Twoje przedmioty")
                    : s.title;
    say += CountSuffix(s.rows);
    F3A_INFO("Container filter: %s side now '%s' rows=%d | other '%s' rows=%d",
             g_on_container ? "container" : "player", s.title.c_str(), s.rows,
             (g_on_container ? mine : theirs).title.c_str(),
             (g_on_container ? mine : theirs).rows);
    tolk::Speak(say, tolk::Priority::Ui, true);
}

// Say what the container holds the moment it opens.
//
// Without this the screen is silent about the one thing that matters: an empty
// locker and a full one sound exactly alike, and the player is left arrowing
// around a list that was never going to have anything in it. (A dump of a
// locker in the vault showed precisely that — the container's list had no rows
// at all, which is why "I cannot see the container's items" had no fix in the
// menu code: there were none.)
void AnnounceContents()
{
    game::ContainerSide mine{}, theirs{};
    if (!game::GetContainerSides(&mine, &theirs)) return;
    std::string say = theirs.title.empty() ? "Pojemnik" : theirs.title;
    say += CountSuffix(theirs.rows);
    F3A_INFO("Container: '%s' rows=%d | yours '%s' rows=%d",
             theirs.title.c_str(), theirs.rows, mine.title.c_str(), mine.rows);
    tolk::Speak(say, tolk::Priority::Ui, true);
}

// The toggle key ([ by default): jump to the other list. Same switch as
// Left/Right, for a player who prefers one key.
void SwitchSide()
{
    if (menu::ActiveMenu() != menu::Id::Container) return;
    int focus = game::ContainerFocusSide();
    bool onContainer = focus >= 0 ? (focus == 1) : g_on_container;
    poll::RequestContainerSide(!onContainer);
    g_want_container = !onContainer;
    g_restore_tries = 0;
    g_side_target = !onContainer;
    g_side_wait   = 6;
}

void OnOpen()  { g_last.clear(); g_last_box = nullptr; g_on_container = false;
                 g_filter_say = 0; g_filter_queue = false; g_side_wait = 0;
                 g_want_container = false; g_restore_wait = 0; g_restore_tries = 0;
                 g_kUp   = (GetAsyncKeyState(VK_UP)   & 0x8000) != 0;
                 g_kDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
                 // Keys already down when the menu opens must not fire.
                 g_kLeft  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
                 g_kRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
                 AnnounceContents(); OnTick(0); }
void OnClose() { g_last.clear(); g_last_box = nullptr;
                 g_filter_say = 0; g_filter_queue = false; g_side_wait = 0; }

} // namespace

void Init()
{
    hotkeys::Bind(config::Get().hotkeys.container_side, &SwitchSide);
    menu::RegisterMenu(menu::Id::Container, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::SleepWait, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Container module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::container
