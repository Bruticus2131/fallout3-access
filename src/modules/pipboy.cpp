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

void AnnounceSelection(bool force)
{
    auto item = game::GetSelectedInventoryItem();
    if (!item) {
        auto text = game::GetActiveMenuSelectionText();
        if (!text || text->empty()) return;
        if (!force && *text == g_last_selection) return;
        g_last_selection = *text;
        tolk::Speak(*text, tolk::Priority::Ui, true);
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
    tolk::Speak(line, tolk::Priority::Ui, true);
}

void AnnounceTabIfChanged()
{
    std::string tab = game::GetActivePipBoyTabName();
    if (tab.empty() || tab == g_last_tab) return;
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
}

void OnOpen()
{
    g_last_selection.clear();
    g_last_tab.clear();
    AnnounceTabIfChanged();
    AnnounceSelection(true);
}

void OnClose()
{
    g_last_selection.clear();
    g_last_tab.clear();
}

void OnTick(float)
{
    AnnounceTabIfChanged();
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
