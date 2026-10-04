#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"
#include "f3a/fose_runtime.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"

#include <string>
#include <cstring>
#include <cstdio>

namespace f3a::modules::container {
namespace {

std::string g_last;
const void* g_last_box = nullptr;   // listbox of the last announcement
bool        g_on_container = false; // which side the focus was last put on

const char* SideName(const void* box);   // defined below

void OnTick(float)
{
    // Keyboard path first: it tracks the highlight across BOTH columns, so
    // switching sides with Left/Right (which never moves the mouse activeTile)
    // gets announced. We re-speak when the row text changes OR the focused
    // column (listbox) changes — the latter is exactly the Left/Right switch.
    auto sel = game::GetKeyboardSelection();
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
            if (const char* side = SideName(sel->container)) {
                text = std::string(side) + ": " + text;
                g_on_container = (std::strcmp(side, "Pojemnik") == 0);
            }
        }

        g_last     = text;
        g_last_box = sel->container;
        tolk::Speak(text, tolk::Priority::Ui, true);
        return;
    }

    // Fallback: the mouse/activeTile label (unchanged old behavior).
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
    if (theirs.rows <= 0) {
        say += ", pusty";
    } else {
        char buf[48];
        // Polish counts: 1 przedmiot, 2-4 przedmioty, 5+ przedmiotow.
        int n10 = theirs.rows % 10, n100 = theirs.rows % 100;
        const char* word = (theirs.rows == 1) ? "przedmiot"
                         : (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14))
                           ? "przedmioty" : "przedmiotów";
        std::snprintf(buf, sizeof(buf), ", %d %s", theirs.rows, word);
        say += buf;
    }
    F3A_INFO("Container: '%s' rows=%d | yours '%s' rows=%d",
             theirs.title.c_str(), theirs.rows, mine.title.c_str(), mine.rows);
    tolk::Speak(say, tolk::Priority::Ui, true);
}

// Jump between the two lists. The engine keeps the keyboard in your own
// inventory and offers no key for the other side, which is what made a
// container's contents unreachable without a mouse.
void SwitchSide()
{
    if (menu::ActiveMenu() != menu::Id::Container) return;
    bool toContainer = !g_on_container;
    if (!game::FocusContainerList(toContainer)) {
        tolk::Speak(toContainer ? "Pojemnik jest pusty."
                                : "Nie masz nic przy sobie.",
                    tolk::Priority::System, true);
        return;
    }
    g_on_container = toContainer;
    tolk::Speak(toContainer ? "Pojemnik" : "Twoje przedmioty",
                tolk::Priority::Ui, true);
}

void OnOpen()  { g_last.clear(); g_last_box = nullptr; g_on_container = false;
                 AnnounceContents(); OnTick(0); }
void OnClose() { g_last.clear(); g_last_box = nullptr; }

} // namespace

void Init()
{
    hotkeys::Bind(config::Get().hotkeys.container_side, &SwitchSide);
    menu::RegisterMenu(menu::Id::Container, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Quantity,  &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::SleepWait, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Container module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::container
