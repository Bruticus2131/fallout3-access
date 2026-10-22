#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <string>

namespace f3a::modules::dialog {
namespace {

// A conversation, as FNV's mod has it: who it is with when it opens, and the
// new speaker's name when someone else takes over (the Megaton sheriff and
// his son, a group in the vault). The topics are read by the keyboard
// selection reader with their place in the list and "already said"; the
// NPC's line is voiced by the game, so it is not read over the voice - D
// repeats it (world_scan's item-info key, see DialogLine).
std::string g_speaker;      // the name last said
bool        g_opened = false;
bool        g_hint_said = false;   // once per session: what D does here

void OnOpen()
{
    g_speaker.clear();
    g_opened = false;
}

void OnClose()
{
    g_speaker.clear();
    g_opened = false;
}

void OnTick(float)
{
    std::string who = game::GetCurrentDialogSpeaker();
    if (who.empty() || who == g_speaker) return;
    g_speaker = who;
    if (!g_opened) {
        g_opened = true;
        std::string line = strings::T("Rozmowa z ", "Talking to ") + who;
        if (!g_hint_said) {
            g_hint_said = true;
            line += strings::T(". D powtarza ostatnią kwestię.", ". D repeats the last line.");
        }
        F3A_INFO("Dialog with '%s'", who.c_str());
        tolk::Speak(line, tolk::Priority::Ui, true);
        return;
    }
    tolk::Speak(who, tolk::Priority::Ui, false);   // someone else speaks now
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::Dialog, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Dialog module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::dialog
