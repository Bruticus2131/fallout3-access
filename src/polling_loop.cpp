#include "f3a/polling_loop.h"
#include "f3a/player_mover.h"
#include "f3a/fose_runtime.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/audio_beacon.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <cstring>
#include <cstdio>
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
// "Some menu is up" — ANY menu, including ones this mod has no handler for.
//
// menu::ActiveMenu() only knows the menus we gave an internal id to, so a menu
// we do not handle (character creation's attribute page, for one) left it
// reading None — indistinguishable from standing in the world. World hotkeys
// then fired inside that menu, which is how the arrow keys on the attribute
// page started answering "Najpierw wybierz cel klawiszem celowania".
std::atomic<bool> g_menu_mode{ false };

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
    // The container screen is read by its own module, column by column. The
    // game's own arrows move the active tile through YOUR column even while
    // the container's is in use, and reading that tile here is what made the
    // other column's items cut in.
    if (menu::ActiveMenu() == menu::Id::Container) return;
    // The level-up screen too: its module reads the row with its value and
    // description, and this reader would say the row a second time.
    if (menu::ActiveMenu() == menu::Id::LevelUp) return;

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

// Character creation (RaceSexMenu): the generic pollers don't pick it up (no
// activeTile), so it is read specially, as the FNV mod reads it — the page's
// name when the page changes, the focused item when the focus moves ("Kobieta,
// wybrane", "Domyślne: 2", "Dalej"), and just the new number when a slider is
// moved. Sliders used to be skipped entirely: on the face page the focus sits
// on one, so the mod said nothing there at all.
int         g_rsm_page  = -2;
const void* g_rsm_focus = nullptr;
std::string g_rsm_label;
std::string g_rsm_value;
int         g_rsm_throttle = 0;

void PollRaceSex()
{
    if (!g_system_ready) return;
    if (--g_rsm_throttle > 0) return;
    g_rsm_throttle = 2;
    game::RaceSexState st;
    if (!game::GetRaceSexState(&st)) {
        g_rsm_page = -2; g_rsm_focus = nullptr;
        g_rsm_label.clear(); g_rsm_value.clear();
        return;
    }
    bool pageSaid = false;
    if (st.page != g_rsm_page) {
        g_rsm_page  = st.page;
        g_rsm_focus = nullptr;
        if (!st.header.empty()) {
            tolk::Speak(st.header, tolk::Priority::Ui, true);
            pageSaid = true;
        }
    }
    if (st.kind == 0) return;
    if (st.focus != g_rsm_focus || st.label != g_rsm_label) {
        g_rsm_focus = st.focus;
        g_rsm_label = st.label;
        g_rsm_value = st.value;
        std::string say = st.label;
        if (st.kind == 2 && !st.value.empty())
            say = say.empty() ? st.value : say + ": " + st.value;
        if (st.kind == 1 && st.selected) say += strings::T(", wybrane", ", selected");
        if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, !pageSaid);  // after the page name
        return;
    }
    if (st.kind == 2 && st.value != g_rsm_value) {
        g_rsm_value = st.value;
        if (!st.value.empty()) tolk::Speak(st.value, tolk::Priority::Ui, true);
    }
}

// HUD corner notifications (quest updates, items received, XP, discoveries) —
// the passive "background text" a sighted player sees top-left. Read on change
// during free gameplay so it isn't missed.
std::string g_hud_msg;

// The game's control hints — "Aby oddać strzał z broni, <key>.", "Naciśnij
// <key>, aby ..." — come back again and again while you play, and every one
// of them used to be read out every time. They are worth hearing once. A
// hint is recognised by the control placeholder the game leaves in it for a
// keyboard player (&sUActnUse;) or by the way the Polish build phrases them;
// everything else on that line (items added, "you cannot wait with enemies
// near", low health) is a real notice and is always read.
bool LooksLikeHint(const std::string& raw)
{
    size_t amp = raw.find('&');
    if (amp != std::string::npos && raw.find(';', amp) != std::string::npos &&
        raw.find(';', amp) - amp <= 40)
        return true;
    auto starts = [&](const char* p) { return raw.rfind(p, 0) == 0; };
    // Polish build first, then the English one ("Press ... to ...", "Hold
    // ...", "To ..."). Both are matched whatever language the mod speaks: the
    // text is the game's, and so is its language.
    return starts("Aby ") || starts("Naciśnij") || starts("Wciśnij") ||
           raw.find(" naciśnij ") != std::string::npos ||
           raw.find("naciśnij ESC") != std::string::npos ||
           starts("Press ") || starts("Hold ") || starts("To ") ||
           raw.find(" press ") != std::string::npos ||
           raw.find("Press ESC") != std::string::npos;
}

// Hints already read this game session (cleared only when the game restarts).
std::vector<std::string> g_hints_said;

// Should this hint be spoken now? Records it as said when the answer is yes.
bool HintShouldSpeak(const std::string& raw)
{
    int mode = config::Get().hint_mode;
    if (mode >= 2) return true;
    if (mode <= 0) return false;
    for (const auto& h : g_hints_said) if (h == raw) return false;
    if (g_hints_said.size() >= 256) g_hints_said.erase(g_hints_said.begin());
    g_hints_said.push_back(raw);
    return true;
}

void PollHudMessages()
{
    if (!g_system_ready || !IsGameplayActive()) { g_hud_msg.clear(); return; }
    menu::Id m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) return;   // free gameplay only
    std::string msg = game::GetHudMessage();
    if (msg == g_hud_msg) return;
    g_hud_msg = msg;
    if (msg.empty()) return;
    // Log the message VERBATIM. Tutorial hints carry the game's own control
    // placeholders, which stay unreplaced for keyboard players and get read out
    // as gibberish ("su act use"); seeing the raw form is the only way to know
    // what to substitute them with.
    if (LooksLikeHint(msg) && !HintShouldSpeak(msg)) {
        F3A_INFO("HUD msg (hint, already read): '%s'", msg.c_str());
        return;
    }
    F3A_INFO("HUD msg: '%s'", msg.c_str());
    tolk::Speak(game::ExpandControlTokens(msg), tolk::Priority::Ui,
                /*interrupt=*/false);
}

// Read the HUD's prose — tutorials, hints, "you can't do that" notices.
//
// These appear and disappear all over the HUD tree, so rather than hunting for
// each tile we take everything currently visible (game_access filters out the
// counters) and speak the lines we haven't already said.
//
// "Already said" used to mean only "still on screen", and that was not enough.
// A tutorial hint blinks, or its tile is rebuilt, and every flicker counted as
// a new line — so the game repeated "hold aim to..." at the player over and
// over while they were trying to listen for something that mattered. A line is
// now remembered for a good while after it is spoken, so a hint is heard once;
// a notice that genuinely recurs much later is still read again.
std::vector<std::string> g_hud_seen;

struct HudSaid { std::string text; DWORD when; };
std::vector<HudSaid> g_hud_said;

// When the last voice line (a subtitle on the HUD) has probably been spoken:
// audio description waits for it. Polish game voices run ~15 characters a
// second.
std::atomic<DWORD> g_voice_quiet_at{ 0 };

// A save was loaded or a game started (FOSE's LoadGame/NewGame), consumed when
// the Loading menu closes - a door's loading screen has none.
std::atomic<bool> g_game_load_pending{ false };

// The game's thread runs our work (MainThreadWork) only when it dispatches a
// window message, and with the player's hands off nothing is sent: the SPECIAL
// book's numbers, the loot list, who is dead or hostile all stood still. A
// harmless WM_NULL posted to the game window keeps it coming, one at a time.
void PumpGameThread()
{
    static HWND s_hwnd = nullptr;
    static DWORD s_found_at = 0, s_posted_at = 0;
    static uint32_t s_seen = 0;
    DWORD now = GetTickCount();
    if ((!s_hwnd || !IsWindow(s_hwnd)) && now - s_found_at > 2000) {
        s_found_at = now;
        struct Ctx { DWORD pid; HWND best; } ctx{ GetCurrentProcessId(), nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { c->best = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        s_hwnd = ctx.best;
    }
    if (!s_hwnd) return;
    // One in flight at most: post again once the last one was dispatched (or
    // after a second, should one have been lost).
    uint32_t pumps = game::MainThreadPumps();
    if (now - s_posted_at < 40) return;
    if (pumps == s_seen && now - s_posted_at < 1000) return;
    s_seen = pumps;
    s_posted_at = now;
    PostMessageA(s_hwnd, WM_NULL, 0, 0);
}

// The quests the audio description watches.
std::mutex g_qs_mx;
std::vector<uint32_t> g_qs;              // under g_qs_mx
constexpr DWORD kHudRepeatAfterMs = 5 * 60 * 1000;   // five minutes

bool HudAlreadySaid(const std::string& line)
{
    DWORD now = GetTickCount();
    for (auto& s : g_hud_said) {
        if (s.text != line) continue;
        if (now - s.when < kHudRepeatAfterMs) return true;
        s.when = now;           // long enough ago to be worth hearing again
        return false;
    }
    if (g_hud_said.size() >= 64) g_hud_said.erase(g_hud_said.begin());
    g_hud_said.push_back({ line, now });
    return false;
}

void PollHudProse()
{
    if (!config::Get().hud_reader) return;
    if (!g_system_ready || !IsGameplayActive()) { g_hud_seen.clear(); return; }
    menu::Id m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) return;

    std::vector<std::string> now = game::GetHudProse();
    for (const auto& line : now) {
        bool seen = false;
        for (const auto& old : g_hud_seen) if (old == line) { seen = true; break; }
        if (seen) continue;
        if (HudAlreadySaid(line)) continue;
        if (LooksLikeHint(line) && !HintShouldSpeak(line)) {
            F3A_INFO("HUD (hint, already read): %s", line.c_str());
            continue;
        }
        F3A_INFO("HUD: %s", line.c_str());
        NoteVoiceLine(line.size());
        tolk::Speak(game::ExpandControlTokens(line), tolk::Priority::Ui, false);
    }
    g_hud_seen = std::move(now);
}

// Quest popups and new objectives — what the HUD's quest reminder shows.
//
// As in the FNV mod, read while ANY menu is up, not only in free play: the
// objective that follows a terminal command ("Otwórz tunel Nadzorcy" →
// "Otwórz właz Krypty") is put up while the terminal is still open, and a
// reader that waited for the menu to close found it gone — which is why that
// objective was never announced. Each text is said once it has held still for
// a couple of polls (the headline is animated letter by letter), and again
// only after something different was shown.
std::string g_qr_head_seen, g_qr_head_said;
std::string g_qr_stage_seen, g_qr_stage_said;
int         g_qr_head_stable = 0, g_qr_stage_stable = 0;
std::string g_qr_last_objective;   // shared with PollQuestChange (no double)
DWORD       g_qr_last_objective_at = 0;

void PollQuestReminder()
{
    if (!g_system_ready || !IsGameplayActive() || g_postload_cooldown > 0) {
        g_qr_head_seen.clear(); g_qr_head_said.clear(); g_qr_head_stable = 0;
        g_qr_stage_seen.clear(); g_qr_stage_said.clear(); g_qr_stage_stable = 0;
        return;
    }
    std::string head, stages;
    if (!game::GetQuestReminder(&head, &stages)) return;

    auto settle = [](const std::string& now, std::string& seen, int& stable) {
        if (now != seen) { seen = now; stable = 0; return false; }
        return ++stable == 2;            // exactly once, two polls after it settled
    };
    if (settle(head, g_qr_head_seen, g_qr_head_stable) &&
        !head.empty() && head != g_qr_head_said) {
        g_qr_head_said = head;
        F3A_INFO("Quest popup: '%s'", head.c_str());
        tolk::Speak(head, tolk::Priority::Ui, false);
    }
    if (settle(stages, g_qr_stage_seen, g_qr_stage_stable) &&
        !stages.empty() && stages != g_qr_stage_said) {
        g_qr_stage_said = stages;
        g_qr_last_objective = stages;
        g_qr_last_objective_at = GetTickCount();
        F3A_INFO("Quest objective: '%s'", stages.c_str());
        tolk::Speak(stages, tolk::Priority::Ui, false);
    }
    if (head.empty()) g_qr_head_said.clear();      // a new popup may repeat it
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
    if (g_postload_cooldown > 0) { reset(); return; }   // HUD still settling
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

    // Verb + what it is: "Rozmawiaj Doktor Li", "Weź Stimpak" — one phrase, the
    // way the FNV mod says it. The verb alone ("Okradnij") doesn't say WHO, and
    // enemies have no verb at all. A thing the game gave no name is NOT called
    // "obiekt": that only filled every glance at the scenery with noise, so an
    // unnamed thing with nothing to do on it is simply not announced. Distance
    // is off by default (CrosshairDistance=1 brings it back).
    const auto& c = config::Get();
    std::string say = verb;
    if (c.crosshair_names && t.known && !t.name.empty() && t.name != verb) {
        if (!say.empty()) say += " ";
        say += t.name;
    }
    if (say.empty()) return;   // keep watching: the verb may still appear
    if (c.crosshair_names && c.crosshair_distance && t.dist > 0.0f)
        say += ", " + strings::FormatDistance(t.dist);

    g_act_refid  = t.refid;
    g_act_prompt = say;
    F3A_INFO("Crosshair prompt: '%s'", say.c_str());
    tolk::Speak(say, tolk::Priority::Ui, /*interrupt=*/false);
}

void PollKeyboardSelection()
{
    if (!g_system_ready) return;
    if (menu::ActiveMenu() == menu::Id::TextEdit) return;   // TextEdit reader owns it
    if (menu::ActiveMenu() == menu::Id::Container) return;  // the container module owns it
    if (menu::ActiveMenu() == menu::Id::LevelUp) return;    // the level-up module owns it

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
    if (!IsGameplayActive() || g_postload_cooldown > 0) { g_quest_seen = false; return; }
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
        std::string line = strings::T("Zadanie: ", "Quest: ") + (name.empty() ? obj : name);
        if (!name.empty() && !obj.empty()) line += ", " + obj;
        // The HUD's quest popup (PollQuestReminder) normally says this first.
        bool popupSaid = !g_qr_last_objective.empty() && !obj.empty() &&
                         GetTickCount() - g_qr_last_objective_at < 30000 &&
                         g_qr_last_objective.find(obj) != std::string::npos;
        F3A_INFO("Quest change: %s%s", line.c_str(),
                 popupSaid ? " (already read from the HUD)" : "");
        if (!popupSaid) tolk::Speak(line, tolk::Priority::Ui, false);
    } else if (!obj.empty()) {
        // Already read off the HUD's quest reminder a moment ago? Then once is
        // enough. (Ui, not Background: a hint arriving at the same moment used
        // to drown it.)
        bool justSaid = !g_qr_last_objective.empty() &&
                        GetTickCount() - g_qr_last_objective_at < 30000 &&
                        g_qr_last_objective.find(obj) != std::string::npos;
        F3A_INFO("Quest change: new objective '%s'%s", obj.c_str(),
                 justSaid ? " (already read from the HUD)" : "");
        if (!justSaid) tolk::Speak(strings::T("Nowy cel: ", "New objective: ") + obj, tolk::Priority::Ui, false);
    }
}

