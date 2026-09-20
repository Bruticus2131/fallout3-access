#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"
#include "f3a/polling_loop.h"
#include "f3a/strings.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"

#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>

namespace f3a::modules::mapnav {
namespace {

// Reading the world map the way a sighted player does: the map screen draws a
// marker per known location, so we read that same list and let the player walk
// it with Page Up / Page Down, hearing each location's name, distance and
// bearing. Enter travels there; Home makes it the walking destination.
//
// The marker list also gets cached for gameplay use — that's what lets a quest
// target in ANOTHER worldspace be reached: travel to the nearest discovered
// marker, then walk the rest (see autowalk).
std::vector<game::MapMarker> g_markers;
int         g_index = -1;
bool        g_open  = false;
// Pip-Boy Quests tab: the generic list reader speaks the quest NAME when the
// selection moves, but the objectives — the part that says what to actually do —
// sit in a separate list. Read them whenever they change.
std::string g_last_objectives;
std::string g_confirm_pending;    // name awaiting a second Enter (undiscovered)

// Fast travel runs entirely through the game's own two-step path: select the
// marker, then press the map's travel button. Both are HandleClick calls on the
// main thread — no mouse steering (that depended on the map's scroll, zoom and
// on which tab was open, and failed silently) and no MoveTo teleport (it crashed
// the game and skipped every rule the real travel enforces).
int         g_confirm_travel = 0;    // ticks until we press the travel button
std::string g_travel_name;

bool g_pgup = false, g_pgdn = false, g_enter = false, g_home = false, g_end = false;

enum Filter { F_All = 0, F_Discovered, F_Undiscovered, F_Count };
int g_filter = F_All;

bool KeyEdge(int vk, bool* prev)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !*prev;
    *prev = down;
    return edge;
}
void SeedKeys()
{
    g_pgup  = (GetAsyncKeyState(VK_PRIOR)  & 0x8000) != 0;
    g_pgdn  = (GetAsyncKeyState(VK_NEXT)   & 0x8000) != 0;
    g_enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    g_home  = (GetAsyncKeyState(VK_HOME)   & 0x8000) != 0;
    g_end   = (GetAsyncKeyState(VK_END)    & 0x8000) != 0;
}

bool Passes(const game::MapMarker& m)
{
    if (g_filter == F_Discovered)   return m.discovered;
    if (g_filter == F_Undiscovered) return !m.discovered;
    return true;
}

const char* FilterName()
{
    switch (g_filter) {
        case F_Discovered:   return "tylko odkryte";
        case F_Undiscovered: return "tylko nieodkryte";
        default:             return "wszystkie";
    }
}

void Refresh()
{
    g_markers = game::GetMapMarkers();
    if (g_index >= (int)g_markers.size()) g_index = -1;
}

// "<name>, <distance>, <clock bearing>[, nieodkryta]"
std::string Describe(const game::MapMarker& m)
{
    std::string s = m.name + ", " + strings::FormatDistance(m.dist);
    auto pp = game::GetPlayerPosition();
    float dx = m.pos.x - pp.x, dy = m.pos.y - pp.y;
    if (dx * dx + dy * dy > 1.0f) {
        // Bearing relative to where the player faces, as a clock direction —
        // the same convention the object scanner uses.
        const float R2D = 57.2957795f;
        float bearing = std::atan2(dx, dy) * R2D - game::GetPlayerYaw();
        while (bearing >  180.0f) bearing -= 360.0f;   // ClockDirection wants -180..180
        while (bearing < -180.0f) bearing += 360.0f;
        s += ", " + strings::ClockDirection(bearing);
    }
    if (!m.discovered) s += ", nieodkryta";
    return s;
}

void Step(int dir)
{
    if (g_markers.empty()) {
        tolk::Speak("Brak lokacji na mapie.", tolk::Priority::Ui, true);
        return;
    }
    int n = (int)g_markers.size();
    int start = (g_index < 0) ? (dir > 0 ? -1 : 0) : g_index;
    for (int step = 0; step < n; ++step) {
        start = (start + dir + n) % n;
        if (Passes(g_markers[start])) {
            g_index = start;
            g_confirm_pending.clear();
            char idx[32];
            std::snprintf(idx, sizeof(idx), ", %d z %d", g_index + 1, n);
            tolk::Speak(Describe(g_markers[g_index]) + idx, tolk::Priority::Ui, true);
            return;
        }
    }
    tolk::Speak(std::string("Brak lokacji w filtrze: ") + FilterName(),
                tolk::Priority::Ui, true);
}

// Enter: real fast travel — select the marker, then let the game travel.
void TravelToSelected()
{
    if (g_index < 0 || g_index >= (int)g_markers.size()) {
        tolk::Speak("Najpierw wybierz lokację.", tolk::Priority::System, true);
        return;
    }
    const auto& m = g_markers[g_index];
    if (!m.has_tile) {
        tolk::Speak("Nie widzę tego markera na mapie świata. "
                    "Przełącz się na mapę świata.", tolk::Priority::System, true);
        return;
    }
    // The game itself refuses undiscovered locations — this is only a heads-up
    // so the player isn't left wondering why nothing happened.
    if (!m.discovered && g_confirm_pending != m.name) {
        g_confirm_pending = m.name;
        tolk::Speak("Lokacja nieodkryta — gra tam nie pozwoli podróżować. "
                    "Enter jeszcze raz, aby spróbować.",
                    tolk::Priority::System, true);
        return;
    }
    g_travel_name = m.name;
    F3A_INFO("mapnav: selecting marker '%s' (disc=%d)",
             m.name.c_str(), (int)m.discovered);
    poll::RequestMapMarkerClick(m.tile);
    g_confirm_travel = 4;                // ~0.3 s, then press the travel button
    tolk::Speak("Podróż do: " + m.name, tolk::Priority::System, true);
}

void WalkToSelected()
{
    if (g_index < 0 || g_index >= (int)g_markers.size()) {
        tolk::Speak("Najpierw wybierz lokację.", tolk::Priority::System, true);
        return;
    }
    const auto& m = g_markers[g_index];
    guide::Stop();
    autowalk::StartTo(m.pos, m.name, m.refr, m.form_id);
    poll::RequestMenuBack();             // leave the map and start walking
}

} // namespace

