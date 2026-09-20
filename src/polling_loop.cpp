#include "f3a/polling_loop.h"
#include "f3a/player_mover.h"
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
#include <cmath>

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
    // The text-entry popup has its own reader (modules::message TextEdit handler)
    // that echoes typed characters; the generic focus poller would double it up
    // by re-reading the prompt/field every tick. Let the dedicated one own it.
    if (menu::ActiveMenu() == menu::Id::TextEdit) return;

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
// Terminal/computer screen. Read text LIVE, as it appears, like F4 Access: the
// boot/welcome banner and headers ("Witamy w terminalu ROBCO Industries…",
// "ZUNIFIKOWANY SYSTEM…") the moment they show, and the opened entry body the
// moment it settles. The selectable command list is read by the focus poller.
std::vector<std::string> g_term_prev_chrome;   // chrome lines present last poll (stability)
std::vector<std::string> g_term_chrome_seen;   // chrome lines already spoken this session
std::string g_term_body_last;                  // last-spoken entry body
std::string g_term_body_pending;               // body awaiting a short settle
DWORD       g_term_body_since = 0;
// The type-out reveal changes the tile string frame-by-frame; require a line to
// persist across one poll (chrome) / this long (body) before speaking, so we
// read whole lines, not half-typed fragments — but with no perceptible delay.
constexpr DWORD kTermBodySettleMs = 150;

bool TermVecHas(const std::vector<std::string>& v, const std::string& s)
{
    for (const auto& e : v) if (e == s) return true;
    return false;
}

void PollTerminal()
{
    if (!g_system_ready) return;
    if (!game::IsTerminalOpen()) {                     // terminal closed → reset
        g_term_prev_chrome.clear(); g_term_chrome_seen.clear();
        g_term_body_last.clear();   g_term_body_pending.clear();
        return;
    }
    // Chrome (welcome/boot banner/headers/result echoes): speak each line ONCE,
    // as soon as it has been on screen for a full poll (so the type-out reveal
    // isn't read as fragments).
    std::vector<std::string> chrome = game::CollectTerminalChrome();
    for (const auto& s : chrome) {
        if (TermVecHas(g_term_chrome_seen, s)) continue;
        if (TermVecHas(g_term_prev_chrome, s)) {
            g_term_chrome_seen.push_back(s);
            tolk::Speak(s, tolk::Priority::Ui, /*interrupt=*/false);
        }
    }
    g_term_prev_chrome = std::move(chrome);

    // Body (the opened log/report/note). Speak when it settles to a new value —
    // re-reads whenever a different entry is opened.
    auto bodyOpt = game::GetTerminalText();
    std::string body = bodyOpt ? *bodyOpt : std::string();
    if (body.empty()) { g_term_body_last.clear(); g_term_body_pending.clear(); return; }
    DWORD now = GetTickCount();
    if (body != g_term_body_pending) { g_term_body_pending = body; g_term_body_since = now; return; }
    if (now - g_term_body_since < kTermBodySettleMs) return;
    if (body == g_term_body_last) return;
    g_term_body_last = body;
    tolk::Speak(body, tolk::Priority::Ui, /*interrupt=*/false);
}

// Character creation (RaceSexMenu): the focused category/option isn't picked up
// by the generic pollers (no activeTile), so read it specially — announce the
// focused list item on change so sex/race/hair/etc. can be navigated by ear.
std::string g_rsm_text;
int         g_rsm_throttle = 0;

void PollRaceSex()
{
    if (!g_system_ready) return;
    if (--g_rsm_throttle > 0) return;
    g_rsm_throttle = 2;
    auto sel = game::GetRaceSexSelection();
    if (!sel) { g_rsm_text.clear(); return; }
    if (*sel == g_rsm_text) return;
    g_rsm_text = *sel;
    tolk::Speak(*sel, tolk::Priority::Ui, true);
}

// HUD corner notifications (quest updates, items received, XP, discoveries) —
// the passive "background text" a sighted player sees top-left. Read on change
// during free gameplay so it isn't missed.
std::string g_hud_msg;

void PollHudMessages()
{
    if (!g_system_ready || !IsGameplayActive()) { g_hud_msg.clear(); return; }
    menu::Id m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) return;   // free gameplay only
    std::string msg = game::GetHudMessage();
    if (msg == g_hud_msg) return;
    g_hud_msg = msg;
    if (!msg.empty()) tolk::Speak(msg, tolk::Priority::Ui, /*interrupt=*/false);
}

// Crosshair activate prompt (HUD "Info" tile): speak the verb + target you're
// about to interact with — "Rozmawiaj", "Weź", "Okradnij", "Otwórz"… — so a
// blind player knows the action before pressing Use (hearing "Okradnij" avoids
// a karma-losing theft). Short settle so sweeping the view doesn't chatter.
std::string g_act_prompt;
uint32_t    g_act_refid = 0, g_act_pending_refid = 0;
DWORD       g_act_since = 0;

void PollActivatePrompt()
{
    auto reset = [] { g_act_prompt.clear(); g_act_refid = 0; g_act_pending_refid = 0; };
    if (!g_system_ready || !IsGameplayActive()) { reset(); return; }
    menu::Id m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) { reset(); return; }

    // Debounce on the picked REFERENCE, not on the prompt text: sweeping the view
    // changes the ref many times a second, and the verb alone repeats across
    // different targets ("Weź" for every item), so text-diffing both chatters and
    // silently swallows real target changes. (Same reason the FNV mod debounces
    // its crosshair ref for ~10 frames.)
    game::CrosshairTarget t;
    bool have = game::GetCrosshairTarget(&t);
    std::string verb = game::GetActivatePrompt();
    if (!have && verb.empty()) { reset(); return; }

    DWORD now = GetTickCount();
    if (t.refid != g_act_pending_refid) { g_act_pending_refid = t.refid; g_act_since = now; return; }
    if (now - g_act_since < 200) return;             // let the crosshair settle
    if (t.refid == g_act_refid) return;

    // Verb + what it is + how far. The verb alone ("Okradnij") doesn't say WHO,
    // and enemies have no verb at all — the name is what the player needs.
    const auto& c = config::Get();
    std::string say = verb;
    if (c.crosshair_names && !t.name.empty() && t.name != verb) {
        if (!say.empty()) say += ", ";
        say += t.name;
    }
    if (say.empty()) return;
    if (c.crosshair_names && c.crosshair_distance && t.dist > 0.0f)
        say += ", " + strings::FormatDistance(t.dist);

    g_act_refid  = t.refid;
    g_act_prompt = say;
    tolk::Speak(say, tolk::Priority::Ui, /*interrupt=*/false);
}

