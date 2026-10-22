#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/logger.h"

#include <string>

namespace f3a::modules::sleepwait {
namespace {

// Sleep / wait: the question, the time now and the hours chosen when it opens,
// then each new number of hours as the arrows change it (the FNV mod reads the
// same three texts). The game sets them in code, so they are read off the
// screen, never worked out here.
std::string g_hours;
bool        g_opened = false;

void OnOpen()  { g_hours.clear(); g_opened = false; }
void OnClose() { g_hours.clear(); g_opened = false; }

void OnTick(float)
{
    game::SleepWaitInfo w;
    if (!game::GetSleepWaitInfo(&w)) return;
    if (!g_opened) {
        if (w.hours.empty()) return;            // the screen is still being set up
        g_opened = true;
        g_hours = w.hours;
        std::string s = w.prompt;
        if (!w.now.empty()) s += (s.empty() ? "" : ". ") + w.now;
        s += (s.empty() ? "" : ". ") + w.hours;
        s += strings::T(". Strzałki zmieniają liczbę godzin, W czeka, E anuluje.",
                        ". The arrows change the hours, W waits, E cancels.");
        tolk::Speak(s, tolk::Priority::Ui, true);
        return;
    }
    if (w.hours.empty() || w.hours == g_hours) return;
    g_hours = w.hours;
    tolk::Speak(w.hours, tolk::Priority::Ui, true);
}

} // namespace

// H here: the time now and the hours chosen.
bool SpeakSummary()
{
    if (menu::ActiveMenu() != menu::Id::SleepWait) return false;
    game::SleepWaitInfo w;
    if (!game::GetSleepWaitInfo(&w)) return false;
    std::string s = w.now;
    if (!w.hours.empty()) s += (s.empty() ? "" : ". ") + w.hours;
    tolk::Speak(s, tolk::Priority::Ui, true);
    return true;
}

void Init()
{
    menu::RegisterMenu(menu::Id::SleepWait, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Sleep/wait module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::sleepwait