// Announce SPECIAL attribute changes (Strength..Luck = AV codes 5..11). Makes
// the image-only "You're SPECIAL!" book (and any +SPECIAL effect) accessible:
// when a value changes we say "<name> <new value>". Throttled, and only while in
// gameplay (where SPECIAL can change), since it calls a game getter per attr.
// { Polish, English }: the language is picked when the name is spoken.
const char* g_special_names[7][2] = {
    { "Siła", "Strength" }, { "Percepcja", "Perception" },
    { "Wytrzymałość", "Endurance" }, { "Charyzma", "Charisma" },
    { "Inteligencja", "Intelligence" }, { "Zręczność", "Agility" },
    { "Szczęście", "Luck" }
};
int  g_special[7]      = { -1, -1, -1, -1, -1, -1, -1 };
bool g_special_init    = false;
int  g_special_throttle = 0;
// ~12 Hz poll, so this is roughly four seconds of uninterrupted gameplay.
constexpr int kSpecialSettleTicks = 48;

void PollSpecialChange()
{
    // The post-load cooldown ends while the save is still settling: the player
    // already exists, but equipment and perks are still being applied and the
    // attribute values move for a second or two afterwards. Every one of those
    // moves used to be announced — that was the "Siła 7, Percepcja 7,
    // Charyzma 6, Charyzma 6..." the player heard. Wait out the settling
    // before taking a baseline, and take the baseline silently.
    static int settle = 0;
    if (!IsGameplayActive() || g_postload_cooldown > 0) {
        g_special_init = false;
        settle = kSpecialSettleTicks;
        return;
    }
    // In the SPECIAL book the book module says every change itself, with the
    // attribute's name; this second voice repeated it a second later. The
    // baseline is taken afresh (silently) once the book closes.
    if (menu::IsOpen(menu::Id::SpecialBookMenu)) {
        g_special_init = false;
        return;
    }
    if (settle > 0) { --settle; return; }
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
    // Confirm a change before speaking it: right after a load these values
    // settle over a few polls, and announcing every intermediate reading is
    // where "Siła 7, Percepcja 7, Charyzma 6..." came from.
    static int  pending[7] = { -1, -1, -1, -1, -1, -1, -1 };
    static bool pending_set[7] = {};
    for (int i = 0; i < 7; ++i) {
        if (cur[i] == g_special[i]) { pending_set[i] = false; continue; }
        if (!pending_set[i] || pending[i] != cur[i]) {
            pending[i] = cur[i];           // first sighting — wait for a repeat
            pending_set[i] = true;
            continue;
        }
        g_special[i] = cur[i];
        pending_set[i] = false;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s %d",
                      strings::T(g_special_names[i][0], g_special_names[i][1]), cur[i]);
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
    tolk::Speak(cur ? strings::T("Trzecia osoba", "Third person")
                    : strings::T("Pierwsza osoba", "First person"),
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
// 0 = nothing; otherwise 1 + (container side ? 2 : 0) + (next ? 1 : 0).
std::atomic<int> g_container_filter{ 0 };
// 0 = nothing; 1 = focus your list, 2 = focus the container's list.
std::atomic<int> g_container_side{ 0 };
// 0 = nothing; otherwise 1 + (container column ? 2 : 0) + (down ? 1 : 0).
std::atomic<int> g_container_step{ 0 };
// Queueing a shot at a specific limb: the tile is chosen on the poll thread,
// clicked on the main one.
const void*       g_vats_limb_tile = nullptr;
std::atomic<bool> g_vats_limb_pending{ false };

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

// Aim TRACKING. A one-shot aim cannot win: the engine recomputes the view from
// its own state every frame, so whatever we set is gone by the time the player
// pulls the trigger. The FNV mod solves this by re-aiming every frame for as
// long as the aim button is held, and this is that — run from the main thread,
// where the game's own update happens, rather than from the ~12 Hz poll.
//
// frames > 0 counts down (a Home press: converge and hold briefly); frames < 0
// means "until told to stop" (the aim key is latched).
std::atomic<const void*> g_aimtrack_refr{ nullptr };
std::atomic<uint32_t>    g_aimtrack_refid{ 0 };
std::atomic<int>         g_aimtrack_up{ 0 };       // float bits
std::atomic<int>         g_aimtrack_frames{ 0 };
// Fallback aim point, for a target that has no usable reference — a quest
// marker, most of the time. Without this the whole tracker silently did
// nothing for exactly the targets the player most needs it for.
std::atomic<int> g_aimtrack_x{ 0 }, g_aimtrack_y{ 0 }, g_aimtrack_z{ 0 };
// Where on the reference the aim point lies — the asked-for point minus the
// reference's origin when the lock was taken (float bits). The held aim
// follows the reference as it moves, and must keep aiming at THAT point: the
// middle of a body or of a target board, not the origin, which sits at an
// actor's feet and at a rifle target's base, off to one side of the board.
std::atomic<int> g_aimtrack_offx{ 0 }, g_aimtrack_offy{ 0 }, g_aimtrack_offz{ 0 };

// Closed-loop pitch correction.
//
// The geometric aim has to guess where the shot leaves the player from. A guess
// that is off by a few dozen units is a miss at any useful range, and it was:
// the aim was landing under the practice targets even though the yaw read back
// exactly right. So ask the engine what the crosshair is actually ON, and while
// that is not our target, sweep a small pitch bias until it is. The bias that
// works is then kept — the error it corrects is the same everywhere, so this
// self-calibrates once and stays right.
float g_aim_bias  = 0.0f;   // GAME UNITS, added to the aim point's height
int   g_aim_probe = 0;      // position in the sweep
int   g_aim_check = 0;      // frames until the next crosshair check
int   g_aim_miss  = 0;      // consecutive checks that were NOT on target
bool  g_aim_settled = false;
constexpr int   kAimCheckEvery = 4;      // frames between crosshair checks
// +-96 units covers every plausible combination of eye height and aim point:
// the player's eye is somewhere between a crouching child and a tall adult,
// and a target's origin is at its foot or at its middle. The previous sweep
// was in degrees and reached about 11 degrees, which at two metres is only
// some 40 units — not enough to cross a target we were aiming a metre under.
constexpr int   kAimProbeMax   = 24;
constexpr float kAimProbeStep  = 8.0f;
// Two references this close in range belong to the same physical object.
constexpr float kSameAssembly  = 60.0f;
// How long after taking a new target the correction may still be searched for.
// The aim itself is held indefinitely, so "search while frames remain" no
// longer works as the window — this is it, and after it the aim stands still.
constexpr int   kAimSearchFrames = 150;   // ~2.5 s
int g_aim_search_left = 0;
// Set when the target visibly reacts to being struck; read once by the module
// that speaks it.
std::atomic<bool> g_aim_reacted{ false };
// A new target needs its reachable aim point worked out once, with the engine's
// ray, on the game's own thread.
std::atomic<bool> g_aim_solve{ false };
// A requested ray map, carried to the game's thread.
std::atomic<bool> g_raymap{ false };
std::atomic<bool> g_rayscan{ false };
// Refine a held aim with the engine's rays: the reference to confirm, and a
// few frames' grace so the geometric aim has been applied first.
std::atomic<uint32_t> g_rayrefine_id{ 0 };
// Held-aim line of sight: what to watch for, and the last answer.
std::atomic<uint32_t> g_los_want{ 0 };
std::atomic<uint32_t> g_los_hit{ 0 };
std::atomic<uint32_t> g_los_dist{ 0 };
int  g_los_every = 0;
bool g_los_on = false;
bool g_los_said = false;        // has the first reading been logged yet
// Someone who is NOT an enemy standing in the line of fire of a held aim:
// found on the game's thread (the ray), spoken by the poll thread.
std::mutex  g_fire_mx;
std::string g_fire_name;        // under g_fire_mx; non-empty = a warning to speak
uint32_t    g_fire_warned = 0;  // main thread: who was last warned about
int         g_fire_clear  = 0;  // main thread: readings since the ray left them
int  g_aim_reapply = 0;         // passes until the held aim is re-applied
// Watching the held target's health, to notice a shot landing.
std::atomic<uint32_t> g_hit_last_hp{ 0 };
std::atomic<bool>     g_hit_pending{ false };
std::atomic<bool>     g_kill_pending{ false };
// WHO we are watching for damage, kept apart from whatever the aim is
// currently pointed at.
//
// These used to be the same thing, and that is why a firefight was silent:
// once the ray refinement has found the exact spot on the target that a shot
// can reach, it re-locks the aim onto that POINT and deliberately passes no
// reference (handing one over would make the tracker go back to aiming at the
// target's origin and undo the measurement). The health watch hung off that
// same reference, so the moment the aim got GOOD, the watch went blind — and
// the player emptied a magazine into an officer with nothing to say whether
// any of it landed.
std::atomic<const void*> g_hit_watch_refr{ nullptr };
std::atomic<uint32_t>    g_hit_watch_id{ 0 };
bool g_hit_have  = false;
int  g_hit_every = 0;
// No line of fire: how far the blocker is, and how high off the floor.
std::atomic<uint32_t> g_block_dist{ 0 }, g_block_up{ 0 };
std::atomic<bool>     g_block_pending{ false };
int g_rayrefine_delay = 0;
std::atomic<int>  g_raymap_x{ 0 }, g_raymap_y{ 0 }, g_raymap_z{ 0 };
// What the ray found, kept as an OFFSET from the reference's origin rather than
// as a fixed point: a practice target never moves, but an enemy does, and
// freezing the aim on the spot where one used to stand would be a poor trade.
std::atomic<int> g_aim_ox{ 0 }, g_aim_oy{ 0 }, g_aim_oz{ 0 };
// Moving the mouse hands control back. Roughly two degrees in a frame — far
// more than the engine's own weapon sway, far less than a deliberate turn.
constexpr float kMouseTakeover = 0.035f;

// Frames to leave the takeover detector alone after the aim is DELIBERATELY
// moved.
//
// The detector compares where the view is now against what we wrote last
// frame, and calls a big difference "the player grabbed the mouse". But the
// ray refinement moves the aim on purpose, by six to twelve degrees, the
// moment it finds the exact spot on the target a shot can reach — so the
// detector saw OUR OWN correction and handed control back to a mouse the
// player does not even own. Every lock in the log ends the same way, 0.14 s
// after it starts: "released — the view moved +0.000 yaw / -0.163 pitch, so
// the mouse has it". Yaw exactly zero, pitch always the same amount: that is
// not a hand on a mouse, that is us.
constexpr int kAimSettleFrames = 12;
int g_aim_settle = 0;
// Requests from the poll thread for the main thread's aim state (see
// RequestAimTrack / NudgeAim): the counters below belong to the main thread.
std::atomic<bool> g_aimtrack_reset_pending{ false };
std::atomic<int>  g_nudge_units{ 0 };   // trims not yet applied, whole units
int g_takeover = 0;
std::atomic<bool> g_aim_released{ false };

// Sweep outwards from zero, alternating sides: 0, +1, -1, +2, -2, ... so the
// nearest correction is always tried first.
float ProbeBias(int step)
{
    if (step <= 0) return 0.0f;
    int mag  = (step + 1) / 2;
    float v  = mag * kAimProbeStep;
    return (step & 1) ? v : -v;
}

int g_aim_tone_ticks = 0;   // paces the "on target" tone

void AimTrackReset()
{
    g_aim_check = 0;
    g_aim_miss  = 0;
    g_aim_search_left = kAimSearchFrames;
    g_aim_solve.store(true);
    g_aim_ox.store(0); g_aim_oy.store(0); g_aim_oz.store(0);
    // A bias that has already put the crosshair on a target is KEPT: it
    // corrects our estimate of where the shot leaves from, which does not
    // change between targets. It still gets re-verified below, and the sweep
    // resumes from it if this target says otherwise.
    if (!g_aim_settled) { g_aim_probe = 0; g_aim_bias = 0.0f; }
}

// Lead a target that is running: a shot is not instant. The BB gun's pellet
// flies at 8000 units a second (BBProjectile in Fallout3.esm) and a charging
// radroach is only 32 units wide, so aiming at where it IS puts the pellet
// where it was. The speed is measured from the reference's own movement,
// frame to frame and smoothed; a target standing still (a practice board, a
// person waiting) gets no lead at all. Main thread only (AimTick). Call with
// refid 0 to forget the target. Returns false when there is nothing to add.
bool AimLead(uint32_t refid, const game::Vec3& origin, const game::Vec3& aim,
             game::Vec3* out)
{
    constexpr float kShotSpeed = 8000.0f;   // units/s; FO3 bullets are this fast or faster
    constexpr float kMinSpeed  = 40.0f;     // slower than this is standing still
    constexpr float kMaxLead   = 64.0f;     // never swing further off than this
    static uint32_t      s_id = 0;
    static game::Vec3    s_last{};
    static LARGE_INTEGER s_t{};
    static float s_vx = 0.0f, s_vy = 0.0f, s_vz = 0.0f;
    static int   s_samples = 0;
    if (!refid || !out) { s_id = 0; return false; }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = freq.QuadPart > 0
        ? (float)(now.QuadPart - s_t.QuadPart) / (float)freq.QuadPart : 0.0f;
    if (refid != s_id || !(dt > 0.0f && dt <= 0.5f)) {
        // A new target, or the lock was paused (a menu): start measuring again.
        s_id = refid; s_last = origin; s_t = now;
        s_vx = s_vy = s_vz = 0.0f; s_samples = 0;
        return false;
    }
    if (dt < 0.004f) return false;          // same frame twice: nothing new
    float ix = (origin.x - s_last.x) / dt;
    float iy = (origin.y - s_last.y) / dt;
    float iz = (origin.z - s_last.z) / dt;
    s_last = origin; s_t = now;
    // "Must be within": a jump of thousands of units a second is a teleport
    // or a respawn, not running — and NaN fails this test too.
    if (!(ix * ix + iy * iy + iz * iz <= 2000.0f * 2000.0f)) {
        s_vx = s_vy = s_vz = 0.0f; s_samples = 0;
        return false;
    }
    const float a = 0.35f;                  // smoothing: a few frames' worth
    s_vx += (ix - s_vx) * a; s_vy += (iy - s_vy) * a; s_vz += (iz - s_vz) * a;
    if (++s_samples < 3) return false;
    float sp = std::sqrt(s_vx * s_vx + s_vy * s_vy);
    if (!(sp >= kMinSpeed)) return false;
    game::Vec3 eye{ aim.x, aim.y, aim.z };
    if (auto* pl = fose_rt::Player()) {
        eye = { pl->posX, pl->posY, pl->posZ + 100.0f };
        game::GetCameraPos(&eye);
    }
    float ex = aim.x - eye.x, ey = aim.y - eye.y, ez = aim.z - eye.z;
    float range = std::sqrt(ex * ex + ey * ey + ez * ez);
    float t = range / kShotSpeed + 1.0f / 60.0f;   // flight + the frame until it fires
    float lx = s_vx * t, ly = s_vy * t, lz = s_vz * t;
    float len = std::sqrt(lx * lx + ly * ly + lz * lz);
    if (!(len > 0.5f)) return false;
    if (len > kMaxLead) { float k = kMaxLead / len; lx *= k; ly *= k; lz *= k; }
    *out = { lx, ly, lz };
    return true;
}
// Aim at a REFERENCE's body (collision centre, measured from the camera). The
// model reads and virtual calls make this main-thread-only.
const void*       g_aimref_refr = nullptr;
std::atomic<uint32_t> g_aimref_id{ 0 };
std::atomic<bool> g_aimref_pending{ false };

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
                  strings::T("Zdrowie %d, akcja %d, promieniowanie %d",
                             "Health %d, action points %d, rads %d"), hp, ap, rad);
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
        tolk::Speak(strings::T("Na celu, strzelaj.", "On target, fire."), tolk::Priority::Ui, true);
        g_naim_phase = 0;
        return;
    }
    if (g_naim_step + 1 >= kAimSteps) {
        // Grid exhausted — the engine's crosshair pick never returned the target
        // (typically out of its short range). Re-centre on the geometric aim and
        // trust it; the player fires with the LOS cue.
        PointAt(0.0f, 0.0f);
        tolk::Speak(strings::T("Celuję na oko, strzelaj.", "Aiming roughly, fire."), tolk::Priority::Ui, true);
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
uint32_t          g_teleport_refid = 0;
std::atomic<bool> g_teleport_pending{ false };
// Press the map's "travel to" button (a HandleClick → main thread only).
std::atomic<bool> g_map_travel_pending{ false };
// The quantity prompt's state, sampled on the MAIN thread. Reading that menu
// from the poll thread crashed the game: the player's own key press closes it on
// the main thread, and our reader was walking its tiles as they were being freed
// (the log ended right after the menu opened, with no click of ours involved).
// VATS numbers, sampled on the MAIN thread for the same reason as the quantity
// prompt: VATS closes under the player's own key press, and reading a menu the
// main thread is freeing is what crashed the game repeatedly.
std::atomic<bool> g_vats_valid{ false };
std::atomic<int>  g_vats_ap{ 0 }, g_vats_maxap{ 0 }, g_vats_clip{ 0 };
std::atomic<int>  g_vats_reserve{ 0 }, g_vats_queued{ 0 }, g_vats_chance{ 0 };
std::atomic<bool> g_vats_has_ap{ false }, g_vats_has_ammo{ false }, g_vats_has_chance{ false };

std::atomic<bool> g_qty_open{ false };
std::atomic<int>  g_qty_amount{ 0 };
std::atomic<int>  g_qty_max{ 0 };

// Loot Menu Updated's overlay, sampled on the MAIN thread for the same reason
// as the two above: the loot mod rebuilds those item tiles as the player
// scrolls, and we must not be walking them while it does.
//
// It carries TEXT, so a plain set of atomics won't do. The writer bumps the
// sequence before and after the copy; a reader that sees an odd number, or a
// different number afterwards, read a half-written record and tries again.
std::atomic<unsigned> g_loot_seq{ 0 };
game::LootMenuInfo    g_loot_snapshot;

// "Cycle the body part N times" (see the dispatch site for why N, not 1).
// Which limb the cursor is being steered to (-1 = idle), and whether to click
// once it arrives.
std::atomic<bool> g_vats_attack{ false };   // queue a shot on the next pass
std::atomic<int>  g_vats_cursor_to{ -1 };   // limb to put the menu cursor on
int   g_vats_click_hold = 0;     // frames left holding the attack button down
// Which limb VATS is aimed at, sampled on the main thread. Carries text, so it
// uses the same sequence guard as the loot overlay.
std::atomic<unsigned> g_vats_pick_seq{ 0 };
game::VatsPick        g_vats_pick;
bool                  g_vats_pick_live = false;   // main thread only
// SPECIAL for the vault's SPECIAL book, sampled on the main thread while that
// menu is open (see PlayerSpecial).
std::atomic<int>  g_special_live[7];
std::atomic<bool> g_special_live_ok{ false };
std::atomic<int>  g_book_page{ -1 }, g_book_index{ 0 }, g_book_max{ 0 };
std::atomic<bool> g_book_ok{ false };
// The attributes' own descriptions, read once on the main thread.
std::mutex        g_special_desc_mx;
std::string       g_special_desc[7];
bool              g_special_desc_tried = false;   // main thread only
// The aim key is held: widen the engine's bullet correction (main thread).
std::atomic<bool> g_engine_aim_want{ false };

// Press the inventory's "Upuść" button (HandleClick → main thread only).
std::atomic<bool> g_drop_pending{ false };
// Track the quest highlighted in the Pip-Boy (clicks its row; main thread only).
std::atomic<bool> g_track_quest_pending{ false };
// Press whatever row the keyboard highlight is on, in any menu. The game's
// controls page only enters its "press a new key" mode on a MOUSE CLICK, so
// without this a blind player simply cannot rebind anything.
std::atomic<bool> g_press_row_pending{ false };
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
    PollQuestReminder();
    PollTerminal();
    PollRaceSex();
    PollHudMessages();
    PollHudProse();
    PollActivatePrompt();
}