void PollKeyboardSelection()
{
    if (!g_system_ready) return;
    if (menu::ActiveMenu() == menu::Id::TextEdit) return;   // TextEdit reader owns it

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

// Announce when the player's ACTIVE QUEST changes (quest-level only, not every
// sub-objective). We compare the TESQuest pointer behind the tracked objective;
// it stays constant across a quest's sub-objectives, so this stays quiet until
// you actually move to a different quest. Skipped while a menu is up so a Pip-
// Boy quest-switch is announced once, on close, not mid-browse.
const void* g_last_quest = nullptr;
std::string g_last_obj;          // last announced objective text
bool        g_quest_seen = false;

void PollQuestChange()
{
    if (!IsGameplayActive()) { g_quest_seen = false; return; }
    auto m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) return;  // menu up: hold
    const void* q = game::GetTrackedQuestPtr();
    if (!q) return;

    // Read the current objective text too, so we also catch an objective
    // advancing WITHIN the same quest ("Wejdź do gabinetu" -> "Wejdź do
    // sterowni") — previously only a whole-quest switch was announced, which is
    // why updates felt delayed/missed.
    std::string obj;
    { auto qt = game::GetCurrentQuestTarget(); if (qt.valid) obj = qt.name; }

    if (!g_quest_seen) {                       // first read after load: silent
        g_last_quest = q; g_last_obj = obj; g_quest_seen = true; return;
    }
    bool quest_changed = (q != g_last_quest);
    bool obj_changed   = (obj != g_last_obj);
    if (!quest_changed && !obj_changed) return;
    g_last_quest = q;
    g_last_obj   = obj;

    if (quest_changed) {
        std::string name = game::GetTrackedQuestName();
        std::string line = "Zadanie: " + (name.empty() ? obj : name);
        if (!name.empty() && !obj.empty()) line += ", " + obj;
        tolk::Speak(line, tolk::Priority::Background, false);
    } else if (!obj.empty()) {
        tolk::Speak("Nowy cel: " + obj, tolk::Priority::Background, false);
    }
}

// Announce SPECIAL attribute changes (Strength..Luck = AV codes 5..11). Makes
// the image-only "You're SPECIAL!" book (and any +SPECIAL effect) accessible:
// when a value changes we say "<name> <new value>". Throttled, and only while in
// gameplay (where SPECIAL can change), since it calls a game getter per attr.
const char* g_special_names[7] = {
    "Siła", "Percepcja", "Wytrzymałość", "Charyzma",
    "Inteligencja", "Zręczność", "Szczęście"
};
int  g_special[7]      = { -1, -1, -1, -1, -1, -1, -1 };
bool g_special_init    = false;
int  g_special_throttle = 0;

void PollSpecialChange()
{
    if (!IsGameplayActive() || g_postload_cooldown > 0) {
        g_special_init = false;
        return;
    }
    if (--g_special_throttle > 0) return;
    g_special_throttle = 6;                 // ~6 ticks between polls

    int cur[7];
    for (int i = 0; i < 7; ++i) {
        float v = game::GetPlayerAV(5 + i);
        if (v < 0.0f) return;               // read failed — skip this round
        cur[i] = (int)(v + 0.5f);
    }
    if (!g_special_init) {
        for (int i = 0; i < 7; ++i) g_special[i] = cur[i];
        g_special_init = true;
        return;
    }
    for (int i = 0; i < 7; ++i) {
        if (cur[i] == g_special[i]) continue;
        g_special[i] = cur[i];
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s %d", g_special_names[i], cur[i]);
        tolk::Speak(buf, tolk::Priority::Ui, true);
    }
}

// First/third-person announce — the SkyrimAccessMod technique: announce
// IMMEDIATELY on the F press by PREDICTING the toggle (read the current POV, say
// the opposite), instead of reading the flag afterwards. The camera switch is a
// smooth ZOOM, so the real flag (bThirdPerson) settles ~350 ms late and blips
// during the transition — reading it after the press gave delayed/doubled/
// inverted announces (their comment notes the same race). Predicting on the
// press is instant and correct every time, because F always flips the POV.
//
// A background POLL still runs as a silent self-correction: if a prediction ever
// disagrees with the settled flag (a press that didn't toggle, or a POV change
// from console/script/another mod), it announces the true settled value.
std::atomic<int>   g_pov_announced{ -1 };  // last announced (shared: F press + poll)
std::atomic<DWORD> g_pov_grace{ 0 };       // poll won't correct before this tick
int   g_pov_pending = -1;                  // poll-side candidate awaiting stability
DWORD g_pov_since    = 0;                   // tick (ms) the candidate first appeared
constexpr DWORD kPovSettleMs = 350;         // > the ~100-150 ms zoom blip

void AnnouncePov(int cur)
{
    g_pov_announced.store(cur);
    tolk::Speak(cur ? "Trzecia osoba" : "Pierwsza osoba",
                tolk::Priority::Ui, true);
}

void PollViewChange()
{
    // Only in free gameplay (HUD up): gate out VATS / Pip-Boy / dialogue cinematic
    // cameras. When not eligible, drop the in-flight candidate but KEEP the last
    // announced value so a real change across a menu still corrects on return.
    menu::Id m = menu::ActiveMenu();
    bool hud = (m == menu::Id::None || m == menu::Id::HUDMain);
    if (!IsGameplayActive() || g_postload_cooldown > 0 || !hud) {
        g_pov_pending = -1;
        return;
    }
    int cur = game::IsThirdPerson() ? 1 : 0;
    DWORD now = GetTickCount();

    // Grace window after an F press: bThirdPerson lags the toggle by the camera
    // zoom, so don't "correct" toward the stale value while it catches up. Keep
    // the baseline fresh so the debounce starts clean when grace ends.
    if (now < g_pov_grace.load()) { g_pov_pending = cur; g_pov_since = now; return; }

    // Debounced self-correction for changes NOT from our F key (console / script /
    // mod / auto camera), or a rare wrong prediction. Seeds silently; otherwise
    // announces only when the settled flag disagrees with what we last said.
    if (cur != g_pov_pending) { g_pov_pending = cur; g_pov_since = now; return; }
    if (now - g_pov_since < kPovSettleMs) return;
    int ann = g_pov_announced.load();
    if (ann == -1) { g_pov_announced.store(cur); return; }   // seed, no announce
    if (cur != ann) AnnouncePov(cur);
}

