#include "f3a/polling_loop.h"
#include "f3a/fose_runtime.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <cstring>
#include <cstdarg>

namespace f3a::poll {
namespace {

std::thread             g_thread;
std::atomic<bool>       g_running{ false };

// Visibility state per cache slot (0..0x3B).
//   confirmed: what we've reported as the current state to menu_dispatch.
//   pending  : last reading that hasn't yet been confirmed by a 2nd tick.
//   debounce : how many consecutive ticks we've seen the pending value.
struct VisCell {
    bool confirmed = false;
    bool pending   = false;
    int  debounce  = 0;
};
VisCell g_state[0x3C];

// First-baseline approach: as soon as InterfaceManager exists, we snapshot
// whatever the MenuVisibilityArray currently says and treat that as the
// starting state — neither announced nor counted as "open". From that
// instant we only react to *transitions*. This avoids both:
//   (a) announcing "SPECIAL Book opened" because uninitialized memory
//       happens to read as true on slot 0x3B;
//   (b) requiring a specific menu (like Start) to appear before we arm
//       — main menu may use HUDMain or no menu at all during intro.
bool g_baseline_taken = false;

// During splash/preload the engine briefly paints Pip-Boy backdrops (Map,
// Stats, Inventory) before Start finally opens. We don't want to announce
// those phantom menus. Gate dispatch until we've seen either Start (main
// menu) or HUDMain (in-game) at least once — after that, treat all
// transitions as real.
bool g_system_ready = false;

// Same backdrop paint happens transiently right after a Loading screen
// closes. We suppress Pip-Boy sub-menu opens for a short window after
// Loading goes away — long enough to absorb engine post-load jitter, short
// enough that an intentional Pip-Boy open right after a save load still
// announces. At kPollIntervalMs the ticker runs ~40 ticks/sec, so 40 ticks
// is roughly 1 second.
int g_postload_cooldown = 0;
constexpr int kPostLoadCooldownTicks = 40;

// Tracks where the player is in the game lifecycle. Initially the engine
// is in splash; once Start opens we're "in main menu"; once HUDMain opens
// we're "in gameplay" (main menu has been exited). This is independent of
// system_ready (which is just "did we see anything yet") and is used by
// hotkeys and the dispatch filter to behave correctly per state.
bool g_in_main_menu = false;
bool g_in_gameplay  = false;

bool IsPipBoyTab(UInt32 menuType)
{
    return menuType == kMenuType_Map      ||
           menuType == kMenuType_Stats    ||
           menuType == kMenuType_Inventory ||
           menuType == kMenuType_Book;
}

// Menus the engine paints as backdrop while the main menu is up (Start
// menu). Now that the tile-list walker correctly sees all of menuRoot,
// these started getting dispatched as opens. They're never legitimate
// during main_menu — char-gen menus only show up after HUDMain opens.
bool IsMainMenuBackdrop(UInt32 menuType)
{
    switch (menuType) {
    case kMenuType_LevelUp:
    case kMenuType_Tutorial:
    case kMenuType_Message:
    case kMenuType_SPECIALBook:
    case kMenuType_RaceSex:
    case kMenuType_TextEdit:
    case kMenuType_Book:
    case kMenuType_Quantity:
    case kMenuType_SleepWait:
        return true;
    default:
        return false;
    }
}

// DumpActiveMenuTree lives outside the anonymous namespace so the public
// header can forward-declare it — see below after Stop().

// Need this many consecutive ticks of the new value before we report it.
constexpr int kDebounceTicks = 2;

inline int CacheSlot(UInt32 menu_type)
{
    if (menu_type < kMenuType_Message) return -1;
    int slot = (int)(menu_type - kMenuType_Message);
    return (slot >= 0 && slot < 0x3C) ? slot : -1;
}

menu::Id ToInternalId(UInt32 fose_type)
{
    switch (fose_type) {
    case kMenuType_Message:        return menu::Id::Message;
    case kMenuType_Inventory:      return menu::Id::Inventory;
    case kMenuType_Stats:          return menu::Id::Stats;
    case kMenuType_HUDMain:        return menu::Id::HUDMain;
    case kMenuType_Loading:        return menu::Id::Loading;
    case kMenuType_Container:      return menu::Id::Container;
    case kMenuType_Dialog:         return menu::Id::Dialog;
    case kMenuType_SleepWait:      return menu::Id::SleepWait;
    case kMenuType_Start:          return menu::Id::Start;
    case kMenuType_LockPick:       return menu::Id::LockPick;
    case kMenuType_Quantity:       return menu::Id::Quantity;
    case kMenuType_Map:            return menu::Id::Map;
    case kMenuType_Book:           return menu::Id::Book;
    case kMenuType_LevelUp:        return menu::Id::LevelUp;
    case kMenuType_Repair:         return menu::Id::Repair;
    case kMenuType_RaceSex:        return menu::Id::Race;
    case kMenuType_TextEdit:       return menu::Id::TextEdit;
    case kMenuType_Barter:         return menu::Id::Barter;
    case kMenuType_Surgery:        return menu::Id::Surgery;
    case kMenuType_Hacking:        return menu::Id::HackingShort;
    case kMenuType_VATS:           return menu::Id::VATS;
    case kMenuType_Computers:      return menu::Id::Computers;
    case kMenuType_RepairServices: return menu::Id::RepairServices;
    case kMenuType_Tutorial:       return menu::Id::Tutorial;
    case kMenuType_SPECIALBook:    return menu::Id::SpecialBookMenu;
    default:                       return menu::Id::None;
    }
}

const char* DebugName(UInt32 t)
{
    switch (t) {
    case kMenuType_Message:        return "Message";
    case kMenuType_Inventory:      return "Inventory";
    case kMenuType_Stats:          return "Stats";
    case kMenuType_HUDMain:        return "HUDMain";
    case kMenuType_Loading:        return "Loading";
    case kMenuType_Container:      return "Container";
    case kMenuType_Dialog:         return "Dialog";
    case kMenuType_SleepWait:      return "SleepWait";
    case kMenuType_Start:          return "Start";
    case kMenuType_LockPick:       return "LockPick";
    case kMenuType_Quantity:       return "Quantity";
    case kMenuType_Map:            return "Map";
    case kMenuType_Book:           return "Book";
    case kMenuType_LevelUp:        return "LevelUp";
    case kMenuType_Repair:         return "Repair";
    case kMenuType_RaceSex:        return "RaceSex";
    case kMenuType_TextEdit:       return "TextEdit";
    case kMenuType_Barter:         return "Barter";
    case kMenuType_Surgery:        return "Surgery";
    case kMenuType_Hacking:        return "Hacking";
    case kMenuType_VATS:           return "VATS";
    case kMenuType_Computers:      return "Computers";
    case kMenuType_RepairServices: return "RepairServices";
    case kMenuType_Tutorial:       return "Tutorial";
    case kMenuType_SPECIALBook:    return "SPECIALBook";
    default:                       return "?";
    }
}

// Cached set of menu typeIDs we confirmed visible last debounce-pass.
std::unordered_set<UInt32> g_confirmed_types;
std::unordered_set<UInt32> g_pending_types;
int  g_pending_match_count = 0;

// Active tile poller — last tile name we announced.
std::string g_last_tile_name;
const Tile* g_last_tile_ptr = nullptr;

// Keyboard selection poller — last focused row we announced. Tracked split
// so that moving to a new row reads "label, value" while only changing a
// slider value (left/right on the same row) reads just the new value.
std::string g_last_kbd_label;
std::string g_last_kbd_value;
const void* g_last_kbd_container = nullptr;

// Filter applied to the active-menu set before dispatch. While Start (main
// menu) is up F3 paints Map / Stats / Inventory as backdrop tiles; we don't
// want our pip-boy / map modules reacting to them.
std::unordered_set<UInt32> FilterMenusForDispatch(
    const std::unordered_set<UInt32>& in)
{
    if (!in.count(kMenuType_Start)) return in;

    // Only Loading is a legitimate companion of Start (splash → main menu
    // transition). Everything else painted by the engine while Start is
    // up — LevelUp, Message, Map, etc. — is backdrop noise from menuRoot
    // and would generate false announcements.
    std::unordered_set<UInt32> out;
    out.insert(kMenuType_Start);
    if (in.count(kMenuType_Loading)) out.insert(kMenuType_Loading);
    return out;
}

// Poll IFM->activeTile and speak the resolved label on change.
// activeTile follows mouse hover (not keyboard selection). We speak only
// when we can resolve a real user-facing string for the tile — internal
// names like "item hotrect" are dropped so the mouse doesn't generate noise.
void PollActiveTile()
{
    if (!g_system_ready) return;

    InterfaceManager* ifm = fose_rt::IFM();
    if (!ifm) return;
    Tile* at = ifm->activeTile;
    if (at == g_last_tile_ptr) return;
    g_last_tile_ptr = at;

    if (!at) {
        g_last_tile_name.clear();
        return;
    }

    // Log every activeTile pointer change — even if we can't resolve a
    // label, this tells us the engine IS moving focus (e.g., keyboard nav
    // would manifest as repeated activeTile updates).
    const char* internal = fose_rt::TileName(at);
    F3A_DEBUG("activeTile change: ptr=%p name='%s'",
              at, internal && *internal ? internal : "?");

    auto label = game::GetActiveMenuSelectionText();
    if (!label || label->empty()) {
        // Could not resolve a real label — keep quiet rather than read
        // an internal tile identifier.
        return;
    }
    if (*label == g_last_tile_name) return;
    g_last_tile_name = *label;

    F3A_DEBUG("activeTile -> '%s'", label->c_str());
    tolk::Speak(*label, tolk::Priority::Ui, true);
}

// Poll the menu tree for a tile that looks keyboard-focused (user-trait >=
// 0.5) and speak its label on change. Runs alongside PollActiveTile —
// activeTile follows mouse, this follows the keyboard cursor.
void PollKeyboardSelection()
{
    if (!g_system_ready) return;

    auto sel = game::GetKeyboardSelection();
    if (!sel || (sel->label.empty() && sel->value.empty())) {
        // Don't clear last state here — when focus briefly drops (a tick
        // where nothing reads as selected) we'd otherwise re-announce.
        return;
    }

    const bool container_changed = sel->container != g_last_kbd_container;
    const bool label_changed     = sel->label != g_last_kbd_label;
    const bool value_changed     = sel->value != g_last_kbd_value;
    if (!container_changed && !label_changed && !value_changed) return;

    std::string utterance;
    if (label_changed || container_changed) {
        // Moved to a different row → announce the full "label, value".
        // Entering a NEW panel (confirmation box, settings page) → prefix
        // its static text, e.g. "Rozpocząć nową grę? Tak".
        if (container_changed && !sel->context.empty()) {
            utterance = sel->context;
            utterance += " ";
        }
        utterance += sel->label;
        if (!sel->value.empty()) {
            if (!utterance.empty()) utterance += ", ";
            utterance += sel->value;
        }
    } else {
        // Same row, value tweaked (left/right on a slider) → just the value.
        utterance = sel->value;
    }

    g_last_kbd_label     = sel->label;
    g_last_kbd_value     = sel->value;
    g_last_kbd_container = sel->container;
    if (utterance.empty()) return;

    // Avoid doubling with what the mouse poller just said.
    if (utterance == g_last_tile_name) return;

    F3A_DEBUG("kbd selection -> '%s'", utterance.c_str());
    tolk::Speak(utterance, tolk::Priority::Ui, true);
}

void LogSet(const char* tag, const std::unordered_set<UInt32>& s)
{
    char buf[512]; int len = 0;
    for (UInt32 t : s) {
        if (len > (int)sizeof(buf) - 32) break;
        len += snprintf(buf + len, sizeof(buf) - len, "%s(0x%X) ",
                        DebugName(t), t);
    }
    F3A_INFO("%s [%s]", tag, len ? buf : "<empty>");
}

void Tick(float dt)
{
    // Don't touch anything until the InterfaceManager has been created.
    InterfaceManager* ifm = fose_rt::IFM();
    if (!ifm) return;

    // Lifecycle flags from the source of truth, recomputed every tick.
    //   * The F3 main menu is actually MapMenu — Start (0x3F5) only flashes
    //     briefly — so "is Start up" can't identify the main menu.
    //   * The in-game pause menu IS Start, the same type as the main menu.
    //   * The only reliable discriminator is whether a game session exists:
    //     at the main menu / loading there is no player (g_thePlayer null);
    //     once a save is loaded or a new game starts, the player is valid and
    //     stays valid through the pause menu.
    //   So: in_gameplay = player exists; in_main_menu = no player.
    g_in_gameplay  = game::IsPlayerValid();
    g_in_main_menu = !g_in_gameplay;

    // Walk the actual UI tree (menuRoot) — same data the game renders.
    // In gameplay, require TileMenu visibility: the engine permanently
    // mounts every menu in menuRoot once a game is loaded, so presence
    // alone would read as a storm of phantom "opens" (Repair, Inventory,
    // barter totals...) right after load. Open menus have visible=1.
    std::unordered_set<UInt32> raw_active;
    fose_rt::CollectActiveMenuTypes(raw_active, /*only_visible=*/g_in_gameplay);
    auto active = FilterMenusForDispatch(raw_active);

    // First tick: take baseline, no dispatch.
    if (!g_baseline_taken) {
        g_confirmed_types = active;
        g_pending_types   = active;
        g_pending_match_count = kDebounceTicks;
        g_baseline_taken = true;
        LogSet("Baseline raw      :", raw_active);
        LogSet("Baseline filtered :", active);
        return;
    }

    // Debounce: require kDebounceTicks consecutive identical readings.
    if (active == g_pending_types) {
        if (g_pending_match_count < kDebounceTicks) g_pending_match_count++;
    } else {
        g_pending_types = active;
        g_pending_match_count = 1;
    }

    if (g_pending_match_count >= kDebounceTicks &&
        active != g_confirmed_types) {

        // Flip the system-ready latch the first time Start or HUDMain is
        // confirmed. Everything earlier (phantom Pip-Boy backdrops painted
        // during splash) is absorbed into the baseline without dispatch.
        if (!g_system_ready) {
            if (active.count(kMenuType_Start) ||
                active.count(kMenuType_HUDMain)) {
                g_system_ready = true;
                F3A_INFO("System ready (saw Start/HUDMain). Arming dispatch.");
            } else {
                // Re-baseline: absorb whatever the engine is painting now.
                g_confirmed_types = active;
                return;
            }
        }

        // (Lifecycle flags g_in_main_menu / g_in_gameplay are computed at the
        // top of Tick from player validity — see above.)

        // Compute opened / closed deltas.
        for (UInt32 t : active) {
            if (g_confirmed_types.count(t)) continue;
            // Suppression rules:
            //  - During post-Loading cooldown OR while still in main menu:
            //    Pip-Boy sub-menus are always engine backdrop, never the
            //    player opening Pip-Boy.
            //  - While in main menu: backdrop menus (LevelUp, Message,
            //    Tutorial, RaceSex, TextEdit, Book, Quantity, SleepWait)
            //    are all painted by the engine but the player can't open
            //    them — only Start itself is real.
            bool suppress =
                ((g_postload_cooldown > 0 || g_in_main_menu) && IsPipBoyTab(t)) ||
                (g_in_main_menu && IsMainMenuBackdrop(t));
            if (suppress) {
                static UInt32 last_suppressed = 0;
                if (t != last_suppressed) {
                    F3A_DEBUG("Suppressed (main_menu/cooldown) open %s (0x%X)",
                              DebugName(t), t);
                    last_suppressed = t;
                }
                continue;
            }
            F3A_DEBUG("Menu open: %s (0x%X)", DebugName(t), t);
            menu::Id id = ToInternalId(t);
            if (id != menu::Id::None) menu::OnMenuOpen(id);

            // Start is the main menu OR the in-game pause menu (same type).
            //   * Main menu (no player): announce "Menu główne otwarte".
            //   * Pause menu (in-game): announce nothing — the selection
            //     poller reads the first focused element instead.
            // menu_dispatch suppresses Start's generic announcement either way.
            if (t == kMenuType_Start && !g_in_gameplay) {
                tolk::Speak(
                    strings::RenderArgs(strings::Key::MenuOpened, "Menu główne"),
                    tolk::Priority::Ui, true);
            }
        }
        for (UInt32 t : g_confirmed_types) {
            if (active.count(t)) continue;
            F3A_DEBUG("Menu close: %s (0x%X)", DebugName(t), t);
            // Loading just closed → arm post-load cooldown.
            if (t == kMenuType_Loading) {
                g_postload_cooldown = kPostLoadCooldownTicks;
                F3A_DEBUG("Loading closed; cooldown=%d ticks",
                          g_postload_cooldown);
            }
            menu::Id id = ToInternalId(t);
            if (id != menu::Id::None) menu::OnMenuClose(id);
        }
        // Filter out Pip-Boy sub-menus from confirmed set during cooldown
        // so when they're later legitimately opened, the delta still
        // fires.
        if (g_postload_cooldown > 0) {
            std::unordered_set<UInt32> filtered;
            for (UInt32 t : active) if (!IsPipBoyTab(t)) filtered.insert(t);
            g_confirmed_types = std::move(filtered);
        } else {
            g_confirmed_types = active;
        }
    }

    if (g_postload_cooldown > 0) g_postload_cooldown--;

    menu::OnTick(dt);
    PollActiveTile();
    PollKeyboardSelection();

    if (config::IsEnabled()) {
        hotkeys::Poll();
        modules::autowalk::Tick(dt);
    }
}

void ThreadProc()
{
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    F3A_INFO("Polling loop started (every %d ms).", kPollIntervalMs);
    while (g_running.load(std::memory_order_relaxed)) {
        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        try { Tick(dt); }
        catch (...) { F3A_ERROR("Exception in polling tick."); }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    F3A_INFO("Polling loop stopped.");
}

} // namespace

void Start()
{
    if (g_running.exchange(true)) return;
    g_baseline_taken    = false;
    g_system_ready      = false;
    g_postload_cooldown = 0;
    g_in_main_menu      = false;
    g_in_gameplay       = false;
    g_confirmed_types.clear();
    g_pending_types.clear();
    g_pending_match_count = 0;
    g_last_tile_name.clear();
    g_last_tile_ptr = nullptr;
    g_last_kbd_label.clear();
    g_last_kbd_value.clear();
    g_last_kbd_container = nullptr;
    for (auto& c : g_state) c = {};
    g_thread = std::thread(&ThreadProc);
}

void Stop()
{
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

// --- Diagnostic dump --------------------------------------------------------
//
// Recursively log a Tile's name + interesting traits (string content, user
// traits != 0, visibility, list index). Triggered manually by hotkey so we
// can figure out the per-menu "selected" marker convention.

namespace {

// Count children of a tile (for dump diagnostics). Walks tList<ChildNode>
// the same way FOSE's Iterator does: termination is when the LIST NODE is
// null, not when item is null. Earlier we broke on null item and missed
// every sibling past the first "hole" in the list.
int CountChildren(Tile* t)
{
    if (!t) return 0;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    int n = 0;
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) n++;
        node = node->next;
    }
    return n;
}

void DumpTileRec(Tile* t, int depth)
{
    if (!t || depth > 12) return;
    const char* name = fose_rt::TileName(t);

    int kids = CountChildren(t);

    // Gather a wide selection of interesting traits, not just the few we
    // initially looked at. ANYTHING that could plausibly mark "selected".
    char buf[512]; int n = 0;
    buf[0] = 0;
    auto add = [&](const char* fmt, ...) {
        if (n >= (int)sizeof(buf) - 16) return;
        va_list ap; va_start(ap, fmt);
        int w = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
        va_end(ap);
        if (w > 0) n += w;
    };

    for (UInt32 i = 0; i < t->values.size && n < (int)sizeof(buf) - 32; ++i) {
        Tile::Value* v = t->values.data[i];
        if (!v) continue;
        if (v->id == kTileValue_string && v->str && *v->str) {
            add(" str='%.48s'", v->str);
        } else if (v->id >= kTileValue_user0 && v->id <= kTileValue_user16) {
            if (v->num != 0.0f)
                add(" user%u=%.2f", v->id - kTileValue_user0, v->num);
        } else if (v->id == kTileValue_visible && v->num != 0.0f) {
            add(" vis=1");
        } else if (v->id == kTileValue_listindex && v->num != 0.0f) {
            add(" idx=%.0f", v->num);
        } else if (v->id == kTileValue_x      && v->num != 0.0f) add(" x=%.0f", v->num);
        else if   (v->id == kTileValue_y      && v->num != 0.0f) add(" y=%.0f", v->num);
        else if   (v->id == kTileValue_height && v->num != 0.0f) add(" h=%.0f", v->num);
        else if   (v->id == kTileValue_width  && v->num != 0.0f) add(" w=%.0f", v->num);
        else if   (v->id == kTileValue_alpha && v->num != 1.0f && v->num != 0.0f) {
            add(" alpha=%.2f", v->num);
        } else if (v->id == kTileValue_target && v->num != 0.0f) add(" tgt=%.0f", v->num);
        else if   (v->id == kTileValue_mouseover && v->num != 0.0f) add(" hover=1");
        else if   (v->id == kTileValue_clicked   && v->num != 0.0f) add(" clk=1");
    }

    log::DumpWrite("%*s[%s] (%d kids)%s",
             depth * 2, "", name && *name ? name : "?", kids, buf);

    // Dump EVERY raw trait for tiles likely involved in selection: list
    // rows / highlight boxes, anything clickable (target trait set), and
    // anything carrying a label. Lets us decode unfamiliar panels (settings
    // sliders, dropdowns) from a single dump.
    bool has_target = false, has_label = false;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (!v) continue;
        if (v->id == kTileValue_target && v->num != 0.0f) has_target = true;
        if (v->id == kTileValue_string && v->str && *v->str) has_label = true;
    }
    if ((name && (strstr(name, "hotrect") || strstr(name, "highlight") ||
                  strstr(name, "option") || strstr(name, "setting") ||
                  strstr(name, "slider")))
        || has_target || has_label) {
        char raw[640]; int rn = 0; raw[0] = 0;
        for (UInt32 i = 0; i < t->values.size && rn < (int)sizeof(raw) - 24; ++i) {
            Tile::Value* v = t->values.data[i];
            if (!v) continue;
            rn += snprintf(raw + rn, sizeof(raw) - rn, " 0x%X=%.1f",
                           v->id, v->num);
        }
        log::DumpWrite("%*s    RAW:%s", depth * 2, "", raw);
    }

    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    int visited = 0;
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            DumpTileRec(cn->child, depth + 1);
            visited++;
        }
        node = node->next;
    }
    if (visited != kids) {
        log::DumpWrite("%*s  (walk visited %d of %d kids)",
                 depth * 2, "", visited, kids);
    }
}
} // namespace