// Penning the pointer inside the game window while we steer it. Without this
// the injected moves walk the desktop cursor across whatever else is on screen,
// which is both alarming and a way to click something that isn't the game.
void ClipToGameWindow(bool on)
{
    static bool clipped = false;
    if (on == clipped) return;
    if (!on) { ClipCursor(nullptr); clipped = false; return; }
    HWND w = GetForegroundWindow();
    RECT r{};
    if (!w || !GetWindowRect(w, &r)) return;
    ClipCursor(&r);
    clipped = true;
}

// Experience and level-ups — the feedback a sighted player gets from the XP
// meter after a kill, a lockpick or a quest stage.
//
// As in the FNV mod: watch the player's XP actor value (24) and speak the gain,
// "+25 PD". Awards that land together (a kill and a quest stage, a whole VATS
// queue of kills) are added up and said once: a gain is only spoken after the
// value has held still for a moment. The level-up banner is the game's own
// text, read off the meter while it shows. The baseline is taken silently
// after every load, the same way the SPECIAL poller does it.
void PollXpChange()
{
    static int   s_settle  = 0;
    static int   s_throttle = 0;
    static int   s_xp      = -1;      // last value spoken (or the baseline)
    static int   s_pending = -1;      // newer value waiting to settle
    static DWORD s_since   = 0;
    static bool  s_lvl_on  = false;
    static DWORD s_lvl_said = 0;
    if (!IsGameplayActive() || g_postload_cooldown > 0) {
        s_xp = -1; s_pending = -1; s_lvl_on = false;
        s_settle = kSpecialSettleTicks;
        return;
    }
    if (s_settle > 0) { --s_settle; return; }
    if (--s_throttle > 0) return;
    s_throttle = 6;

    DWORD now = GetTickCount();
    std::string lvl = game::GetHudLevelUpText();
    if (!lvl.empty() && !s_lvl_on && now - s_lvl_said > 5000) {
        s_lvl_said = now;
        F3A_INFO("Level up banner: %s", lvl.c_str());
        tolk::Speak(lvl + "!", tolk::Priority::Ui, false);
    }
    s_lvl_on = !lvl.empty();

    float v = game::GetPlayerAV(24);           // eActorVal_XP
    if (v < 0.0f) return;
    int xp = (int)(v + 0.5f);
    if (s_xp < 0 || xp < s_xp) {               // baseline, or XP went down
        if (s_xp < 0) F3A_INFO("XP baseline: %d", xp);
        s_xp = xp; s_pending = -1;
        return;
    }
    if (xp == s_xp) { s_pending = -1; return; }
    if (xp != s_pending) { s_pending = xp; s_since = now; return; }
    if (now - s_since < 600) return;            // still adding up
    int gain = xp - s_xp;
    s_xp = xp; s_pending = -1;
    F3A_INFO("XP +%d (now %d)", gain, xp);
    char buf[48];
    std::snprintf(buf, sizeof(buf), strings::T("+%d PD", "+%d XP"), gain);
    tolk::Speak(buf, tolk::Priority::Ui, false);
}

