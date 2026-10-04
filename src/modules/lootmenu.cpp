#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace f3a::modules::lootmenu {
namespace {

// "Loot Menu Updated" (F3LootMenu.dll) replaces the container MENU with a list
// drawn straight onto the HUD: look at a crate and its contents appear, scroll
// with the wheel, take with a key, and the game never opens a menu at all.
//
// For a sighted player that is a convenience. For a blind one it is the only
// thing between them and the container, because our menu handlers never fire —
// there is no menu. So this module is what makes containers work at all once
// that mod is installed: it watches the overlay's own tile traits and reads out
// the container, the highlighted entry and its position in the list.

bool        g_open = false;
std::string g_last_title;
std::string g_last_item;
int         g_last_index = -1;
int         g_last_offset = -1;
int         g_last_total = -1;
bool        g_last_equipped = false;

// The overlay flickers: it drops to invisible for a frame or two while the
// player turns slightly or the list is rebuilt. Announcing the container again
// every time that happens is unbearable, so a close only counts once it has
// held for a few ticks (the New Vegas mod does the same, for the same reason).
int g_gone_ticks = 0;
constexpr int kGoneTicksToClose = 6;

void Forget()
{
    g_open = false;
    g_gone_ticks = 0;
    g_last_title.clear();
    g_last_item.clear();
    g_last_index = -1;
    g_last_offset = -1;
    g_last_total = -1;
    g_last_equipped = false;
}

void SpeakContainer(const game::LootMenuInfo& v)
{
    std::string say = strings::RenderArgs(strings::Key::LootContainerFmt, v.title);
    if (v.stealing) say += " " + strings::Render(strings::Key::LootStealing) + ".";
    if (v.weight[0]) {
        say += " " + strings::RenderArgs(strings::Key::LootWeightFmt, v.weight);
    }
    tolk::Speak(say, tolk::Priority::Ui, true);
}

void SpeakEntry(const game::LootMenuInfo& v)
{
    if (!v.item[0]) return;
    // Position is the scroll offset plus the highlight's place in the window —
    // the overlay only ever names the rows it is currently drawing.
    char at[16], of[16];
    std::snprintf(at, sizeof(at), "%d", v.offset + v.index + 1);
    std::snprintf(of, sizeof(of), "%d", v.total);
    std::string pos = strings::RenderArgs(strings::Key::LootPositionFmt, at, of);
    std::string say = strings::RenderArgs(strings::Key::LootItemFmt, v.item,
                                          pos.c_str());
    if (v.equipped) say += ", " + strings::Render(strings::Key::LootEquipped);
    tolk::Speak(say, tolk::Priority::Ui, true);
}

} // namespace

bool IsOpen() { return g_open; }

void Tick(float)
{
    game::LootMenuInfo v;
    if (!poll::LootMenuState(&v)) {
        if (!g_open) return;
        if (++g_gone_ticks < kGoneTicksToClose) return;   // flicker, not a close
        Forget();
        tolk::Speak(strings::Render(strings::Key::LootClosed),
                    tolk::Priority::Ui, false);
        return;
    }
    g_gone_ticks = 0;

    const bool new_container = !g_open || g_last_title != v.title;
    if (new_container) {
        g_open = true;
        g_last_title = v.title;
        g_last_index = -1;
        g_last_offset = -1;
        g_last_total = -1;
        g_last_item.clear();
        g_last_equipped = false;
        SpeakContainer(v);
        F3A_INFO("Loot menu: \"%s\", %d item(s)", v.title, v.total);
    }

    // Read the entry when the highlight moves, when the list scrolls, and also
    // when the text under an unchanged highlight changes — taking an item slides
    // the next one into the same slot, and that is still a new entry to hear.
    const bool moved = v.index != g_last_index || v.offset != g_last_offset;
    const bool text_changed = g_last_item != v.item;
    if (moved || text_changed || v.equipped != g_last_equipped) {
        g_last_index = v.index;
        g_last_offset = v.offset;
        g_last_equipped = v.equipped;
        if (text_changed || moved) {
            g_last_item = v.item;
            SpeakEntry(v);
        }
    }
    g_last_total = v.total;
}

void Init()
{
    Forget();
    F3A_INFO("Loot menu module ready.");
}

void Shutdown() { Forget(); }

} // namespace f3a::modules::lootmenu
