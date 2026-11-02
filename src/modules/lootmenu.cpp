#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
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

// Taking an item. The overlay says nothing when the player presses Use on an
// entry: the entry just disappears and the next one slides into its place.
// So the press is noted with the entry highlighted at that moment (the list
// read afterwards may already show the next one in its slot), and "taken" is
// said once the list confirms it: that entry gone, its count smaller, or -
// the last one - the list gone. A press the game refuses (too heavy...)
// changes nothing, and nothing is said; the game's own message about it is
// read by the HUD reader. (Working the take out from list changes alone,
// without the press noted, missed takes: the player heard nothing.)
DWORD g_use_down_at = 0;            // last time the Use key was seen down
bool  g_use_was_down = false;
constexpr DWORD kUseRecentMs = 700;

// True on the press of the Use key (E unless rebound).
bool WatchUseKey()
{
    uint8_t sc = game::GetUseKeyScancode();
    UINT vk = sc ? MapVirtualKeyA(sc, MAPVK_VSC_TO_VK) : 0;
    if (!vk) vk = 'E';
    const bool down = (GetAsyncKeyState((int)vk) & 0x8000) != 0;
    const bool pressed = down && !g_use_was_down;
    g_use_was_down = down;
    if (down) g_use_down_at = GetTickCount();
    return pressed;
}

bool UseRecently() { return g_use_down_at && GetTickCount() - g_use_down_at <= kUseRecentMs; }

// A press of Use on the list, waiting for the list to confirm the take.
struct PendingTake {
    std::string item, title;
    int   total = 0;
    DWORD at = 0;          // 0 = none
};
PendingTake g_take;
constexpr DWORD kTakeWaitMs = 1500;

// "Śrut (25)" -> "Śrut", 25; no count -> the whole text, 1.
std::string SplitCount(const std::string& s, int* count)
{
    *count = 1;
    size_t open = s.rfind(" (");
    if (open == std::string::npos || s.empty() || s.back() != ')') return s;
    std::string num = s.substr(open + 2, s.size() - open - 3);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return s;
    *count = std::atoi(num.c_str());
    return s.substr(0, open);
}

// `count` taken of `name` ("Śrut, 25"; one: the name alone).
void SayTaken(const std::string& name, int count)
{
    // Taken from a body: it has been searched (the scanner says so).
    if (uint32_t id = game::GetCrosshairActorID(); id && game::IsKnownDead(id))
        game::NoteSearched(id);
    if (name.empty()) return;
    std::string said = name;
    if (count > 1) said += ", " + std::to_string(count);
    F3A_INFO("Loot menu: taken '%s'", said.c_str());
    tolk::Speak(strings::RenderArgs(strings::Key::LootTakenFmt, said.c_str()),
                tolk::Priority::Ui, true);
}

void Forget()
{
    g_take = {};
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

void SpeakEntry(const game::LootMenuInfo& v, bool interrupt = true)
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
    // No "equipped": this list is always someone else's - what a body still
    // wears - and the word was heard as the item being on the player, as in
    // the container's column (2026-10-31).
    tolk::Speak(say, tolk::Priority::Ui, interrupt);
}

} // namespace

bool IsOpen() { return g_open; }

void Tick(float)
{
    // E on the list: the entry highlighted until now is the one it takes -
    // noted before this tick's list, which may already show the next one.
    if (WatchUseKey() && g_open && g_gone_ticks == 0 && !g_last_item.empty()) {
        g_take.item  = g_last_item;
        g_take.title = g_last_title;
        g_take.total = g_last_total;
        g_take.at    = GetTickCount() | 1u;
        F3A_INFO("Loot menu: Use on '%s' (%d entries in '%s')",
                 g_take.item.c_str(), g_take.total, g_take.title.c_str());
    }
    bool taken = false;
    game::LootMenuInfo v;
    const bool shown = poll::LootMenuState(&v);
    if (g_take.at) {
        int was = 1, now = 1;
        const std::string wasName = SplitCount(g_take.item, &was);
        if (!shown) {
            // The last entry taken: the list goes away (a tick of it, not a
            // flicker of the overlay).
            if (g_take.total <= 1 && g_gone_ticks >= 1) {
                SayTaken(wasName, was);
                g_take = {};
                taken = true;
            }
        } else if (g_take.title != v.title) {
            g_take = {};                              // looking at another container
        } else {
            const std::string nowName = SplitCount(v.item, &now);
            if (g_take.total > 0 && v.total < g_take.total) {
                SayTaken(wasName, was);               // the entry went
                g_take = {};
                taken = true;
            } else if (nowName == wasName && now < was) {
                SayTaken(wasName, was - now);         // part of a stack
                g_take = {};
                taken = true;
            }
        }
        if (g_take.at && GetTickCount() - g_take.at > kTakeWaitMs) {
            F3A_INFO("Loot menu: Use on '%s' took nothing", g_take.item.c_str());
            g_take = {};
        }
    }
    if (!shown) {
        if (!g_open) return;
        if (++g_gone_ticks < kGoneTicksToClose) return;   // flicker, not a close
        Forget();
        tolk::Speak(strings::Render(strings::Key::LootClosed),
                    tolk::Priority::Ui, false);
        return;
    }
    g_gone_ticks = 0;

    // Another container: another title - or the same title with another
    // number of entries when nothing was taken (two "Szafka" lockers side by
    // side; a take always follows the Use key).
    const bool new_container = !g_open || g_last_title != v.title ||
                               (g_last_total > 0 && v.total != g_last_total && !UseRecently());
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
            SpeakEntry(v, !taken);        // after "Zabrano", not over it
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