void MainThreadWork()
{
    game::NoteMainThread();
    // Who is fighting the player: a virtual call per actor, so it is worked
    // out here and ScanNearby (poll thread) reads the result.
    {
        static DWORD s_hostile_at = 0;
        DWORD now = GetTickCount();
        if (IsGameplayActive() && now - s_hostile_at >= 300) {
            s_hostile_at = now;
            game::RefreshHostiles();
        }
    }
    if (config::IsEnabled()) {       // F12 off = no unasked-for speech
        PollSpecialChange();
        PollXpChange();
        PollViewChange();
    }
    TickNativeAim();
    // The engine's own bullet correction, widened while the aim key is held
    // and put back the moment it is not — or the mod is off, or a load starts.
    {
        static bool     s_on = false;
        static DWORD    s_log_at = 0;
        static uint32_t s_seen = 0xFFFFFFFFu;
        const int deg = config::Get().engine_aim_deg;
        bool want = g_engine_aim_want.load() && deg > 0 && config::IsEnabled() &&
                    IsGameplayActive();
        if (want != s_on) {
            game::SetEngineAutoAim(want, (float)deg);
            s_on = want;
            s_seen = 0xFFFFFFFFu;
        }
        // Which actor the game itself has picked to bend the shots onto — the
        // evidence that the correction is in play, logged when it changes.
        if (s_on && GetTickCount() - s_log_at >= 500) {
            s_log_at = GetTickCount();
            uint32_t t = game::EngineAutoAimTargetId();
            if (t != s_seen) {
                s_seen = t;
                F3A_INFO("EngineAim: the game's target is %08X (our lock %08X)",
                         t, g_aimtrack_refid.load());
            }
        }
    }
    if (g_teleport_pending.exchange(false))
        game::TeleportPlayerToRef(g_teleport_refr, g_teleport_refid);
    AnnounceStatus();
    if (g_menuback_pending.exchange(false)) game::ClickMenuBack();
    if (g_restore_pending.exchange(false)) {
        // Silent unless it actually did something. R is the reload key during
        // play, so a spoken "open the controls page first" fired on every
        // reload — advice for something the player was not trying to do.
        if (game::ClickRestoreDefaults())
            tolk::Speak(strings::T("Przywracam domyślne sterowanie. Potwierdź wybór.",
                                   "Restoring default controls. Confirm your choice."),
                        tolk::Priority::System, true);
        else
            F3A_DEBUG("RestoreDefaults: not on the controls page — ignored.");
    }
    if (g_skip_pending.exchange(false)) {
        bool ok = game::AdvanceTrackedQuestStage();
        tolk::Speak(ok ? strings::T("Przeskoczono etap zadania. Sprawdź nowy cel.",
                                    "Quest stage skipped. Check the new objective.")
                       : strings::T("Nie udało się przeskoczyć etapu — brak aktywnego zadania.",
                                    "Could not skip the stage: no active quest."),
                    tolk::Priority::System, true);
    }
    // Sample the quantity prompt while we are on the thread that owns it — but
    // ONLY when it is actually up. Every other block here sits behind a request
    // flag and does nothing per frame; this one ran unconditionally from the very
    // first frame, walking the menu tree during the loading screen before the
    // interface exists, and that crashed the game on startup. The menu id comes
    // from our own dispatcher state, so testing it costs nothing.
    if (g_system_ready && menu::IsOpen(menu::Id::VATS)) {
        game::VatsInfo v;
        if (game::GetVatsInfo(&v)) {
            g_vats_ap.store(v.ap); g_vats_maxap.store(v.max_ap);
            g_vats_clip.store(v.clip_ammo); g_vats_reserve.store(v.reserve_ammo);
            g_vats_queued.store(v.queued); g_vats_chance.store(v.hit_chance);
            g_vats_has_ap.store(v.has_ap); g_vats_has_ammo.store(v.has_ammo);
            g_vats_has_chance.store(v.has_chance);
            g_vats_valid.store(true);
        }
        game::VatsPick pick;
        game::GetVatsPick(&pick);
        // The target's name is read here too: VATS is closed by a key press on
        // this thread, and the poll thread reading its tiles raced with that.
        if (auto t = game::GetVatsSelectionText())
            std::snprintf(pick.target, sizeof(pick.target), "%s", t->c_str());
        pick.sampled = true;
        g_vats_pick_seq.fetch_add(1, std::memory_order_acq_rel);
        g_vats_pick = pick;
        g_vats_pick_seq.fetch_add(1, std::memory_order_acq_rel);
        g_vats_pick_live = true;
    } else {
        if (g_vats_valid.load()) g_vats_valid.store(false);
        if (g_vats_pick_live) {
            // Forget the closed session's pick, so the next VATS does not open
            // on the last one's target.
            g_vats_pick_live = false;
            g_vats_pick_seq.fetch_add(1, std::memory_order_acq_rel);
            g_vats_pick = game::VatsPick{};
            g_vats_pick_seq.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (g_system_ready && menu::IsOpen(menu::Id::SpecialBookMenu)) {
        for (int i = 0; i < 7; ++i) {
            float v = game::GetPlayerAV(5 + i);     // 5..11 = SPECIAL
            g_special_live[i].store(v >= 0.0f ? (int)(v + 0.5f) : -1);
        }
        g_special_live_ok.store(true);
        if (!g_special_desc_tried) {
            g_special_desc_tried = true;
            int got = 0;
            for (int i = 0; i < 7; ++i) {
                std::string d = game::GetSpecialDescription(i);
                if (!d.empty()) ++got;
                std::lock_guard<std::mutex> lk(g_special_desc_mx);
                g_special_desc[i] = std::move(d);
            }
            F3A_INFO("SpecialBook: %d of 7 descriptions read from the game", got);
        }
        int pg = -1, ix = 0, mx = 0;
        if (game::GetSpecialBookState(&pg, &ix, &mx)) {
            g_book_page.store(pg); g_book_index.store(ix); g_book_max.store(mx);
            g_book_ok.store(true);
        } else {
            g_book_ok.store(false);
        }
    } else if (g_special_live_ok.load() || g_book_ok.load()) {
        g_special_live_ok.store(false);
        g_book_ok.store(false);
    }

    // The loot overlay lives ON the HUD, so there is no menu transition to hang
    // this off — it has to be sampled whenever the player is in the world.
    if (g_system_ready && g_in_gameplay) {
        game::LootMenuInfo loot;
        if (!game::GetLootMenuInfo(&loot)) loot = game::LootMenuInfo{};
        if (loot.visible || g_loot_snapshot.visible) {
            g_loot_seq.fetch_add(1, std::memory_order_acq_rel);   // -> odd
            g_loot_snapshot = loot;
            g_loot_seq.fetch_add(1, std::memory_order_acq_rel);   // -> even
        }
    }

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
    if (g_press_row_pending.exchange(false)) {
        // Settings pages only. The key is F2 — a Pip-Boy page key of the
        // game's own — so anywhere else it is the game's: in the Pip-Boy a
        // "click the selected row" would use or equip whatever is selected.
        if (!game::SettingsPageOpen()) {
            F3A_DEBUG("PressRow: not on a settings page - left to the game.");
        } else if (game::ClickSelectedRowIn(nullptr)) {
            tolk::Speak(strings::T("Naciśnij nowy klawisz.", "Press the new key."), tolk::Priority::System, true);
        } else {
            tolk::Speak(strings::T("Nie ma zaznaczonej pozycji.", "Nothing is selected."), tolk::Priority::System, true);
        }
    }
    if (g_track_quest_pending.exchange(false)) {
        if (game::ClickSelectedRowIn("MM_QuestsList"))
            tolk::Speak(strings::T("Śledzę to zadanie.", "Tracking this quest."), tolk::Priority::Ui, true);
        else
            tolk::Speak(strings::T("Najpierw wybierz zadanie na liście.", "First select a quest in the list."),
                        tolk::Priority::System, true);
    }
    if (g_drop_pending.exchange(false)) {
        // The inventory menu has a real Drop button; clicking it through the
        // engine gives the vanilla behaviour, including the quantity prompt for
        // a stack. No item juggling on our side.
        if (!game::ClickMenuButton(kMenuType_Inventory, "IM_DropButton", 8))
            tolk::Speak(strings::T("Otwórz ekwipunek i wybierz przedmiot, potem Delete.",
                                   "Open the inventory and select an item, then press Delete."),
                        tolk::Priority::System, true);
    }
    if (g_map_marker_pending.exchange(false)) {
        if (!game::ClickMapMarkerTile(g_map_marker_tile))
            tolk::Speak(strings::T("Nie znalazłem tej lokacji na mapie świata.",
                                   "Could not find this location on the world map."),
                        tolk::Priority::System, true);
    }
    if (g_map_travel_pending.exchange(false)) {
        bool ok = game::ClickMapTravelButton();
        if (!ok) tolk::Speak(strings::T("Nie znalazłem przycisku podróży na mapie.",
                                        "Could not find the travel button on the map."),
                             tolk::Priority::System, true);
    }
    if (int c = g_container_filter.exchange(0)) {
        int v = c - 1;
        bool side = (v & 2) != 0, next = (v & 1) != 0;
        if (!game::ClickContainerFilter(side, next))
            tolk::Speak(strings::T("Nie znalazłem przełącznika kategorii.", "Could not find the category switch."),
                        tolk::Priority::System, true);
    }
    if (int c = g_container_step.exchange(0)) {
        int v = c - 1;
        bool side = (v & 2) != 0;
        int r = game::StepContainerSelection(side, (v & 1) ? +1 : -1);
        // At either end the selection does not move, so nothing new would be
        // read: say the row again, which is the answer a list gives there.
        if (r == 2) {
            if (auto s = game::GetContainerSelection())
                tolk::Speak(s->value.empty() ? s->label : s->label + ", " + s->value,
                            tolk::Priority::Ui, true);
        } else if (r == 0) {
            tolk::Speak(side ? strings::T("Pojemnik jest pusty.", "The container is empty.")
                            : strings::T("Nie masz nic przy sobie.", "You are carrying nothing."),
                        tolk::Priority::Ui, true);
        }
    }
    if (int c = g_container_side.exchange(0)) {
        bool toContainer = (c >= 2);
        bool quiet = (c == 3);       // the module taking its focus back
        int r = game::SwitchContainerSide(toContainer);
        // A refusal is the menu saying that column is empty — say so, or the
        // key press simply vanishes. A switch speaks for itself: the module
        // announces the new column and row as the highlight lands there.
        if (quiet) {
            if (r <= 0) F3A_INFO("Container: focus restore refused (r=%d)", r);
        } else if (r == 0)
            tolk::Speak(toContainer ? strings::T("Pojemnik jest pusty.", "The container is empty.")
                                   : strings::T("Nie masz nic przy sobie.", "You are carrying nothing."),
                        tolk::Priority::Ui, true);
        else if (r < 0)
            tolk::Speak(strings::T("Nie mogę przełączyć kolumny w tej wersji gry.",
                                "Cannot switch columns in this version of the game."),
                        tolk::Priority::System, true);
    }
    if (int c = g_vats_click.exchange(0)) {
        const char* btn = (c == 2) ? "left_arrow"
                        : (c == 3) ? "right_arrow"
                        : (c == 4) ? "Select_button"   // "Wybierz" = queue a shot
                                   : "BodyPart_button";
        // Logged: when A or D produce the game's own error beep instead of a
        // target change, the question is whether OUR click reached the arrow
        // at all, and only the menu can answer that.
        bool ok = game::ClickVatsButton(btn);
        F3A_INFO("VATS button '%s': %s", btn, ok ? "clicked" : "NOT FOUND");
    }
    // Queue a shot: press and HOLD the attack button, and move NOTHING.
    //
    // There used to be a cursor servo here: it drove the mouse across the
    // screen towards a limb marker, learning a gain as it went, because we
    // believed FO3's VATS could only be worked with the cursor. It cannot only
    // be worked that way. The game's own PC controls already do all of the
    // selecting — W and S step through body parts, A and D switch targets — so
    // the servo was duplicating the engine and fighting it at the same time:
    // moving the mouse makes the menu re-pick whatever is under the cursor,
    // which overrides the keyboard selection (the New Vegas tweak mod ships a
    // bugfix for exactly that). It also dragged the system cursor around, which
    // is how it ended up stuck on a window outside the game.
    //
    // What is genuinely needed is the attack BUTTON, which queues a shot at
    // whatever is currently selected. A button is not a movement: nothing on
    // screen and nothing outside the game moves because of this.
    // Move the ENGINE's cursor onto a body-part marker. Done here because it
    // calls into the game, and this block runs on the game's own thread — the
    // lesson from the aim-point crash, which did the same kind of work from the
    // polling thread and took the game down.
    if (menu::IsOpen(menu::Id::VATS)) {
        int want = g_vats_cursor_to.exchange(-1);
        if (want >= 0) {
            auto limbs = game::GetVatsLimbs();
            if (want < (int)limbs.size()) {
                // The CENTRE of the body-part marker, and through the engine's
                // own cursor position. Writing only the cursor tile (the old
                // way) moved a picture: the engine went on testing its real
                // cursor, which stayed wherever the mouse was — so VATS kept
                // picking the torso or the right leg whatever W and S said.
                const auto& L = limbs[want];
                float cx = L.x + (L.w > 0.0f ? L.w * 0.5f : 0.0f);
                float cy = L.y + (L.h > 0.0f ? L.h * 0.5f : 0.0f);
                bool moved = game::MoveEngineCursorTo(cx, cy);
                if (!moved) game::SetMenuCursorPos(cx, cy);   // old way, harmless
                F3A_INFO("VATS cursor: -> %s at (%.0f,%.0f) size %.0fx%.0f | "
                         "engine cursor moved=%d",
                         L.name.c_str(), cx, cy, L.w, L.h, (int)moved);
            }
        }
    }

    if (g_vats_attack.exchange(false) && menu::IsOpen(menu::Id::VATS)) {
        // The game samples input once per frame, so a press and a release
        // delivered together can land inside one sample and register as no
        // click at all. Hold it for several frames; released below.
        INPUT in{}; in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &in, sizeof(in));
        g_vats_click_hold = 6;
        F3A_INFO("VATS: attack button held down to queue a shot");
    }

    // Let the held mouse button up once the game has had frames to see it.
    if (g_vats_click_hold > 0 && --g_vats_click_hold == 0) {
        INPUT in{}; in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(in));
        F3A_INFO("VATS point: button released");
    }

    if (g_aimref_pending.exchange(false)) {
        // Guard the pointer with the id it had when queued: a cell change can
        // free it, and the address gets reused.
        game::Vec3 pos{};
        uint32_t want = g_aimref_id.load();
        if (game::GetRefPosition(g_aimref_refr, want, &pos)) {
            std::string how;
            if (game::AimAtReference(g_aimref_refr, &how)) {
                // The aim point tells the player how precise this is: a
                // collision hit is exact, an origin fallback is a guess.
                tolk::Speak(how == "collision" ? strings::T("Na celu.", "On target.")
                          : how == "bounds"    ? strings::T("Na celu.", "On target.")
                                               : strings::T("Celuję z grubsza.", "Aiming roughly."),
                            tolk::Priority::Ui, false);
            }
        }
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
    // Aiming normally rides the movement hook (see AimTick). Without that hook
    // this is the only main-thread slot we have; it is the wrong point in the
    // frame, but a weak aim beats none.
    if (!mover::Available()) AimTick();

    if (g_aim_pending.exchange(false)) g_aim_frames = kAimFrames;
    if (g_aim_frames > 0) {
        --g_aim_frames;
        // Yaw first (heading), then pitch — both via the engine's SetAngle.
        if (g_aim_do_yaw.load())
            game::SetPlayerAngleDeg('Z', BitsToF(g_aim_yaw_bits.load()));
        if (g_aim_do_pitch.load())
            game::SetPlayerAngleDeg('X', BitsToF(g_aim_pitch_bits.load()));
    }

    // Keep the aim ON the target, every frame, for as long as tracking is on.
    // This is the half that was missing: aiming once and hoping is how the
    // crosshair ended up next to the target instead of on it.
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

    // Updated every tick, not only on a change, and taken from the RAW engine
    // list rather than from our dispatch stack: the whole point is to notice
    // menus we do not recognise. The HUD is not a menu in this sense — it is
    // what is up while you are playing.
    {
        bool blocking = false;
        for (UInt32 t : active)
            if (t != kMenuType_HUDMain) { blocking = true; break; }
        g_menu_mode.store(blocking);
    }

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
                    strings::RenderArgs(strings::Key::MenuOpened,
                                        strings::T("Menu główne", "Main menu")),
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
                // A game loaded (not just a door): the audio description and
                // exploration start afresh.
                bool gameLoad = g_game_load_pending.exchange(false);
                modules::audiodesc::OnLoadingClosed(gameLoad);
                if (gameLoad) modules::explore::ResetSession();
                g_pov_announced.store(-1);
                // A held or calibration aim must not carry into the loaded
                // save (F9 is the game's quickload), nor the old save's map
                // markers or quest baseline.
                StopAimTrack();
                WatchAimLos(0);
                g_hit_watch_refr.store(nullptr);
                g_rayrefine_id.store(0);
                game::ClearMapMarkerCache();
                g_quest_seen = false;
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

    if (!quantity_up && config::IsEnabled()) {
        menu::OnTick(dt);
        PollActiveTile();
        PollMenuReaders();
    }

    // Always polled: with the mod switched off only the toggle key acts (see
    // hotkeys::Poll) — before, F12 could switch the mod off but never on.
    hotkeys::Poll();

    PumpGameThread();
    if (config::IsEnabled()) {
        modules::autowalk::Tick(dt);
        modules::guide::Tick(dt);
        modules::intro::Tick(dt);
        if (!quantity_up) {
            // These search the menu tree (lockpick cue, hacking grid, map list).
            modules::worldscan::Tick(dt);
            modules::hacking::Tick(dt);
        modules::specialbook::Tick(dt);
            modules::mapnav::Tick(dt);
        }
        modules::quantity::Tick(dt);   // sampled state only — safe
        modules::lootmenu::Tick(dt);   // sampled state only — safe
        modules::sonar::Tick(dt);      // speaks the sweep's summary when it ends
        modules::explore::Tick(dt);    // finds on the way during auto-walk
        modules::audiodesc::Tick(dt);  // descriptions of scenes and people
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

void RequestContainerFilter(bool container_side, bool next)
{
    g_container_filter.store(1 + (container_side ? 2 : 0) + (next ? 1 : 0));
}

void RequestContainerStep(bool container_side, int delta)
{
    g_container_step.store(1 + (container_side ? 2 : 0) + (delta > 0 ? 1 : 0));
}

void RequestContainerSide(bool container_side, bool quiet)
{
    g_container_side.store(container_side ? (quiet ? 3 : 2) : 1);
}

void RequestVatsLimbClick(const void* tile)
{
    g_vats_limb_tile = tile;
    g_vats_limb_pending.store(true);
}
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
void RequestTeleport(const void* refr, uint32_t refid)
{
    g_teleport_refr  = refr;
    g_teleport_refid = refid;
    g_teleport_pending.store(true);
}

void RequestAimAtRef(const void* refr, uint32_t refid)
{
    g_aimref_refr = refr;
    g_aimref_id.store(refid);
    g_aimref_pending.store(true);
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

void RequestPressRow() { g_press_row_pending.store(true); }

bool LootMenuState(game::LootMenuInfo* out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        unsigned before = g_loot_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;                 // a write is in progress
        game::LootMenuInfo copy = g_loot_snapshot;
        if (g_loot_seq.load(std::memory_order_acquire) != before) continue;
        *out = copy;
        return copy.visible;
    }
    return false;
}


void RequestVatsAttack() { g_vats_attack.store(true); }
void RequestVatsCursorTo(int limbIndex) { g_vats_cursor_to.store(limbIndex); }

bool VatsPickState(game::VatsPick* out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        unsigned before = g_vats_pick_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        game::VatsPick copy = g_vats_pick;
        if (g_vats_pick_seq.load(std::memory_order_acquire) != before) continue;
        *out = copy;
        return copy.count > 0;
    }
    return false;
}

void SetEngineAimAssist(bool on) { g_engine_aim_want.store(on); }

std::string SpecialDescription(int i)
{
    if (i < 0 || i >= 7) return {};
    std::lock_guard<std::mutex> lk(g_special_desc_mx);
    return g_special_desc[i];
}

bool SpecialBookState(int* page, int* indexStat, int* maxPoints)
{
    if (!g_book_ok.load()) return false;
    if (page)      *page      = g_book_page.load();
    if (indexStat) *indexStat = g_book_index.load();
    if (maxPoints) *maxPoints = g_book_max.load();
    return true;
}

int PlayerSpecial(int i)
{
    if (i < 0 || i >= 7 || !g_special_live_ok.load()) return -1;
    return g_special_live[i].load();
}

bool VatsTargetState(std::string* out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        unsigned before = g_vats_pick_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        game::VatsPick copy = g_vats_pick;
        if (g_vats_pick_seq.load(std::memory_order_acquire) != before) continue;
        if (!copy.sampled) return false;
        copy.target[sizeof(copy.target) - 1] = 0;
        out->assign(copy.target);
        return true;
    }
    return false;
}

bool VatsState(game::VatsInfo* out)
{
    if (!out || !g_vats_valid.load()) return false;
    out->ap = g_vats_ap.load();             out->max_ap = g_vats_maxap.load();
    out->clip_ammo = g_vats_clip.load();    out->reserve_ammo = g_vats_reserve.load();
    out->queued = g_vats_queued.load();     out->hit_chance = g_vats_chance.load();
    out->has_ap = g_vats_has_ap.load();     out->has_ammo = g_vats_has_ammo.load();
    out->has_chance = g_vats_has_chance.load();
    return true;
}


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

// Hold the aim on a reference. `frames` counts main-thread frames; pass a
// negative number to track until StopAimTrack(). Safe from any thread — the
// aiming itself happens in MainThreadWork.
// Put the aim on the target for this frame.
//
// This runs from the PLAYER MOVEMENT hook, not from the message-pump work
// queue, and that placement is the whole point. The message pump runs at the
// START of a frame, before the engine updates the player: everything written
// there was recomputed and thrown away before the crosshair was ever cast, so
// the aim was real for a fraction of a frame and gone by the time a shot left
// the barrel. The proof was in the log — sweeping the aim point through a
// 42-degree arc never changed what the engine reported under the crosshair,
// which cannot happen if the view is really moving. The movement hook runs
// inside the engine's own update, so what it writes is what the camera and the
// shot ray actually use.
//
// The FNV accessibility mod sidesteps this by running from NVSE's main-loop
// message; FOSE has no equivalent, so the movement hook is ours.
void AimTick()
{
    // Work the poll thread asked for: a new target's fresh start and the
    // player's trims. Done here because these counters are this thread's.
    if (g_aimtrack_reset_pending.exchange(false)) {
        g_aim_settle = kAimSettleFrames;   // let our own change bed in
        AimTrackReset();                   // a new target: search over
        g_hit_have = false;                // re-seed the health baseline
    }
    if (int n = g_nudge_units.exchange(0)) {
        g_aim_bias += (float)n;
        if (g_aim_bias >  400.0f) g_aim_bias =  400.0f;
        if (g_aim_bias < -400.0f) g_aim_bias = -400.0f;
        // Whatever the player settles on is kept: it corrects our estimate of
        // the eye height and of where a target's aim point sits, and neither
        // of those changes from one target to the next.
        g_aim_settled = true;
        F3A_INFO("AimTrim: correction now %+.0f units.", g_aim_bias);
    }

    // Has the view moved on its own since we set it last frame? Two things can
    // cause that, and both matter.
    //
    // A LOT of movement means somebody is steering with the mouse — and they
    // must be allowed to. A held aim rewrites the view every single frame, so
    // while it is on, the mouse is dead: a sighted player could not aim at all,
    // which is exactly the wrong way round for a mod whose whole purpose is to
    // let people play together. So the mouse wins: moving it releases the lock.
    //
    // A LITTLE movement is the engine quietly undoing us, which is worth a line
    // in the log but not a handover.
    {
        float dyaw = 0.0f, dpitch = 0.0f;
        if (g_aim_settle > 0) {
            --g_aim_settle;            // our own change is settling; not a mouse
            game::AimDriftSinceLast(&dyaw, &dpitch);   // consume, don't judge
        } else if (!config::Get().aim_mouse_takeover) {
            // Switched off: nothing is allowed to break the lock except the
            // player letting go of the aim. For a keyboard-only player there
            // is no mouse to hand control back to.
            game::AimDriftSinceLast(&dyaw, &dpitch);
        } else if (g_aimtrack_frames.load() != 0 &&
            game::AimDriftSinceLast(&dyaw, &dpitch)) {
            float moved = std::fabs(dyaw) > std::fabs(dpitch) ? std::fabs(dyaw)
                                                              : std::fabs(dpitch);
            if (moved > kMouseTakeover) {
                if (++g_takeover >= 2) {        // two frames: not a stray spike
                    g_takeover = 0;
                    StopAimTrack();
                    F3A_INFO("AimTrack: released — the view moved %+.3f yaw / "
                             "%+.3f pitch, so the mouse has it.", dyaw, dpitch);
                    g_aim_released.store(true);
                }
            } else {
                g_takeover = 0;
                if (moved > 0.03f) {
                    static DWORD s_last = 0;
                    DWORD now = GetTickCount();
                    if (now - s_last > 1000) {
                        s_last = now;
                        F3A_INFO("Aim: the engine moved the view %+.3f yaw / "
                                 "%+.3f pitch since we set it last frame.",
                                 dyaw, dpitch);
                    }
                }
            }
        } else {
            g_takeover = 0;
        }
    }

    // One line a second, while a target is held, with every value that decides
    // where the shot goes: whether the camera orbits independently (third
    // person), where the body points, and where the RENDERER thinks it points.
    // If the body faces the target and the camera does not, that is the answer.
    if (g_aimtrack_frames.load() != 0) {
        static DWORD s_last = 0;
        DWORD now = GetTickCount();
        if (now - s_last > 1000) {
            s_last = now;
            const float R2D = 57.2957795f;
            float camP = 0.0f, camY = 0.0f;
            bool haveCam = game::GetCameraAngles(&camP, &camY);
            auto* pl = fose_rt::Player();
            game::CrosshairTarget ct;
            bool haveCt = game::GetCrosshairTarget(&ct);
            // The player's own position and the range to the target go in
            // too: with those and a dump of what is nearby, the line of fire
            // can be reconstructed on paper — which is the one thing left to
            // check now that the aim itself is known to be steady and correct.
            float tx = BitsToF(g_aimtrack_x.load());
            float ty = BitsToF(g_aimtrack_y.load());
            float tz = BitsToF(g_aimtrack_z.load());
            float tdist = 0.0f;
            if (pl) {
                float ax = tx - pl->posX, ay = ty - pl->posY;
                tdist = std::sqrt(ax * ax + ay * ay);
            }
            F3A_INFO("AimState: 3rd=%d body yaw=%.1f pitch=%.1f | camera yaw=%.1f "
                     "pitch=%.1f (%s) | player=(%.0f,%.0f,%.0f) target=(%.0f,%.0f,%.0f) "
                     "range=%.0f trim=%+.0f | crosshair=%08X dist=%.0f",
                     game::IsThirdPerson() ? 1 : 0,
                     pl ? pl->rotZ * R2D : 0.0f, pl ? pl->rotX * R2D : 0.0f,
                     haveCam ? camY * R2D : 0.0f, haveCam ? camP * R2D : 0.0f,
                     haveCam ? "read" : "unavailable",
                     pl ? pl->posX : 0.0f, pl ? pl->posY : 0.0f, pl ? pl->posZ : 0.0f,
                     tx, ty, tz, tdist, g_aim_bias,
                     haveCt ? ct.refid : 0u, haveCt ? ct.dist : 0.0f);
        }
    }

    // Line of sight for the held-aim cue, measured HERE because this is the
    // game's own thread. The cue needs it ~20 times a second, and the module
    // that plays the tone runs on the polling thread, where casting an engine
    // ray crashes the game — a rule this project had already written down and
    // which this code broke anyway. So the ray is cast here and the answer is
    // published for the poller to read.
    if (uint32_t want = g_los_want.load()) {
        if (--g_los_every <= 0) {
            g_los_every = 3;
            uint32_t hit = 0;
            float    dist = 0.0f;
            bool ok = game::RayPickAhead(&hit, &dist);
            g_los_hit.store(ok ? hit : 0);
            g_los_dist.store(FToBits(ok ? dist : 0.0f));
            bool now_on = (hit == want);
            // A person or creature who is not an enemy is in the way: say so
            // before the player fires. Butch's mother died to four shots of
            // an aim that was on her; with the aim right, she can still step
            // in front of the radroach.
            if (ok && hit && hit != want && !game::IsKnownHostile(hit) &&
                !game::IsKnownDead(hit)) {
                g_fire_clear = 0;
                if (hit != g_fire_warned) {
                    bool isActor = false;
                    std::string nm = game::RefActorName(hit, &isActor);
                    if (isActor) {
                        g_fire_warned = hit;
                        F3A_INFO("AimLOS: %08X '%s' (not an enemy) is in the line of fire "
                                 "at %.0f units", hit, nm.c_str(), dist);
                        std::lock_guard<std::mutex> lk(g_fire_mx);
                        g_fire_name = nm.empty() ? std::string(strings::T("ktoś", "someone")) : nm;
                    }
                }
            } else if (g_fire_warned && ++g_fire_clear >= 10) {
                g_fire_warned = 0;          // out of the way for ~0.5 s: warn again next time
            }
            if (now_on != g_los_on || !g_los_said) {
                g_los_on = now_on;
                g_los_said = true;
                F3A_INFO("AimLOS: %s %08X (the ray hits %08X at %.0f units)",
                         g_los_on ? "ON" : "off", want, hit, dist);
            }
        }
    }

    // Confirm the held aim against the engine's own rays, and re-lock onto the
    // exact spot one of them reached.
    //
    // Everything else the mod knows about a target comes from the game's
    // RECORDS, which say where an object's origin is - and an origin can sit
    // inside the floor, inside a wall, or at a radroach's feet. That is the
    // difference between "the mod says it is aiming at the enemy" and a shot
    // that lands. A ray is the one measurement that answers the real question:
    // if a bullet went this way, what would it hit? So aim by geometry first to
    // bring the target into the sweep, then ask the rays and trust them over
    // our own arithmetic.
    if (g_rayrefine_id.load() != 0 && menu::ActiveMenu() != menu::Id::VATS) {
        if (--g_rayrefine_delay <= 0) {
            uint32_t want = g_rayrefine_id.exchange(0);
            // A narrow cone: the geometric aim has already pointed the view at
            // the target, so this only has to find exactly where on it a shot
            // can land, not search the room.
            auto hits = game::RayScanAhead(12.0f, 12.0f, 1.5f, 4000.0f);
            const game::RayHit* pick = nullptr;
            for (const auto& h : hits) if (h.refid == want) { pick = &h; break; }
            if (!pick) {
                F3A_INFO("AimRefine: no ray reached %08X (%u thing(s) in the "
                         "cone) - keeping the computed aim", want,
                         (unsigned)hits.size());
            } else {
                auto* pl = fose_rt::Player();
                game::Vec3 eye{};
                if (pl) {
                    eye = { pl->posX, pl->posY, pl->posZ };
                    game::GetCameraPos(&eye);
                }
                const float D2R = 0.01745329f;
                float yaw   = (pl ? pl->rotZ : 0.0f) + pick->yaw * D2R;
                float pitch = (pl ? pl->rotX : 0.0f) + pick->pitch * D2R;
                float cp = std::cos(pitch);
                game::Vec3 at{ eye.x + std::sin(yaw) * cp * pick->dist,
                               eye.y + std::cos(yaw) * cp * pick->dist,
                               eye.z - std::sin(pitch) * pick->dist };
                // Keep the REFERENCE, with the measured spot as its offset
                // (RequestAimTrack stores at - origin): the tracker then aims
                // at that spot on the target wherever it goes. Locking onto
                // the bare point instead is why a radroach could be hit once
                // and then never again — the first shot landed, the roach ran,
                // and the aim stayed on the floor where it had been. Only a
                // bare point when the ray reached some other part of the same
                // target we cannot follow (no live reference to hang it on).
                const void* tr = g_aimtrack_refr.load();
                uint32_t    tid = g_aimtrack_refid.load();
                int left = g_aimtrack_frames.load();   // the lock's own length
                game::Vec3 live{};
                if (left == 0)
                    ;                                   // the lock is already over
                else if (tr && tid == want && game::GetRefPosition(tr, tid, &live))
                    RequestAimTrack(tr, tid, at, 0.0f, left);
                else
                    RequestAimTrack(nullptr, 0, at, 0.0f, left);
                g_aim_solve.store(false);
                F3A_INFO("AimRefine: ray hit %08X '%s' at %.0f units, "
                         "%+.1f deg side, %+.1f deg up/down - locked on that spot",
                         pick->refid, pick->name.c_str(), pick->dist,
                         pick->yaw, pick->pitch);
            }
        }
    }

    // A requested ray map runs HERE, on the game's own thread. Casting these
    // from the polling thread took the whole game down.
    // "What can I actually shoot, and where is it?"
    //
    // Every other answer in this mod comes from the game's records, and those
    // record where an object's ORIGIN is — which can be inside a wall, behind
    // scenery, or nowhere near the part you are meant to hit. Rays report what
    // a bullet would really meet. This is the tool for a player who is aiming
    // correctly and still hitting nothing.
    if (g_rayscan.exchange(false)) {
        auto hits = game::RayScanAhead(45.0f, 25.0f, 2.5f, 4000.0f);
        F3A_INFO("RayScan: %u distinct things in front:", (unsigned)hits.size());
        std::string say;
        int spoken = 0;
        for (const auto& h : hits) {
            F3A_INFO("RayScan:   %08X '%s' %.0f units, %+.0f deg side, "
                     "%+.0f deg up/down, %d rays",
                     h.refid, h.name.c_str(), h.dist, h.yaw, h.pitch, h.samples);
            if (spoken >= 3) continue;
            // Skip the room shell: it swallows most of the rays and is never
            // what the player is looking for.
            if (h.samples > 200 && h.name.empty()) continue;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          strings::T("%s, %.0f jednostek, %.0f stopni %s. ",
                                     "%s, %.0f units, %.0f degrees %s. "),
                          h.name.empty() ? strings::T("obiekt", "object") : h.name.c_str(),
                          h.dist, std::fabs(h.yaw),
                          h.yaw < -1.0f ? strings::T("w lewo", "left")
                                        : (h.yaw > 1.0f ? strings::T("w prawo", "right")
                                                        : strings::T("na wprost", "ahead")));
            say += buf;
            ++spoken;
        }
        // Lock the aim onto the nearest thing that is NOT the room shell, so
        // the player can simply raise the weapon and fire at whatever is
        // actually there. Chasing a quest marker's coordinates is what failed;
        // this aims at something a bullet can reach, by construction.
        const game::RayHit* pick = nullptr;
        for (const auto& h : hits) {
            if (h.samples > 200 && h.name.empty()) continue;   // the room itself
            pick = &h;
            break;
        }
        if (pick) {
            auto* pl = fose_rt::Player();
            game::Vec3 eye{};
            if (pl) {
                eye = { pl->posX, pl->posY, pl->posZ };
                game::GetCameraPos(&eye);
            }
            const float D2R = 0.01745329f;
            float yaw   = (pl ? pl->rotZ : 0.0f) + pick->yaw * D2R;
            float pitch = (pl ? pl->rotX : 0.0f) + pick->pitch * D2R;
            float cp = std::cos(pitch);
            game::Vec3 at{ eye.x + std::sin(yaw) * cp * pick->dist,
                           eye.y + std::cos(yaw) * cp * pick->dist,
                           eye.z - std::sin(pitch) * pick->dist };
            // Deliberately WITHOUT the reference id.
            //
            // `at` is the exact spot on the object where a ray already landed —
            // the one point we know for certain a shot can reach. Passing the
            // reference too would make the tracker look the object up and aim
            // at its ORIGIN instead, which for these statics is buried inside
            // the geometry, and then re-solve its way to some other surface.
            // That is how a lock that was exactly right drifted to "nearly".
            RequestAimTrack(nullptr, 0, at, 0.0f, -1);
            g_aim_solve.store(false);   // nothing to solve: the ray already did
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          strings::T("Celuję w najbliższy obiekt: %.0f jednostek, %.0f stopni %s.",
                                     "Aiming at the nearest object: %.0f units, %.0f degrees %s."),
                          pick->dist, std::fabs(pick->yaw),
                          pick->yaw < -1.0f ? strings::T("w lewo", "left")
                                            : (pick->yaw > 1.0f ? strings::T("w prawo", "right")
                                                                : strings::T("na wprost", "ahead")));
            say += buf;
        }
        if (say.empty()) say = strings::T("Nic, w co dałoby się trafić.", "Nothing that could be hit.");
        tolk::Speak(say, tolk::Priority::Ui, true);
    }

    if (g_raymap.exchange(false)) {
        game::Vec3 c{ BitsToF(g_raymap_x.load()), BitsToF(g_raymap_y.load()),
                      BitsToF(g_raymap_z.load()) };
        game::RayMapAround(c);
    }

    int trackFrames = g_aimtrack_frames.load();
    // A held aim must not reach into menus, dialogue or a load screen: forcing
    // the view while the player is reading their Pip-Boy fights the game for no
    // benefit. Skipped, not cancelled — the lock resumes when play does.
    if (trackFrames != 0 && (!IsGameplayActive() || MenuModeActive())) trackFrames = 0;
    // Nor while auto-walk drives the player: both turned the body every frame
    // (Home pressed mid-walk, a lock with no end), and the walk zig-zagged.
    // A lock with no end is dropped; a timed one just waits.
    if (trackFrames != 0 && mover::Active()) {
        if (trackFrames < 0) g_aimtrack_frames.store(0);
        trackFrames = 0;
    }
    if (trackFrames != 0) {
        // Live reference position when there is one (it may be walking away),
        // otherwise the point we were given.
        game::Vec3 tp{};
        if (game::GetRefPosition(g_aimtrack_refr.load(),
                                 g_aimtrack_refid.load(), &tp)) {
            game::Vec3 origin = tp;
            // The live origin plus where on it we were asked to aim.
            tp.x += BitsToF(g_aimtrack_offx.load());
            tp.y += BitsToF(g_aimtrack_offy.load());
            tp.z += BitsToF(g_aimtrack_offz.load());
            game::Vec3 lead{};
            if (AimLead(g_aimtrack_refid.load(), origin, tp, &lead)) {
                tp.x += lead.x; tp.y += lead.y; tp.z += lead.z;
            }
        } else {
            AimLead(0, {}, {}, nullptr);       // nothing moving to lead
            tp.x = BitsToF(g_aimtrack_x.load());
            tp.y = BitsToF(g_aimtrack_y.load());
            tp.z = BitsToF(g_aimtrack_z.load());
        }
        // Apply the solved offset to wherever the target is NOW.
        tp.x += BitsToF(g_aim_ox.load());
        tp.y += BitsToF(g_aim_oy.load());
        tp.z += BitsToF(g_aim_oz.load());

        game::AimPlayerAtPoint(tp, BitsToF(g_aimtrack_up.load()), g_aim_bias);
        if (trackFrames > 0) g_aimtrack_frames.store(trackFrames - 1);

        if (g_aim_search_left > 0) --g_aim_search_left;

        // Did the target react? A rifle target turns, falls or is switched off
        // when it is struck, so ANY change to its placement is the hit we could
        // not otherwise see. This is the only honest feedback available: the
        // engine's crosshair reference does not follow the view (proved in the
        // log — the aim swung through 24 degrees and it never changed), and
        // FOSE has no hit event to listen to.
        {
            static const void* s_watch = nullptr;
            static float s_rx = 0.0f, s_ry = 0.0f, s_rz = 0.0f, s_pz = 0.0f;
            static uint32_t s_flags = 0;
            const void* r = g_aimtrack_refr.load();
            // Not for a person or a creature: they turn and step all the
            // time, and every frame of it was logged as "a hit".
            if (r && !IsBadReadPtr(r, 0x40) && !game::RefIsActor(r)) {
                auto* ref = reinterpret_cast<const TESObjectREFR*>(r);
                if (r != s_watch) {
                    s_watch = r;
                } else if (std::fabs(ref->rotX - s_rx) > 0.01f ||
                           std::fabs(ref->rotY - s_ry) > 0.01f ||
                           std::fabs(ref->rotZ - s_rz) > 0.01f ||
                           std::fabs(ref->posZ - s_pz) > 1.0f ||
                           ref->flags != s_flags) {
                    g_aim_reacted.store(true);
                    F3A_INFO("AimTrack: target %08X REACTED (rot %.3f/%.3f/%.3f "
                             "z %.1f flags %08X) — that is a hit.",
                             g_aimtrack_refid.load(), ref->rotX, ref->rotY,
                             ref->rotZ, ref->posZ, ref->flags);
                }
                s_rx = ref->rotX; s_ry = ref->rotY; s_rz = ref->rotZ;
                s_pz = ref->posZ; s_flags = ref->flags;
            }
        }

        // Did a shot land? There is no hit event to hook here, so the evidence
        // is the target's own health going down. Checked on this thread because
        // it calls into the engine, and only while a target is held — the
        // question is only interesting with a gun pointed at something.
        {
            const void* tr = g_hit_watch_refr.load();
            if (tr && --g_hit_every <= 0) {
                g_hit_every = 6;
                // Validate the pointer against the id it had when we started
                // watching: a cell change frees it and the address is reused.
                game::Vec3 ignore{};
                bool alive = game::GetRefPosition(tr, g_hit_watch_id.load(),
                                                  &ignore);
                if (!alive) { g_hit_watch_refr.store(nullptr); g_hit_have = false; }
                // Never return from here: this block sits inside AimTick, and
                // bailing out of the whole tick over a stale watch pointer
                // would also skip the per-frame re-aim below it.
                float hp = alive ? game::GetRefAV(tr, 16) : -1.0f;  // 16 = Health
                if (hp >= 0.0f) {
                    float prev = BitsToF(g_hit_last_hp.load());
                    // A drop, not a change: healing and the first reading must
                    // not sound like a hit.
                    if (g_hit_have && hp <= 0.0f && prev > 0.0f) {
                        // Health reached zero while we were watching. Not a
                        // guess and not an announcement of our own doing — the
                        // number came from the game.
                        g_kill_pending.store(true);
                        g_hit_watch_refr.store(nullptr);
                        g_hit_have = false;
                    }
                    if (g_hit_have && hp < prev - 0.5f) {
                        g_hit_pending.store(true);
                        F3A_INFO("AimHit: %08X health %.0f -> %.0f",
                                 g_aimtrack_refid.load(), prev, hp);
                    }
                    g_hit_last_hp.store(FToBits(hp));
                    g_hit_have = true;
                }
            }
        }

        // Once per target: ask the engine's ray where on this object we can
        // actually put a shot, and aim THERE from then on.
        if (g_aim_solve.exchange(false)) {
            game::Vec3 solved{};
            uint32_t want = g_aimtrack_refid.load();
            if (game::SolveAimPoint(tp, want, &solved)) {
                // Sideways only for things that stay put (see RequestAimTrack).
                bool still = !game::RefIsActor(g_aimtrack_refr.load());
                g_aim_ox.store(FToBits(still ? solved.x - tp.x : 0.0f));
                g_aim_oy.store(FToBits(still ? solved.y - tp.y : 0.0f));
                g_aim_oz.store(FToBits(solved.z - tp.z));
                // Only the offset is kept. The stored point stays the
                // reference's ORIGIN, so the correction is applied exactly once
                // — adding it to an already-corrected point would double it.
                F3A_INFO("AimSolve: %08X reachable at (%.0f,%.0f,%.0f), "
                         "%+.0f,%+.0f,%+.0f from its origin.", want,
                         solved.x, solved.y, solved.z,
                         solved.x - tp.x, solved.y - tp.y, solved.z - tp.z);
            } else {
                F3A_INFO("AimSolve: nothing on %08X could be reached by the "
                         "engine's ray — something is in the way.", want);
                // Say so, once, with numbers the player can act on. The solver
                // sweeps points around the TARGET, so when it comes back empty
                // the blockage is near the shooter instead — and no angle fixes
                // that. Standing there pulling the trigger cannot work, and
                // without being told, that is exactly what a blind player does.
                uint32_t hit = g_los_hit.load();
                float    hd  = BitsToF(g_los_dist.load());
                if (hit && hit != want && hd > 0.0f) {
                    // Where the obstacle is in the vertical, worked out from
                    // the ray we just cast: height above the player's feet.
                    float up = 0.0f;
                    if (auto* pl = fose_rt::Player()) {
                        game::Vec3 eye{ pl->posX, pl->posY, pl->posZ + 100.0f };
                        game::GetCameraPos(&eye);
                        float pitch = pl->rotX;          // radians, + = down
                        up = (eye.z - std::sin(pitch) * hd) - pl->posZ;
                    }
                    g_block_dist.store(FToBits(hd));
                    g_block_up.store(FToBits(up));
                    g_block_pending.store(true);
                }
            }
        }

        // KEEP THE VIEW ON IT. Nothing here used to write an angle at all: the
        // aim was taken once when the key went down, and everything after that
        // only watched. So when the solver worked out that the shot clears the
        // obstruction 19 units higher, that answer was recorded and then thrown
        // away — the view never moved to it. The New Vegas mod recomputes its
        // angles every sixth frame for the same reason.
        //
        // `tp` already carries the solved offset (added above), so this aims at
        // the point a ray proved reachable, not at the record's origin.
        if (--g_aim_reapply <= 0) {
            g_aim_reapply = 6;
            auto* pl = fose_rt::Player();
            if (pl) {
                game::Vec3 eye{ pl->posX, pl->posY, pl->posZ + 100.0f };
                game::GetCameraPos(&eye);
                float ax = tp.x - eye.x, ay = tp.y - eye.y;
                float az = (tp.z + BitsToF(g_aimtrack_up.load())) - eye.z;
                float horiz = std::sqrt(ax * ax + ay * ay);
                if (horiz > 1.0f) {
                    const float R2D = 57.2957795f;
                    float yawDeg   = std::atan2(ax, ay) * R2D;
                    float pitchUp  = std::atan2(az, horiz) * R2D;
                    game::SetPlayerAngleDeg('Z', yawDeg);
                    game::SetPlayerAngleDeg('X', -pitchUp);   // X: negative looks up
                }
            }
        }

        // How far the aim point is from the player, for the "is this the same
        // object" test below.
        float ourDist = 0.0f;
        if (auto* pl = fose_rt::Player()) {
            float ax = tp.x - pl->posX, ay = tp.y - pl->posY;
            float az = (tp.z + BitsToF(g_aimtrack_up.load())) - pl->posZ;
            ourDist = std::sqrt(ax * ax + ay * ay + az * az);
        }

        // Is the crosshair actually on it? The engine's own pick answers that,
        // and it is the only honest measure of whether we are aiming or only
        // claiming to.
        uint32_t want = g_aimtrack_refid.load();
        if (want && --g_aim_check <= 0) {
            g_aim_check = kAimCheckEvery;
            // Asked with a RAY along the view, not with the crosshair field.
            // That field was measured holding one value through a 21-degree
            // swing of the aim, so it answers a different question than "what
            // am I pointing at" — and every probe decision made from it was
            // being made on noise.
            //
            // Same-RANGE still counts as the same thing: a rifle target is
            // built from a stand, a board and the activator that scores the
            // hit, and demanding the exact reference meant one could never
            // confirm. As far as a bullet is concerned they are one object.
            uint32_t hitId = 0;
            float    hitDist = 0.0f;
            bool     have = game::RayPickAhead(&hitId, &hitDist);
            bool on = have &&
                      (hitId == want ||
                       (hitDist > 0.0f && ourDist > 0.0f &&
                        std::fabs(hitDist - ourDist) <= kSameAssembly));
            if (on) {
                g_aim_miss = 0;
                if (!g_aim_settled) {
                    g_aim_settled = true;
                    F3A_INFO("AimTrack: crosshair ON target after %d probe(s), "
                             "aim-point correction %+.0f units.",
                             g_aim_probe, g_aim_bias);
                }
                // No tone here. There used to be one, firing once every 30
                // checks, and it was the "beeped twice" the player heard —
                // sparse enough to read as a glitch, and a second opinion
                // competing with the held-aim cue. One signal, in one place:
                // world_scan plays it from the line-of-sight answer, for as
                // long as the answer stays true.
            } else {
                g_aim_tone_ticks = 0;     // re-arm, so re-acquiring sounds again
                if (g_aim_miss == 0)
                    F3A_INFO("AimTrack: the ray hits %08X at %.0f units, want "
                             "%08X at %.0f — probing (correction %+.0f units).",
                             hitId, hitDist, want, ourDist, g_aim_bias);
            }
            // Searching happens ONLY during the brief window after the aim
            // key centres on a target — never while the weapon is raised and
            // held (frames < 0). Sweeping the aim point through ±96 units
            // takes well over a second, and doing that under a player who is
            // pulling the trigger throws their shots away. By then the
            // correction has already been found; hold it still and shoot.
            // The sweep is DISABLED while the crosshair reading is in doubt.
            // In the last session the engine reported the same reference under
            // the crosshair through a 42-degree swing of the aim, which either
            // means the view is not moving or means this field is not a live
            // pick — and until that is settled, letting it steer the aim only
            // wobbles the gun under a player trying to shoot. The check below
            // still runs, so the log keeps telling us what it sees.
            const bool searching = false && (g_aim_search_left > 0);
            const int  needed    = g_aim_settled ? 3 : 1;
            if (searching && !on && ++g_aim_miss >= needed) {
                // Off target for a while — resume the sweep.
                g_aim_settled = false;
                if (g_aim_probe < kAimProbeMax) {
                    ++g_aim_probe;
                    g_aim_bias = ProbeBias(g_aim_probe);
                } else if (g_aim_probe == kAimProbeMax) {
                    ++g_aim_probe;          // report once, then hold still
                    g_aim_bias = 0.0f;
                    F3A_INFO("AimTrack: swept +-%.0f units without the crosshair "
                             "ever reporting refid %08X — either it is out of "
                             "the engine's pick range or it is not pickable at "
                             "all; holding the plain geometric aim.",
                             kAimProbeMax / 2 * kAimProbeStep, want);
                }
                g_aim_miss = 0;
            }
        }
    }
}

