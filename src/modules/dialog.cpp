#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <cstdio>
#include <string>

namespace f3a::modules::dialog {
namespace {

int         g_last_highlight = -2;
std::string g_last_line;

void AnnounceCurrentLine()
{
    std::string line = game::GetCurrentDialogLine();
    std::string who  = game::GetCurrentDialogSpeaker();
    if (line.empty() || line == g_last_line) return;
    g_last_line = line;
    tolk::Speak(
        strings::RenderArgs(strings::Key::NpcSpeechFmt,
                            who.empty() ? "?" : who.c_str(),
                            line.c_str()),
        tolk::Priority::Dialog, true);
}

void AnnounceHighlight()
{
    int idx = game::GetHighlightedDialogOption();
    if (idx == g_last_highlight) return;
    g_last_highlight = idx;

    auto opts = game::GetDialogOptions();
    if (opts.empty()) {
        tolk::Speak(strings::Render(strings::Key::DialogNoOptions),
                    tolk::Priority::Ui, true);
        return;
    }
    if (idx < 0 || idx >= (int)opts.size()) return;

    const auto& o = opts[idx];
    char idx_buf[8]; std::snprintf(idx_buf, sizeof(idx_buf), "%d", o.index);
    tolk::Speak(
        strings::RenderArgs(strings::Key::DialogOptionFmt,
                            idx_buf,
                            o.text.c_str(),
                            o.skill_req.c_str()),
        tolk::Priority::Ui, true);
}

void OnOpen()
{
    g_last_line.clear();
    g_last_highlight = -2;
}

void OnClose()
{
    g_last_line.clear();
    g_last_highlight = -2;
}

void OnTick(float)
{
    AnnounceCurrentLine();
    AnnounceHighlight();
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Dialog, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Dialog module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::dialog