// HP/AP/radiation readout, requested by the H hotkey (worker thread sets the
// flag; we read the actor values here, on the main thread).
std::atomic<bool> g_status_pending{ false };

// Menu-back / close (Backspace). Calls Menu::HandleClick (a game function that
// opens/closes menus), so it must run on the main thread too.
std::atomic<bool> g_menuback_pending{ false };

// Restore-default-controls (R, in the settings/controls page). Also a
// Menu::HandleClick game call → main thread only.
std::atomic<bool> g_restore_pending{ false };

// Skip the current objective by advancing the tracked quest's stage (calls
// TESQuest::SetStage → runs scripts → main thread only).
std::atomic<bool> g_skip_pending{ false };

// VATS button click (Menu::HandleClick game call → main thread only).
// 0 = none, 1 = body part, 2 = previous target, 3 = next target.
std::atomic<int> g_vats_click{ 0 };

// Native SetAngle aim (the friend's lead): point the player's view via the
// engine's own SetAngle (0x522B50 → touches the 3D node → main thread only).
// Yaw and pitch are optional and carried as float bit patterns through atomics.
std::atomic<bool> g_aim_pending{ false };
std::atomic<bool> g_aim_do_yaw{ false };
std::atomic<bool> g_aim_do_pitch{ false };
std::atomic<int>  g_aim_yaw_bits{ 0 };
std::atomic<int>  g_aim_pitch_bits{ 0 };
// Frames left to keep re-applying the requested angle (main thread only).
constexpr int kAimFrames = 5;
int g_aim_frames = 0;
// Native "face this point" request (the engine's own actor-facing routine).
std::atomic<bool> g_face_pending{ false };
std::atomic<int>  g_face_x{ 0 }, g_face_y{ 0 }, g_face_z{ 0 };

float BitsToF(int bits) { float f; std::memcpy(&f, &bits, 4); return f; }
int   FToBits(float f)  { int b;  std::memcpy(&b, &f, 4);  return b; }

void AnnounceStatus()
{
    if (!g_status_pending.exchange(false)) return;
    if (!IsGameplayActive()) return;
    int hp  = (int)(game::GetPlayerAV(16) + 0.5f);   // Health (current)
    int ap  = (int)(game::GetPlayerAV(12) + 0.5f);   // ActionPoints
    int rad = (int)(game::GetPlayerAV(54) + 0.5f);   // RadLevel
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "Zdrowie %d, akcja %d, promieniowanie %d", hp, ap, rad);
    tolk::Speak(buf, tolk::Priority::Ui, true);
}

// ---- Full mouse-free aim (the friend's plan) -------------------------------
//
// A MAIN-THREAD state machine: point the view at the target's centre via the
// native SetAngle, then VERIFY with GetCrosshairRef and micro-scan a small
// yaw/pitch grid until the crosshair ref == the target (handles the origin-vs-
// hitbox offset). Announces "na celu" when locked, or "na oko" if the engine's
// crosshair pick can't reach it (long range) — then the player fires with V.
//
// Worker (hotkey) writes the target then raises g_naim_start; we own the rest.
game::Vec3 g_naim_pos{};
uint32_t   g_naim_refid = 0;
std::atomic<bool> g_naim_start{ false };

int   g_naim_phase = 0;     // 0 idle, 1 verify/micro-scan
int   g_naim_step  = 0;     // current micro-scan grid index
int   g_naim_wait  = 0;     // frames to let the camera + crosshair pick settle
float g_naim_yaw0  = 0.0f;  // base yaw (deg, 0 = north)
float g_naim_pitch0 = 0.0f; // base SetAngle-X pitch (deg; negative = up)
constexpr int kAimSettle = 2;

// (dyaw, dpitch) probes in degrees, nearest-first. Pitch gets more range —
// target height / origin varies more than azimuth. SetAngle-X units.
const float kAimGrid[][2] = {
    {0,0}, {0,-2},{0,2}, {1.5f,0},{-1.5f,0}, {0,-4},{0,4},
    {1.5f,-2},{-1.5f,-2},{1.5f,2},{-1.5f,2},
    {0,-6},{0,6}, {3,0},{-3,0}, {0,-8},{0,8},
};
constexpr int kAimSteps = (int)(sizeof(kAimGrid) / sizeof(kAimGrid[0]));

void PointAt(float dyaw, float dpitch)
{
    game::SetPlayerAngleDeg('Z', g_naim_yaw0 + dyaw);
    game::SetPlayerAngleDeg('X', g_naim_pitch0 + dpitch);
}

void TickNativeAim()
{
    if (g_naim_start.exchange(false)) {
        if (!IsGameplayActive()) { g_naim_phase = 0; return; }
        auto pp = game::GetPlayerPosition();
        float dx = g_naim_pos.x - pp.x, dy = g_naim_pos.y - pp.y;
        float horiz = std::sqrt(dx * dx + dy * dy);
        const float R2D = 57.2957795f;
        g_naim_yaw0   = std::atan2(dx, dy) * R2D;        // 0 = north(+Y)
        float elev    = std::atan2(g_naim_pos.z - (pp.z + 100.0f),
                                   horiz > 1.0f ? horiz : 1.0f) * R2D;  // +=above eye
        g_naim_pitch0 = -elev;                           // SetAngle X inverted
        g_naim_phase  = 1;
        g_naim_step   = 0;
        PointAt(kAimGrid[0][0], kAimGrid[0][1]);
        g_naim_wait   = kAimSettle;
        return;
    }
    if (g_naim_phase == 0) return;
    if (!IsGameplayActive()) { g_naim_phase = 0; return; }
    if (g_naim_wait > 0) { --g_naim_wait; return; }

    // Did the last probe land the crosshair on the target?
    uint32_t cur = game::GetCrosshairRefID();
    if (g_naim_refid && cur == g_naim_refid) {
        tolk::Speak("Na celu, strzelaj.", tolk::Priority::Ui, true);
        g_naim_phase = 0;
        return;
    }
    if (g_naim_step + 1 >= kAimSteps) {
        // Grid exhausted — the engine's crosshair pick never returned the target
        // (typically out of its short range). Re-centre on the geometric aim and
        // trust it; the player fires with the LOS cue.
        PointAt(0.0f, 0.0f);
        tolk::Speak("Celuję na oko, strzelaj.", tolk::Priority::Ui, true);
        g_naim_phase = 0;
        return;
    }
    ++g_naim_step;
    PointAt(kAimGrid[g_naim_step][0], kAimGrid[g_naim_step][1]);
    g_naim_wait = kAimSettle;
}