void RequestAimTrack(const void* refr, uint32_t refid, const game::Vec3& at,
                     float up, int frames)
{
    g_aimtrack_refr.store(refr);
    g_aimtrack_refid.store(refid);
    // Only a call that actually names a reference moves the damage watch. The
    // ray refinement re-locks onto a bare POINT with no reference, and must
    // leave the watch where it is — see g_hit_watch_refr.
    if (refr && refid) {
        g_hit_watch_refr.store(refr);
        g_hit_watch_id.store(refid);
        g_kill_pending.store(false);
    }
    // Remember where on the reference the point lies (see g_aimtrack_offx).
    // Until now the held aim re-aimed at the bare origin every frame, so a
    // lock asked for the middle of a target board turned ~4 degrees off it,
    // onto the board's base — and onto an enemy's feet.
    {
        game::Vec3 origin{};
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        if (refr && refid && game::GetRefPosition(refr, refid, &origin)) {
            ox = at.x - origin.x; oy = at.y - origin.y; oz = at.z - origin.z;
            if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) ||
                !(ox * ox + oy * oy + oz * oz <= 1000.0f * 1000.0f)) {
                ox = oy = oz = 0.0f;       // implausible: aim at the origin
            }
            // A person or creature turns and runs: a sideways offset kept in
            // world terms points beside it as soon as it moves (a ray that
            // touched a radroach's flank stayed 15 units to that side while
            // the roach circled). Their middle is over the origin, so only the
            // height is kept.
            if (game::RefIsActor(refr)) ox = oy = 0.0f;
        }
        g_aimtrack_offx.store(FToBits(ox));
        g_aimtrack_offy.store(FToBits(oy));
        g_aimtrack_offz.store(FToBits(oz));
    }
    g_aimtrack_x.store(FToBits(at.x));
    g_aimtrack_y.store(FToBits(at.y));
    g_aimtrack_z.store(FToBits(at.z));
    g_aimtrack_up.store(FToBits(up));
    // The search counters, settle window and health baseline are the main
    // thread's; from the poll thread (the aim keys) they are reset there, on
    // the next AimTick, instead of written from here under its feet.
    if (game::OnMainThread()) {
        g_aim_settle = kAimSettleFrames;   // let our own change bed in
        AimTrackReset();   // a new target: start the crosshair search over
        g_hit_have = false;
    } else {
        g_aimtrack_reset_pending.store(true);
    }
    g_aimtrack_frames.store(frames);
    g_hit_pending.store(false);
    g_block_pending.store(false);
    F3A_INFO("AimTrack: refr=%p refid=%08X at=(%.0f,%.0f,%.0f) up=%.0f frames=%d",
             refr, refid, at.x, at.y, at.z, up, frames);
}

