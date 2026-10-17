#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"
#include "f3a/strings.h"

#include <string>

namespace f3a::modules::levelup {
namespace {

// The level-up screen, read out the way the FNV mod does it, in the game's own
// words: the headline and the points counter when it opens, the page's
// counter when the page changes (skills, then perks), the entry the keyboard
// is on with its value, then that entry's description; and when a value
// moves, the new value with the counter. Before this the screen read only the
// row ("Handel, 19") — never a description, nor how many points were left.
bool        g_said_open = false;
int         g_page = -1;            // 0 skills, 1 perks
const void* g_focus = nullptr;
std::string g_label, g_value, g_counter, g_desc;

void OnOpen()
{
    g_said_open = false;
    g_page = -1;
    g_focus = nullptr;
    g_label.clear(); g_value.clear(); g_counter.clear(); g_desc.clear();
}

void OnClose() { OnOpen(); }

void OnTick(float)
{
    game::LevelUpState st;
    if (!game::GetLevelUpState(&st)) return;

    bool queue = false;                  // something was just said: follow it
    if (!g_said_open) {
        g_said_open = true;
        std::string say = st.headline;
        if (!st.counter.empty()) say += (say.empty() ? "" : ". ") + st.counter;
        if (!say.empty()) { tolk::Speak(say, tolk::Priority::Ui, true); queue = true; }
        g_page = st.perks ? 1 : 0;
        g_counter = st.counter;
    }
    if ((st.perks ? 1 : 0) != g_page) {
        g_page = st.perks ? 1 : 0;
        g_focus = nullptr;
        g_desc.clear();
        g_counter = st.counter;
        if (!st.counter.empty()) { tolk::Speak(st.counter, tolk::Priority::Ui, true); queue = true; }
    }

    if (st.focus && (st.focus != g_focus || st.label != g_label)) {
        g_focus = st.focus;
        g_label = st.label;
        g_value = st.value;
        g_desc.clear();
        std::string say = st.label;
        if (!st.value.empty()) say += ", " + st.value;
        if (st.chosen) say += strings::T(", wybrane", ", chosen");
        if (st.dimmed) say += strings::T(", niedostępne", ", unavailable");
        if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, !queue);
        F3A_DEBUG("LevelUp: focus '%s'", say.c_str());
        return;                          // the description follows when it updates
    }

    if (st.focus && st.value != g_value) {
        g_value = st.value;
        std::string say = st.value;
        if (st.counter != g_counter && !st.counter.empty())
            say += (say.empty() ? "" : ". ") + st.counter;
        g_counter = st.counter;
        if (!say.empty()) tolk::Speak(say, tolk::Priority::Ui, true);
        return;
    }
    if (st.counter != g_counter) {
        g_counter = st.counter;
        if (!st.counter.empty()) tolk::Speak(st.counter, tolk::Priority::Ui, false);
    }
    if (!st.button && !st.description.empty() && st.description != g_desc) {
        g_desc = st.description;
        tolk::Speak(st.description, tolk::Priority::Ui, false);
    }
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::LevelUp, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("Level-up module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::levelup