// Teleport (Alt+Home): MoveTo the player to a target reference. The native call
// touches cells/collision → main thread only. Worker writes the ref then raises
// the flag.
const void*       g_teleport_refr = nullptr;
std::atomic<bool> g_teleport_pending{ false };
// Press the map's "travel to" button (a HandleClick → main thread only).
std::atomic<bool> g_map_travel_pending{ false };
// The quantity prompt's state, sampled on the MAIN thread. Reading that menu
// from the poll thread crashed the game: the player's own key press closes it on
// the main thread, and our reader was walking its tiles as they were being freed
// (the log ended right after the menu opened, with no click of ours involved).
std::atomic<bool> g_qty_open{ false };
std::atomic<int>  g_qty_amount{ 0 };
std::atomic<int>  g_qty_max{ 0 };

// Press the inventory's "Upuść" button (HandleClick → main thread only).
std::atomic<bool> g_drop_pending{ false };
// Track the quest highlighted in the Pip-Boy (clicks its row; main thread only).
std::atomic<bool> g_track_quest_pending{ false };
// Confirm the quantity prompt (presses its Ok; main thread only).
// Generic "click this named tile in this menu" request (HandleClick → main
// thread). One slot is enough: requests come from key presses, one at a time.
std::atomic<bool> g_menuclick_pending{ false };
std::atomic<uint32_t> g_menuclick_menu{ 0 };
// Fixed buffer, not std::string: the poll thread fills it and the main thread
// reads it, and a reallocating string would hand the reader a freed pointer.
char              g_menuclick_tile[64] = {};
// Select a world-map marker by location name (HandleClick → main thread). The
// name is written before the flag is raised and only read once, on the frame the
// flag is consumed.
const void*       g_map_marker_tile = nullptr;
std::atomic<bool> g_map_marker_pending{ false };

// Runs every frame on the MAIN thread (via the DispatchMessageA hook), so it's
// safe to call game functions like GetActorValue from here.
// Structured-exception guard around the menu readers. See the call site for why
// this is needed. Deliberately contains no objects requiring unwinding, which
// __try forbids.
// These walk the live menu tile tree. They run on the POLL thread, which is safe
// for every menu the game keeps around while the player navigates it — but NOT
// for a prompt the player dismisses with a key, because the main thread frees it
// mid-read. The quantity prompt is exactly that case, so it is skipped here and
// sampled on the main thread instead (see MainThreadWork / poll::QuantityState).
//
// An SEH guard was tried here and made things worse: __except does not unwind
// C++ objects, so an exception taken while the logger held its mutex deadlocked
// every thread that logs.
void PollMenuReaders()
{
    if (menu::ActiveMenu() == menu::Id::Quantity) return;
    PollKeyboardSelection();
    PollTerminal();
    PollRaceSex();
    PollHudMessages();
    PollActivatePrompt();
}