void Tick(float)
{
    bool open = menu::ActiveMenu() == menu::Id::Map;
    if (!open) {
        if (g_open) {
            g_open = false; g_index = -1;
            g_confirm_pending.clear(); g_last_objectives.clear();
        }
        return;
    }

    // Quests tab: speak the selected quest's objectives when they change. The
    // list is empty on the other tabs, so this costs nothing there.
    {
        std::string obj = game::GetQuestObjectivesText();
        if (!obj.empty() && obj != g_last_objectives) {
            g_last_objectives = obj;
            tolk::Speak(obj, tolk::Priority::Ui, false);
        }
    }
    if (!g_open) {
        g_open = true;
        g_index = -1;
        SeedKeys();                      // a held key on open must not fire
        Refresh();
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "Mapa: %d lokacji. Page Up i Page Down przeglądają, "
            "Enter podróżuje, Home idzie pieszo, End filtruje.",
            (int)g_markers.size());
        tolk::Speak(buf, tolk::Priority::Ui, true);
        return;
    }

    if (g_markers.empty()) Refresh();    // the list fills in a beat after opening

    if (g_confirm_travel > 0 && --g_confirm_travel == 0) {
        poll::RequestMapTravel();              // press "Podróżować do <miejsce>"
        return;
    }

    if (KeyEdge(VK_NEXT,  &g_pgdn))  Step(+1);
    if (KeyEdge(VK_PRIOR, &g_pgup))  Step(-1);
    if (KeyEdge(VK_RETURN, &g_enter)) {
        // On the Quests tab, Enter makes the highlighted quest the TRACKED one —
        // moving the highlight alone does not; the game starts tracking when the
        // row is clicked. The request is refused when the highlight is not in the
        // quest list, so on the map tabs it falls through to travelling.
        if (game::IsKeyboardSelectionIn("MM_QuestsList"))
            poll::RequestTrackQuest();
        else
            TravelToSelected();
    }
    if (KeyEdge(VK_HOME,   &g_home))  WalkToSelected();
    if (KeyEdge(VK_END,    &g_end)) {
        g_filter = (g_filter + 1) % F_Count;
        g_index  = -1;
        tolk::Speak(std::string("Filtr: ") + FilterName(), tolk::Priority::Ui, true);
    }
}

void Init() { F3A_INFO("Map navigation module ready."); }
void Shutdown() {}

} // namespace f3a::modules::mapnav
