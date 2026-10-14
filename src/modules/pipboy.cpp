#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <cstdio>
#include <string>

namespace f3a::modules::pipboy {
namespace {

std::string g_last_selection;
std::string g_last_tab;
std::string g_last_subtab;

// mute = work out what WOULD be said and remember it, without saying it. Used
// right after a page switch so the page's first row does not get read on top
// of the page name, while still counting as "already announced" so the next
// real move of the selection speaks normally.
void AnnounceSelection(bool force, bool mute = false)
{
    auto item = game::GetSelectedInventoryItem();
    if (!item) {
        auto text = game::GetActiveMenuSelectionText();
        if (!text || text->empty()) return;
        if (!force && *text == g_last_selection) return;
        g_last_selection = *text;
        if (!mute) tolk::Speak(*text, tolk::Priority::Ui, true);
        return;
    }

    char extra[160] = {0};
    const auto& cfg = config::Get();
    int extra_len = 0;

    if (cfg.read_item_weight && item->weight > 0.01f) {
        char wbuf[32]; std::snprintf(wbuf, sizeof(wbuf), "%.1f", item->weight);
        extra_len += std::snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                   "%s%s",
                                   extra_len ? ", " : "",
                                   strings::RenderArgs(strings::Key::ItemWeightFmt, wbuf).c_str());
    }
    if (cfg.read_item_value && item->value_caps > 0) {
        char vbuf[16]; std::snprintf(vbuf, sizeof(vbuf), "%d", item->value_caps);
        extra_len += std::snprintf(extra + extra_len, sizeof(extra) - extra_len,
                                   "%s%s",
                                   extra_len ? ", " : "",
                                   strings::RenderArgs(strings::Key::ItemValueFmt, vbuf).c_str());
    }

    std::string line = strings::RenderArgs(strings::Key::SelectionFmt,
                                           item->name.c_str(), extra);
    if (!force && line == g_last_selection) return;
    g_last_selection = line;
    if (!mute) tolk::Speak(line, tolk::Priority::Ui, true);
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
void AnnounceSubTabIfChanged(bool queue_after_tab)
{
    std::string sub = game::GetActivePipBoySubTabName();
    if (sub.empty() || sub == g_last_subtab) return;

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
    if (fresh.empty()) return;
    // Don't interrupt the page announcement when both change at once.
    tolk::Speak(fresh, tolk::Priority::Ui, !queue_after_tab);
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

void OnTick(float)
{
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
    AnnounceSubTabIfChanged(false);
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
