#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdio>
#include <optional>
#include <string>

namespace f3a::modules::pipboy {
namespace {

std::string g_last_selection;
std::string g_last_tab;
std::string g_last_subtab;
std::string g_last_kbd;      // the keyboard highlight's row, as last seen
std::string g_last_act;      // the mouse's (activeTile) row, as last seen

// What a sighted player reads beside the list, said after the row: on the
// Items page the item card (damage, weight, value, condition, ammo, effects),
// on the Stats page the description of the selected S.P.E.C.I.A.L., skill or
// perk, or the open Status view (limbs, radiation, effects). The game fills it
// in as the highlight lands, so it is read a moment later - and only if the
// highlight is still on that row.
std::string g_card_row;      // the row the card is due for
DWORD       g_card_due = 0;
bool        g_card_stats = false;   // the Stats page's detail, not an item card
constexpr DWORD kCardDelayMs = 150;

bool ItemsPage()
{
    return menu::IsOpen(menu::Id::Inventory) && !menu::IsOpen(menu::Id::Stats) &&
           !menu::IsOpen(menu::Id::Map);
}

bool StatsPage()
{
    return menu::IsOpen(menu::Id::Stats) && !menu::IsOpen(menu::Id::Inventory) &&
           !menu::IsOpen(menu::Id::Map);
}

void ScheduleCard(const std::string& row)
{
    if (!config::Get().item_card) { g_card_due = 0; return; }   // [Voice] ItemCard = 0
    if (ItemsPage())      g_card_stats = false;
    else if (StatsPage()) g_card_stats = true;
    else { g_card_due = 0; return; }
    g_card_row = row;
    g_card_due = GetTickCount() + kCardDelayMs;
}

// The row the player is on. The arrows move the list's highlight box, and in
// the Pip-Boy they often leave activeTile empty - or on the row the hidden
// mouse pointer happens to rest on - so reading activeTile alone went silent
// "sometimes" (on the item categories especially). Whichever of the two moved
// since the last look wins; `force` takes the keyboard's row, else the mouse's.
std::optional<std::string> CurrentRow(bool force)
{
    auto kbd = game::GetKeyboardSelectionText();
    auto act = game::GetActiveMenuSelectionText();
    std::string k = kbd ? *kbd : std::string();
    std::string a = act ? *act : std::string();
    bool kMoved = !k.empty() && k != g_last_kbd;
    bool aMoved = !a.empty() && a != g_last_act;
    g_last_kbd = k;
    g_last_act = a;
    if (force) {
        if (!k.empty()) return k;
        if (!a.empty()) return a;
        return std::nullopt;
    }
    if (kMoved) return k;
    if (aMoved) return a;
    return std::nullopt;
}

// A quest in the Data page's quest list: whether it is the tracked one, as a
// screen reader says a check box's state.
std::string QuestState(const std::string& row)
{
    if (!menu::IsOpen(menu::Id::Map)) return {};
    std::string tracked = game::GetTrackedQuestName();
    if (!tracked.empty() && row == tracked) return strings::T(", śledzone", ", tracked");
    for (const auto& n : game::RunningQuestNames())
        if (n == row) return strings::T(", nieśledzone", ", not tracked");
    for (const auto& n : game::CompletedQuestNames())
        if (n == row) return strings::T(", ukończone", ", completed");
    return {};
}

// mute = work out what WOULD be said and remember it, without saying it. Used
// right after a page switch so the page's first row does not get read on top
// of the page name, while still counting as "already announced" so the next
// real move of the selection speaks normally.
void AnnounceSelection(bool force, bool mute = false, bool interrupt = true)
{
    auto text = CurrentRow(force);
    if (!text || text->empty()) return;
    if (!force && *text == g_last_selection) return;
    g_last_selection = *text;
    if (!mute) {
        tolk::Speak(*text + QuestState(*text), tolk::Priority::Ui, interrupt);
        ScheduleCard(*text);
    }
}

bool AnnounceTabIfChanged()
{
    std::string tab = game::GetActivePipBoyTabName();
    if (tab.empty() || tab == g_last_tab) return false;
    bool first_time = g_last_tab.empty();
    g_last_tab = tab;
    // First announcement after Pip-Boy opens gets the "Pip-Boy: " prefix
    // so the user has context. Subsequent tab switches are just the tab.
    if (first_time) {
        std::string line = "Pip-Boy, " + tab;
        tolk::Speak(line, tolk::Priority::Ui, true);
    } else {
        tolk::Speak(tab, tolk::Priority::Ui, true);
    }
    return true;
}

// Announce the active sub-tab strip selection (Status / SPECIAL / Skills...,
// KND / RAD / SKT, Weapons / Apparel / ... on the Items page) on change.
// A page has several tab strips at once (tailline + the KND/RAD/SKT view
// switch), so on change we voice ONLY the parts that weren't active before —
// e.g. switching the view says just "RAD", not "Status, RAD" again.
bool AnnounceSubTabIfChanged(bool queue_after_tab)
{
    std::string sub = game::GetActivePipBoySubTabName();
    if (sub.empty() || sub == g_last_subtab) return false;

    std::string fresh;
    size_t start = 0;
    while (start <= sub.size()) {
        size_t end = sub.find(", ", start);
        std::string part = (end == std::string::npos)
                               ? sub.substr(start)
                               : sub.substr(start, end - start);
        if (!part.empty() &&
            g_last_subtab.find(part) == std::string::npos) {
            if (!fresh.empty()) fresh += ", ";
            fresh += part;
        }
        if (end == std::string::npos) break;
        start = end + 2;
    }
    g_last_subtab = sub;
    if (fresh.empty()) return false;
    // Don't interrupt the page announcement when both change at once.
    tolk::Speak(fresh, tolk::Priority::Ui, !queue_after_tab);
    return true;
}

// How many Pip-Boy pages are up right now. Switching page OPENS the new page
// before it CLOSES the old one, so for a moment two are open — and treating
// each open as "the Pip-Boy just came up" and each close as "it went away" is
// what produced "Przedmioty, Pip-Boy, Przedmioty, Ubiór, Pip-Boy, Przedmioty"
// for one press: the old page said the page, the new one said it again with
// the prefix and the sub-tab, and the old page's close wiped the memory so
// the new page said it all a third time. (The dispatcher lists a menu as open
// already in its open handler and still in its close handler.)
int OpenPages()
{
    int n = 0;
    for (auto id : { menu::Id::PipBoy, menu::Id::Inventory, menu::Id::Stats,
                     menu::Id::Map })
        if (menu::IsOpen(id)) ++n;
    return n;
}

void OnOpen()
{
    if (OpenPages() >= 2) {
        // A page switch, not the Pip-Boy opening: the page name once (unless
        // the old page already said it), nothing else — as OnTick does.
        AnnounceTabIfChanged();
        g_last_subtab = game::GetActivePipBoySubTabName();
        AnnounceSelection(/*force*/true, /*mute*/true);
        return;
    }
    g_last_selection.clear();
    g_last_tab.clear();
    g_last_subtab.clear();
    g_last_kbd.clear();
    g_last_act.clear();
    g_card_due = 0;
    AnnounceTabIfChanged();
    AnnounceSubTabIfChanged(/*queue_after_tab=*/true);
    // On the Stats page read the vitals once: level, HP, AP, XP.
    std::string vitals = game::GetPipBoyVitals();
    if (!vitals.empty()) {
        tolk::Speak(vitals, tolk::Priority::Ui, false);  // queue, don't cut
    }
    AnnounceSelection(true);
}

void OnClose()
{
    if (OpenPages() >= 2) return;   // another page is taking over: keep the memory
    g_last_selection.clear();
    g_last_tab.clear();
    g_last_subtab.clear();
}

// The card for the row the name was just said for.
void CardTick()
{
    if (!g_card_due || GetTickCount() < g_card_due) return;
    g_card_due = 0;
    if (g_card_row != g_last_selection) return;                    // moved on
    if (g_card_stats ? !StatsPage() : !ItemsPage()) return;        // page switched
    std::string card = g_card_stats ? game::GetStatsDetail() : game::GetItemCard();
    if (!card.empty()) tolk::Speak(card, tolk::Priority::Ui, false);   // after the name
}

// The hotkey wheel, while it is open: which key the selection is on and what
// it holds, on every change.
void HotkeyWheelTick()
{
    static int s_slot = -2;
    static std::string s_text;
    int slot = -1;
    std::string text;
    if (!ItemsPage() || !game::GetHotkeyWheel(&slot, &text)) { s_slot = -2; s_text.clear(); return; }
    if (slot == s_slot && text == s_text) return;
    const bool opened = s_slot == -2;
    s_slot = slot; s_text = text;
    std::string say;
    if (opened) say = strings::T("Szybki wybór. ", "Hotkeys. ");
    if (slot >= 0) say += strings::T("Klawisz ", "Key ") + std::to_string(slot + 1);
    if (!text.empty()) say += (say.empty() || say.back() == ' ' ? "" : ": ") + text;
    if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, true);
}

void OnTick(float)
{
    CardTick();
    HotkeyWheelTick();
    // Switching page with F1/F2/F3 says the PAGE, full stop.
    //
    // It used to say three things at once: the page, then the sub-tab strip it
    // landed on, then whatever row happened to be selected there. Pressing F1,
    // F2, F3 in a row — which is how you look for something — buried the one
    // word you were listening for under two you were not. The other two are
    // still CAPTURED, so they are treated as already announced and the next
    // real move of the selection reads normally.
    if (AnnounceTabIfChanged()) {
        g_last_subtab = game::GetActivePipBoySubTabName();
        AnnounceSelection(/*force*/true, /*mute*/true);
        return;
    }
    // A new tab (Status, General, Chems, Weapons...): its name, and the row
    // it lands on after it - queued, so the name is not cut off by the row.
    if (AnnounceSubTabIfChanged(false)) {
        AnnounceSelection(/*force*/true, /*mute*/false, /*interrupt*/false);
        // Status's CND / RAD / EFF have no list row of their own to hang the
        // view on: the switch itself brings it.
        if (StatsPage() && g_card_due == 0) ScheduleCard(g_last_selection);
        return;
    }
    AnnounceSelection(false);
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::PipBoy, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Inventory, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Stats, &OnOpen, &OnClose, &OnTick);
    menu::RegisterMenu(menu::Id::Map, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("PipBoy module ready.");
}

void Shutdown() {}

} // namespace f3a::modules::pipboy