void StopAimTrack() { g_aimtrack_frames.store(0); }

float NudgeAim(float units)
{
    // Applied by AimTick on the main thread, which owns the bias; the value
    // returned is what it will be once that has happened.
    int add = (int)std::lround(units);
    int pending = g_nudge_units.fetch_add(add) + add;
    float total = g_aim_bias + (float)pending;
    if (total >  400.0f) total =  400.0f;
    if (total < -400.0f) total = -400.0f;
    return total;
}

float AimCorrection() { return g_aim_bias; }

void RequestRayAimRefine(uint32_t refid)
{
    // Logged on the way IN as well: the last run produced no AimRefine line at
    // all, and without this there is no way to tell "the request never arrived"
    // from "the rays found nothing".
    F3A_INFO("AimRefine: requested for %08X", refid);
    g_rayrefine_delay = 8;      // let the computed aim settle first
    g_rayrefine_id.store(refid);
}

void WatchAimLos(uint32_t refid)
{
    if (refid == 0) { g_los_hit.store(0); g_los_dist.store(0); g_los_on = false; }
    g_los_said = false;
    g_los_want.store(refid);
}

void NoteVoiceLine(size_t chars)
{
    DWORD until = GetTickCount() + 500 + (DWORD)(chars * 70);
    DWORD cur = g_voice_quiet_at.load();
    if (until > cur) g_voice_quiet_at.store(until);
}