bool IsGameplayActive() { return g_in_gameplay && !g_in_main_menu; }
bool IsInMainMenu()     { return g_in_main_menu; }

void DumpActiveMenuTree()
{
    InterfaceManager* ifm = fose_rt::IFM();
    if (!ifm) {
        F3A_INFO("DumpMenuTree: no interface manager.");
        return;
    }
    // Write the tree to the dedicated small dump file, not the rolling log.
    log::BeginDump();
    log::DumpWrite("===== Active menu tree dump =====");
    log::DumpWrite("in_main_menu=%d in_gameplay=%d",
                   (int)g_in_main_menu, (int)g_in_gameplay);
    {
        auto kbd = game::GetKeyboardSelectionText();
        auto mouse = game::GetActiveMenuSelectionText();
        log::DumpWrite("GetKeyboardSelectionText() -> '%s'",
                       kbd ? kbd->c_str() : "<null>");
        log::DumpWrite("GetActiveMenuSelectionText() -> '%s'",
                       mouse ? mouse->c_str() : "<null>");
    }
    log::DumpWrite("IFM=%p menuRoot=%p activeTile=%p cursor=%p",
                   ifm, ifm->menuRoot, ifm->activeTile, ifm->cursor);
    log::DumpWrite("  unk0A0=%p unk0A4=%p pipboyMgr=%p",
                   ifm->unk0A0, ifm->unk0A4, ifm->pipboyManager);

    auto dump_root = [&](const char* tag, Tile* root) {
        if (!root) { log::DumpWrite("--- %s: <null>", tag); return; }
        log::DumpWrite("--- %s: %p (%d kids) ---",
                       tag, root, CountChildren(root));
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = reinterpret_cast<Node*>(&root->childList);
        int n = 0;
        for (int safety = 0; node && safety < 4096; ++safety) {
            Tile::ChildNode* cn = node->item;
            if (cn && cn->child) {
                log::DumpWrite("  [child #%d]", n++);
                DumpTileRec(cn->child, 1);
            }
            node = node->next;
        }
        if (n == 0) log::DumpWrite("  (no children)");
    };

    dump_root("menuRoot", ifm->menuRoot);
    dump_root("unk0A0",   ifm->unk0A0);
    if (ifm->activeTile) {
        log::DumpWrite("--- activeTile (mouse hover) ---");
        DumpTileRec(ifm->activeTile, 0);
    }
    if (ifm->cursor) {
        log::DumpWrite("--- cursor tile ---");
        DumpTileRec(ifm->cursor, 0);
    }
    log::DumpWrite("===== End dump =====");
    log::EndDump();
    tolk::Speak("Drzewo menu zapisane w pliku dump.",
                tolk::Priority::System, true);
}

} // namespace f3a::poll