void MainThreadWork()
{
    PollSpecialChange();
    PollViewChange();
    TickNativeAim();
    if (g_teleport_pending.exchange(false))
        game::TeleportPlayerToRef(g_teleport_refr);
    AnnounceStatus();
    if (g_menuback_pending.exchange(false)) game::ClickMenuBack();
    if (g_restore_pending.exchange(false)) {
        bool ok = game::ClickRestoreDefaults();
        tolk::Speak(ok ? "Przywracam domyślne sterowanie. Potwierdź wybór."
                       : "Otwórz ustawienia sterowania, potem naciśnij R.",
                    tolk::Priority::System, true);
    }
    if (g_skip_pending.exchange(false)) {
        bool ok = game::AdvanceTrackedQuestStage();
        tolk::Speak(ok ? "Przeskoczono etap zadania. Sprawdź nowy cel."
                       : "Nie udało się przeskoczyć etapu — brak aktywnego zadania.",
                    tolk::Priority::System, true);
    }
    // Sample the quantity prompt while we are on the thread that owns it — but
    // ONLY when it is actually up. Every other block here sits behind a request
    // flag and does nothing per frame; this one ran unconditionally from the very
    // first frame, walking the menu tree during the loading screen before the
    // interface exists, and that crashed the game on startup. The menu id comes
    // from our own dispatcher state, so testing it costs nothing.
    if (g_system_ready && menu::ActiveMenu() == menu::Id::Quantity) {
        int a = 0, m = 0;
        bool ok = game::GetQuantityState(&a, &m);
        if (ok) { g_qty_amount.store(a); g_qty_max.store(m); }
        g_qty_open.store(ok);
    } else if (g_qty_open.load()) {
        g_qty_open.store(false);
    }
    if (g_menuclick_pending.exchange(false)) {
        game::ClickMenuButton(g_menuclick_menu.load(), g_menuclick_tile, 8);
    }
    if (g_track_quest_pending.exchange(false)) {
        if (game::ClickSelectedRowIn("MM_QuestsList"))
            tolk::Speak("Śledzę to zadanie.", tolk::Priority::Ui, true);
        else
            tolk::Speak("Najpierw wybierz zadanie na liście.",
                        tolk::Priority::System, true);
    }
    if (g_drop_pending.exchange(false)) {
        // The inventory menu has a real Drop button; clicking it through the
        // engine gives the vanilla behaviour, including the quantity prompt for
        // a stack. No item juggling on our side.
        if (!game::ClickMenuButton(kMenuType_Inventory, "IM_DropButton", 8))
            tolk::Speak("Otwórz ekwipunek i wybierz przedmiot, potem Delete.",
                        tolk::Priority::System, true);
    }
    if (g_map_marker_pending.exchange(false)) {
        if (!game::ClickMapMarkerTile(g_map_marker_tile))
            tolk::Speak("Nie znalazłem tej lokacji na mapie świata.",
                        tolk::Priority::System, true);
    }
    if (g_map_travel_pending.exchange(false)) {
        bool ok = game::ClickMapTravelButton();
        if (!ok) tolk::Speak("Nie znalazłem przycisku podróży na mapie.",
                             tolk::Priority::System, true);
    }
    if (int c = g_vats_click.exchange(0)) {
        const char* btn = (c == 2) ? "left_arrow"
                        : (c == 3) ? "right_arrow"
                                   : "BodyPart_button";
        game::ClickVatsButton(btn);   // the VATS reader announces the new pick
    }
    // Native facing runs before the angle writes below, so a follow-up pitch
    // adjustment lands on top of it rather than being undone by it.
    if (g_face_pending.exchange(false)) {
        game::Vec3 p{ BitsToF(g_face_x.load()), BitsToF(g_face_y.load()),
                      BitsToF(g_face_z.load()) };
        game::FacePointNative(p);
    }
    // Aim: re-apply the angle for several consecutive frames. A single-frame
    // SetAngle gets overwritten by the game's own input processing before it
    // reaches the camera — the FNV accessibility mod re-applies its look-at over
    // 5 frames for exactly this reason, and pitch is the axis that suffered.
    if (g_aim_pending.exchange(false)) g_aim_frames = kAimFrames;
    if (g_aim_frames > 0) {
        --g_aim_frames;
        // Yaw first (heading), then pitch — both via the engine's SetAngle.
        if (g_aim_do_yaw.load())
            game::SetPlayerAngleDeg('Z', BitsToF(g_aim_yaw_bits.load()));
        if (g_aim_do_pitch.load())
            game::SetPlayerAngleDeg('X', BitsToF(g_aim_pitch_bits.load()));
    }
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
                // Take over the player's movement update now that the engine is
                // up. Verified against the expected function, so a mismatch just
                // leaves the key-based walker in place.
                if (config::Get().native_walk) mover::Install();
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
            // Loading just closed → arm post-load cooldown AND wipe per-session
            // cache (a new game / loaded save must not report the previous
            // session's objects), and re-seed the POV announce silently.
            if (t == kMenuType_Loading) {
                g_postload_cooldown = kPostLoadCooldownTicks;
                modules::worldscan::ResetSession();
                g_pov_announced.store(-1);
                // Loading closed with no player yet → the new-game opening
                // cinematic (a loaded save spawns the player at once, and the
                // intro AD auto-stops when it does). Kick off the description.
                if (!game::IsPlayerValid()) modules::intro::Start();
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

    // While the quantity prompt is up, NOTHING on this thread may walk the menu
    // tree. That prompt is dismissed with a key press, so the main thread frees
    // it at an arbitrary moment — and any iteration over menuRoot's children,
    // whatever it is looking for, can be inside the list when that happens. This
    // is what crashed the game on A and on E, repeatedly.
    //
    // Everything the player needs from that prompt (the chosen amount) is instead
    // sampled on the main thread in MainThreadWork and read back through
    // poll::QuantityState, so the narration keeps working while nothing here
    // touches the live menu.
    const bool quantity_up = menu::ActiveMenu() == menu::Id::Quantity;

    if (!quantity_up) {
        menu::OnTick(dt);
        PollActiveTile();
        PollMenuReaders();
    }

    if (config::IsEnabled()) {
        hotkeys::Poll();
        modules::autowalk::Tick(dt);
        modules::guide::Tick(dt);
        modules::intro::Tick(dt);
        if (!quantity_up) {
            // These search the menu tree (lockpick cue, hacking grid, map list).
            modules::worldscan::Tick(dt);
            modules::hacking::Tick(dt);
            modules::mapnav::Tick(dt);
        }
        modules::quantity::Tick(dt);   // sampled state only — safe
        PollQuestChange();
        // NOTE: SPECIAL/status read game functions (GetActorValue) and so MUST
        // run on the main thread — see MainThreadWork(), invoked by the
        // DispatchMessageA hook, NOT here on the worker thread.
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
    // Run SPECIAL/status reads (which call game functions) on the main thread.
    game::SetMainThreadCallback(&MainThreadWork);
    g_thread = std::thread(&ThreadProc);
}

void Stop()
{
    if (!g_running.exchange(false)) return;
    game::SetMainThreadCallback(nullptr);
    if (g_thread.joinable()) g_thread.join();
    // Put the engine's own movement update back before we unload.
    mover::Shutdown();
}

// Called from any thread (the H hotkey); the actual actor-value reads happen on
// the main thread in AnnounceStatus().
void RequestStatus() { g_status_pending.store(true); }

// Called from any thread (Backspace); ClickMenuBack runs on the main thread.
void RequestMenuBack() { g_menuback_pending.store(true); }

// Called from any thread (R); ClickRestoreDefaults runs on the main thread.
void RequestRestoreDefaults() { g_restore_pending.store(true); }

// Called from any thread (=); AdvanceTrackedQuestStage runs on the main thread.
void RequestSkipObjective() { g_skip_pending.store(true); }

// Called from any thread; the VATS HandleClick runs on the main thread.
// code: 1 = body part, 2 = previous target, 3 = next target.
void RequestVatsClick(int code) { g_vats_click.store(code); }
void RequestVatsBodyPart() { RequestVatsClick(1); }

// Called from the F hotkey (worker thread). Announces the view IMMEDIATELY by
// predicting the toggle — F flips the POV, so we read the current flag and say
// the opposite at once (no wait for the camera zoom). The poll silently corrects
// the rare wrong prediction. Reads are cross-thread-safe; tolk::Speak is too.
void RequestViewAnnounce()
{
    if (!IsGameplayActive() || g_postload_cooldown > 0) return;
    menu::Id m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) return;   // F ≠ view in menus
    // Predict from our OWN last-announced state, not the live flag: bThirdPerson
    // lags the toggle by the camera zoom, so reading it here (especially on rapid
    // presses) returns the stale value and mis-predicts. F always flips the POV,
    // so flipping our tracked value is correct and instant.
    int last = g_pov_announced.load();
    int predicted = (last == -1) ? (game::IsThirdPerson() ? 0 : 1)   // no baseline yet
                                 : (last ? 0 : 1);                    // flip
    AnnouncePov(predicted);
    g_pov_grace.store(GetTickCount() + 800);   // let the flag catch up before poll corrects
}

// Called from any thread (the aim hotkey): start the full mouse-free aim at a
// target (world point + its ref id for crosshair verification). The state
// machine in TickNativeAim (main thread) points, verifies and micro-scans.
void RequestNativeAim(const game::Vec3& pos, uint32_t refid)
{
    g_naim_pos   = pos;
    g_naim_refid = refid;
    g_naim_start.store(true);
}

// Called from any thread (Alt+Home): teleport the player to a reference. The
// native MoveTo runs on the main thread (cells/collision).
void RequestTeleport(const void* refr)
{
    g_teleport_refr = refr;
    g_teleport_pending.store(true);
}

void RequestFacePoint(const game::Vec3& p)
{
    g_face_x.store(FToBits(p.x));
    g_face_y.store(FToBits(p.y));
    g_face_z.store(FToBits(p.z));
    g_face_pending.store(true);
}

void RequestMapTravel() { g_map_travel_pending.store(true); }

void RequestDropItem() { g_drop_pending.store(true); }

bool QuantityState(int* amount, int* maximum)
{
    if (!g_qty_open.load()) return false;
    if (amount)  *amount  = g_qty_amount.load();
    if (maximum) *maximum = g_qty_max.load();
    return true;
}

void RequestTrackQuest() { g_track_quest_pending.store(true); }


void RequestMenuClick(uint32_t menuType, const std::string& tileName)
{
    g_menuclick_menu.store(menuType);
    std::strncpy(g_menuclick_tile, tileName.c_str(), sizeof(g_menuclick_tile) - 1);
    g_menuclick_tile[sizeof(g_menuclick_tile) - 1] = 0;
    g_menuclick_pending.store(true);   // raised last: the name is already there
}

void RequestMapMarkerClick(const void* markerTile)
{
    g_map_marker_tile = markerTile;
    g_map_marker_pending.store(true);
}


// Called from any thread; the native SetAngle runs on the main thread. Pass
// degrees (yaw 0 = north; pitch via SetAngle X, NEGATIVE = look up). Set only
// the axes you want to change.
void RequestAim(bool doYaw, float yawDeg, bool doPitch, float pitchDeg)
{
    if (doYaw)   g_aim_yaw_bits.store(FToBits(yawDeg));
    if (doPitch) g_aim_pitch_bits.store(FToBits(pitchDeg));
    g_aim_do_yaw.store(doYaw);
    g_aim_do_pitch.store(doPitch);
    g_aim_pending.store(true);
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

// --- Navmesh dumper (read-only RE aid) -------------------------------------
//
// FOSE doesn't define FO3's NavMesh structure (only TESObjectCELL::NavMeshArray
// @0x60 = BSSimpleArray<NavMesh*>, and a forward-declared `class NavMesh;`).
// To build navmesh A* pathfinding we first need the FO3 layout, so this dumps
// the array region and the first NavMesh's leading bytes — annotated as
// int/float/pointer — so the vertex & triangle sub-arrays can be identified.
// Everything is guarded by IsBadReadPtr; it never writes game memory.
namespace {

bool MemReadable(const void* p, size_t n)
{
    return p && !IsBadReadPtr(p, n);
}

void DumpDwords(const char* tag, const UInt8* base, int from, int to)
{
    for (int off = from; off < to; off += 4) {
        if (!MemReadable(base + off, 4)) {
            log::DumpWrite("  %s+0x%03X = <unreadable>", tag, off);
            continue;
        }
        UInt32 v = *reinterpret_cast<const UInt32*>(base + off);
        float  f = *reinterpret_cast<const float*>(&v);
        bool   isptr = MemReadable(reinterpret_cast<void*>(v), 4);
        log::DumpWrite("  %s+0x%03X = 0x%08X  int=%-9d f=%11.3f%s",
                       tag, off, v, (int)v, f, isptr ? "  <ptr>" : "");
    }
}

void DumpNavmesh()
{
    log::DumpWrite("===== Navmesh dump =====");
    auto* player = fose_rt::Player();
    if (!player) { log::DumpWrite("navmesh: no player"); return; }
    auto* cell = reinterpret_cast<UInt8*>(player->parentCell);
    if (!MemReadable(cell, 0x90)) { log::DumpWrite("navmesh: no/bad cell"); return; }
    log::DumpWrite("cell=%p", cell);

    DumpDwords("cell", cell, 0x5C, 0x88);

    // Interior vs exterior: worldSpace @ +0xC0 (null = interior). Exteriors keep
    // navmesh in the worldspace (NavMeshInfoMap), NOT at cell+0x60 — so when the
    // cell holder is null this reveals where to look next.
    UInt8* ws = MemReadable(cell + 0xC0, 4)
                    ? *reinterpret_cast<UInt8**>(cell + 0xC0) : nullptr;
    log::DumpWrite("worldSpace=%p  interior=%d", (void*)ws, ws ? 0 : 1);
    if (MemReadable(ws, 0x140)) {
        log::DumpWrite("worldSpace region (find NavMeshInfoMap / navmesh ptr):");
        DumpDwords("ws", ws, 0x00, 0x140);
    }

    // cell+0x60 is a POINTER to the navmesh-array holder object (confirmed by
    // a first dump: only +0x60 held a pointer, rest zero). Follow it.
    if (!MemReadable(cell + 0x60, 4)) { log::DumpWrite("navmesh: +0x60 bad"); return; }
    UInt8* holder = *reinterpret_cast<UInt8**>(cell + 0x60);
    if (!MemReadable(holder, 0x40)) {
        log::DumpWrite("navmesh: holder %p unreadable", (void*)holder);
        return;
    }
    log::DumpWrite("holder=%p — first 0x40 bytes (find {data ptr, size, alloc}):",
                   (void*)holder);
    DumpDwords("h", holder, 0x00, 0x40);

    // Try a few plausible {data, size} interpretations of the holder and, for
    // each that looks like an array of readable pointers, follow entry [0] and
    // dump the NavMesh leading bytes.
    static const int kDataOffsets[] = { 0x00, 0x04, 0x08 };
    for (int doff : kDataOffsets) {
        if (!MemReadable(holder + doff, 8)) continue;
        void** data = *reinterpret_cast<void***>(holder + doff);
        UInt32 size = *reinterpret_cast<UInt32*>(holder + doff + 4);
        if (!data || size == 0 || size > 64 || !MemReadable(data, sizeof(void*)))
            continue;
        void* nav0 = data[0];
        if (!MemReadable(nav0, 0x160)) continue;
        log::DumpWrite("holder+0x%X looks like data=%p size=%u; "
                       "navmesh[0]=%p leading 0x150 bytes:",
                       doff, (void*)data, size, nav0);
        DumpDwords("nm", reinterpret_cast<UInt8*>(nav0), 0x00, 0x150);

        // Scan for embedded BSSimpleArrays { exe-vtbl, heap-data, size, cap }
        // and dump the first two's CONTENTS — #1 as float triples (vertices),
        // #2 as uint16 octets (triangles: v0 v1 v2 | n0 n1 n2 | flags...).
        UInt8* nm = reinterpret_cast<UInt8*>(nav0);
        int found = 0;
        for (int off = 0x18; off <= 0x60 && found < 3; off += 4) {
            if (!MemReadable(nm + off, 16)) continue;
            UInt32 vtbl = *reinterpret_cast<UInt32*>(nm + off);
            UInt32 dptr = *reinterpret_cast<UInt32*>(nm + off + 4);
            UInt32 sz   = *reinterpret_cast<UInt32*>(nm + off + 8);
            UInt32 cap  = *reinterpret_cast<UInt32*>(nm + off + 12);
            bool vtblOk = vtbl >= 0x00400000 && vtbl < 0x01800000;
            bool dataOk = dptr >= 0x01800000 &&
                          MemReadable(reinterpret_cast<void*>(dptr), 16);
            bool szOk   = sz >= 1 && sz < 100000 && cap >= sz && cap <= sz + 256;
            if (!(vtblOk && dataOk && szOk)) continue;
            ++found;
            UInt8* d = reinterpret_cast<UInt8*>(dptr);
            log::DumpWrite("  array@nm+0x%X: data=0x%08X size=%u cap=%u",
                           off, dptr, sz, cap);
            int n = sz < 6 ? (int)sz : 6;
            if (found == 1) {            // assume vertices (NiPoint3, 12 bytes)
                for (int i = 0; i < n; ++i) {
                    float* v = reinterpret_cast<float*>(d + i * 12);
                    if (!MemReadable(v, 12)) break;
                    log::DumpWrite("    vert[%d] = %.2f, %.2f, %.2f",
                                   i, v[0], v[1], v[2]);
                }
            } else if (found == 2) {     // assume triangles (16 bytes = 8 u16)
                for (int i = 0; i < n; ++i) {
                    UInt16* t = reinterpret_cast<UInt16*>(d + i * 16);
                    if (!MemReadable(t, 16)) break;
                    log::DumpWrite("    tri[%d] = %u %u %u | %u %u %u | "
                                   "0x%04X 0x%04X", i,
                                   t[0], t[1], t[2], t[3], t[4], t[5],
                                   t[6], t[7]);
                }
            }
        }
        return;
    }
    log::DumpWrite("navmesh: could not locate array data inside holder "
                   "— read holder bytes above");
}

// Find the real first/third-person flag for this runtime: snapshot a byte range
// of PlayerCharacter on the first F11, then on the next F11 (after toggling view
// with F) log every BOOL-like byte that flipped 0<->1. The flipping offset is
// bThirdPerson (the FOSE SDK's 0x5A8 is wrong for GOTY 1.7.0.3). Stand still
// between the two dumps so unrelated fields don't add noise.
void DumpViewOffsets()
{
    log::DumpWrite("===== View-flag finder =====");
    auto* player = fose_rt::Player();
    if (!player) { log::DumpWrite("no player"); return; }
    UInt8* base = reinterpret_cast<UInt8*>(player);
    // Scan a wide dword range; log every dword that CHANGED between the two
    // dumps, as int and float, so a camera-distance float or an enum/bool POV
    // field both show up. Stand still so few unrelated fields move.
    const int from = 0x180, to = 0x9C0;
    static UInt32 snap[(0x9C0 - 0x180) / 4];
    static bool have = false;
    int n = (to - from) / 4;
    if (!have) {
        for (int k = 0; k < n; ++k) {
            UInt8* a = base + from + k * 4;
            snap[k] = MemReadable(a, 4) ? *reinterpret_cast<UInt32*>(a) : 0;
        }
        have = true;
        log::DumpWrite("baseline captured 0x%X..0x%X — toggle view (F), F11 again",
                       from, to);
        return;
    }
    int diffs = 0;
    for (int k = 0; k < n; ++k) {
        UInt8* a = base + from + k * 4;
        if (!MemReadable(a, 4)) continue;
        UInt32 now = *reinterpret_cast<UInt32*>(a), was = snap[k];
        if (now != was) {
            float fw = *reinterpret_cast<float*>(&was);
            float fn = *reinterpret_cast<float*>(&now);
            log::DumpWrite("  +0x%03X: 0x%08X(%d, %.2f) -> 0x%08X(%d, %.2f)",
                           from + k * 4, was, (int)was, fw, now, (int)now, fn);
            ++diffs;
        }
        snap[k] = now;
    }
    log::DumpWrite("  %d changed dwords", diffs);
}

// Does a pointer look like a TESObjectREFR? baseForm ptr @0x1C, world pos
// floats @0x2C..0x34 (plausible coordinate magnitudes).
bool LooksLikeRefr(const UInt8* p)
{
    if (!MemReadable(p, 0x40)) return false;
    UInt32 baseForm = *reinterpret_cast<const UInt32*>(p + 0x1C);
    if (baseForm < 0x00400000) return false;  // not a real form pointer
    float x = *reinterpret_cast<const float*>(p + 0x2C);
    float y = *reinterpret_cast<const float*>(p + 0x30);
    float z = *reinterpret_cast<const float*>(p + 0x34);
    auto ok = [](float f){ return f > -1e7f && f < 1e7f; };
    return ok(x) && ok(y) && ok(z);
}

// Dump the player's current quest objective and chase its pointers one or two
// levels, looking for the target reference (whose position the compass arrow
// points to) so we can read a bearing to it.
void DumpQuestObjective()
{
    log::DumpWrite("===== Quest objective dump =====");
    auto* player = fose_rt::Player();
    if (!player) { log::DumpWrite("no player"); return; }
    UInt8* obj = *reinterpret_cast<UInt8**>(
        reinterpret_cast<UInt8*>(player) + 0x618);   // questObjective
    if (!MemReadable(obj, 0x40)) { log::DumpWrite("no/bad questObjective"); return; }
    log::DumpWrite("questObjective=%p", (void*)obj);

    char* txt = *reinterpret_cast<char**>(obj + 0x08);   // displayText.m_data
    if (MemReadable(txt, 1)) log::DumpWrite("  displayText='%.100s'", txt);

    DumpDwords("qo", obj, 0x00, 0x40);

    for (int off = 0x10; off < 0x40; off += 4) {
        if (!MemReadable(obj + off, 4)) continue;
        UInt32 v = *reinterpret_cast<UInt32*>(obj + off);
        if (v < 0x01000000 || !MemReadable(reinterpret_cast<void*>(v), 0x40))
            continue;
        UInt8* pe = reinterpret_cast<UInt8*>(v);
        if (LooksLikeRefr(pe)) {
            float x = *reinterpret_cast<float*>(pe + 0x2C);
            float y = *reinterpret_cast<float*>(pe + 0x30);
            float z = *reinterpret_cast<float*>(pe + 0x34);
            log::DumpWrite("  qo+0x%X -> REFR %p pos=(%.0f,%.0f,%.0f)",
                           off, (void*)pe, x, y, z);
            continue;
        }
        log::DumpWrite("  qo+0x%X -> %p, first 0x30 bytes:", off, (void*)pe);
        DumpDwords("  >", pe, 0x00, 0x30);
        // Second level: treat the pointee as an array of pointers and test
        // each for a REFR (the targets array → target ref).
        for (int k = 0; k < 16; k += 4) {
            if (!MemReadable(pe + k, 4)) break;
            UInt32 w = *reinterpret_cast<UInt32*>(pe + k);
            if (w < 0x01000000 || !MemReadable(reinterpret_cast<void*>(w), 0x40))
                continue;
            UInt8* pe2 = reinterpret_cast<UInt8*>(w);
            if (LooksLikeRefr(pe2)) {
                float x = *reinterpret_cast<float*>(pe2 + 0x2C);
                float y = *reinterpret_cast<float*>(pe2 + 0x30);
                float z = *reinterpret_cast<float*>(pe2 + 0x34);
                log::DumpWrite("    [%d] -> REFR %p pos=(%.0f,%.0f,%.0f)",
                               k, (void*)pe2, x, y, z);
            }
        }
    }
}

// Dump every 'running' quest with a name, its running byte, and each
// objective's status word + whether it carries a live marker. Lets us pin the
// exact status offset/bits that distinguish journal-active quests from the
// hundreds of finished/background quests the engine keeps 'running'.
void DumpQuestList()
{
    log::DumpWrite("===== Quest list dump =====");
    auto* gp = reinterpret_cast<UInt8**>(fose_rt::g_addrs->dataHandler);
    if (!MemReadable(gp, 4)) { log::DumpWrite("no dataHandler ptr"); return; }
    UInt8* dh = *gp;
    if (!MemReadable(dh, 0xD8)) { log::DumpWrite("bad dataHandler"); return; }

    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);   // questList @ +0xD4
    int total = 0, named = 0;
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (!MemReadable(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || !MemReadable(q, 0x54)) continue;
        ++total;

        UInt8 running = *reinterpret_cast<UInt8*>(q + 0x3C);
        const char* nm = *reinterpret_cast<char**>(q + 0x34);
        bool hasName = MemReadable(nm, 1) && *nm;
        if (!hasName) continue;       // only the ones that reach the scanner
        ++named;
        log::DumpWrite("Quest '%.60s'  running=%u", nm, running);

        Node* on = reinterpret_cast<Node*>(q + 0x4C);   // objectives @ +0x4C
        for (int s2 = 0; on && s2 < 64; ++s2) {
            if (!MemReadable(on, 8)) break;
            UInt8* obj = on->item;
            on = on->next;
            if (!obj || !MemReadable(obj, 0x24)) continue;
            UInt32 st14 = *reinterpret_cast<UInt32*>(obj + 0x14);
            UInt32 st18 = *reinterpret_cast<UInt32*>(obj + 0x18);
            UInt32 st1C = *reinterpret_cast<UInt32*>(obj + 0x1C);
            UInt32 st20 = *reinterpret_cast<UInt32*>(obj + 0x20);
            char* txt = *reinterpret_cast<char**>(obj + 0x08);
            bool marker = false;
            // +0x14 -> Target -> +0x0C -> REFR (the marker we navigate to)
            if (st14 >= 0x01000000 && MemReadable(reinterpret_cast<void*>(st14), 0x10)) {
                UInt8* tgt = reinterpret_cast<UInt8*>(st14);
                UInt8* refr = *reinterpret_cast<UInt8**>(tgt + 0x0C);
                marker = LooksLikeRefr(refr);
            }
            // +0x20 = runtime journal flags (from Cmd_SetObjectiveDisplayed
            // disasm): bit0 = displayed in Pip-Boy, bit1 = completed.
            log::DumpWrite("    obj +14=0x%X +18=0x%X +1C=0x%X +20=0x%X%s%s "
                           "marker=%d '%.50s'",
                           st14, st18, st1C, st20,
                           (st20 & 1) ? " DISP" : "",
                           (st20 & 2) ? " DONE" : "",
                           (int)marker,
                           MemReadable(txt, 1) ? txt : "");
        }
    }
    log::DumpWrite("quest list: %d total, %d named", total, named);
}

} // namespace

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
        auto term = game::GetTerminalText();
        log::DumpWrite("GetTerminalText() -> '%s'",
                       term ? term->c_str() : "<null>");
        log::DumpWrite("GetHudMessage() -> '%s'",
                       game::GetHudMessage().c_str());
        log::DumpWrite("%s", modules::autowalk::DiagString().c_str());
        // Map markers: verifies the auto-detected marker list AND the inferred
        // MarkerData layout — `flags` is what "discovered" is derived from, so a
        // dump taken with both discovered and undiscovered locations on screen
        // tells us whether the bit guess is right.
        {
            auto mm = game::GetMapMarkers();
            if (!mm.empty()) {
                log::DumpWrite("map markers: %d", (int)mm.size());
                for (size_t i = 0; i < mm.size() && i < 60; ++i)
                    log::DumpWrite("  '%s' flags=0x%04X type=%u disc=%d dist=%.0f",
                                   mm[i].name.c_str(), mm[i].flags, mm[i].type,
                                   (int)mm[i].discovered, mm[i].dist);
            }
        }
        // Hacking: what we read, plus every extra string trait in the menu — the
        // word under the cursor is supposed to sit in the grid's user0.
        auto hk = game::GetHackingInfo();
        if (hk.active) {
            log::DumpWrite("hacking: attempts=%d locked=%d words=%d highlighted='%s'",
                           hk.attempts, (int)hk.locked, (int)hk.words.size(),
                           hk.highlighted.c_str());
            for (const auto& w : hk.words) log::DumpWrite("  word: %s", w.c_str());
            for (const auto& e : game::GetHackingLogEntries())
                log::DumpWrite("  log: %s", e.c_str());
            for (const auto& s : game::DebugHackingTraits())
                log::DumpWrite("  %s", s.c_str());
        }
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
    // In gameplay, also dump the navmesh + the current quest objective so we
    // can decode them (pathfinding / quest-marker bearing).
    if (game::IsPlayerValid()) {
        DumpViewOffsets();
        DumpQuestObjective();
        DumpQuestList();
        log::DumpWrite("===== Nearby refs (radius 2000) =====");
        for (const auto& line : game::DebugNearbyRefs(2000))
            log::DumpWrite("  %s", line.c_str());
        DumpNavmesh();
    }

    log::DumpWrite("===== End dump =====");
    log::EndDump();
    tolk::Speak("Zrzut zapisany w pliku dump.",
                tolk::Priority::System, true);
}

} // namespace f3a::poll