DWORD VoiceQuietAt() { return g_voice_quiet_at.load(); }

void NoteGameLoad() { g_game_load_pending.store(true); }

void WatchQuestStages(const std::vector<uint32_t>& ids)
{
    std::lock_guard<std::mutex> lk(g_qs_mx);
    g_qs = ids;
}

bool QuestStage(uint32_t id, int* stage, bool* running)
{
    // Read live, as plain data (game::ReadQuestState): sampling on the game's
    // thread went quiet in the birth scene - nothing sends the game a window
    // message while the controls are taken away - and the descriptions came
    // half a minute late, over the father's first words.
    return game::ReadQuestState(id, stage, running);
}

bool TakeLineOfFireWarning(std::string* name)
{
    std::lock_guard<std::mutex> lk(g_fire_mx);
    if (g_fire_name.empty()) return false;
    if (name) *name = g_fire_name;
    g_fire_name.clear();
    return true;
}

bool AimLosHit(uint32_t* outHit, float* outDist)
{
    if (outHit)  *outHit  = g_los_hit.load();
    if (outDist) *outDist = BitsToF(g_los_dist.load());
    return g_los_want.load() != 0;
}

void RequestRayScan() { g_rayscan.store(true); }

void RequestRayMap(const game::Vec3& centre)
{
    g_raymap_x.store(FToBits(centre.x));
    g_raymap_y.store(FToBits(centre.y));
    g_raymap_z.store(FToBits(centre.z));
    g_raymap.store(true);
}

bool ConsumeAimTargetReacted()  { return g_aim_reacted.exchange(false); }
bool ConsumeTargetHit()         { return g_hit_pending.exchange(false); }
bool ConsumeTargetDown()        { return g_kill_pending.exchange(false); }

bool ConsumeNoLineOfFire(float* outDist, float* outHeight)
{
    if (!g_block_pending.exchange(false)) return false;
    if (outDist)   *outDist   = BitsToF(g_block_dist.load());
    if (outHeight) *outHeight = BitsToF(g_block_up.load());
    return true;
}
bool ConsumeAimReleased()       { return g_aim_released.exchange(false); }

bool AimTrackActive() { return g_aimtrack_frames.load() != 0; }

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
bool MenuModeActive()   { return g_menu_mode.load(); }
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
    // The quantity prompt and VATS are freed by a key press on the main
    // thread; walking their tiles from here is a known crash.
    if (menu::ActiveMenu() == menu::Id::Quantity || menu::IsOpen(menu::Id::VATS)) {
        tolk::Speak(strings::T("Zrzut niedostępny w tym ekranie.", "Dump not available on this screen."),
                    tolk::Priority::System, true);
        return;
    }
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
    tolk::Speak(strings::T("Zrzut zapisany w pliku dump.", "Dump saved to the dump file."),
                tolk::Priority::System, true);
}

} // namespace f3a::poll
