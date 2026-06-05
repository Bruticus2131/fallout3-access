#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/polling_loop.h"

#include <windows.h>
#include <algorithm>

#include <cstdio>
#include <string>

namespace f3a::modules::worldscan {
namespace {

void Announce(const game::WorldEntity& e, const game::Bearing& b)
{
    std::string dist = strings::FormatDistance(b.distance);
    std::string dir  = strings::ClockDirection(b.relative_yaw);

    strings::Key key;
    switch (e.kind) {
    case game::WorldEntity::Kind::Actor:
        key = e.hostile ? strings::Key::NearbyHostileFmt
                        : strings::Key::NearbyFriendlyFmt;
        break;
    case game::WorldEntity::Kind::Container:
        key = strings::Key::NearbyContainerFmt; break;
    case game::WorldEntity::Kind::Door:
        key = strings::Key::NearbyDoorFmt; break;
    default:
        key = strings::Key::NearbyItemFmt; break;
    }

    tolk::Speak(
        strings::RenderArgs(key, e.name.c_str(), dist.c_str(), dir.c_str()),
        tolk::Priority::Background, false);
}

void DoScan(bool hostiles_only)
{
    if (!poll::IsGameplayActive()) {
        F3A_DEBUG("worldscan: hotkey ignored (not in gameplay).");
        return;
    }
    const auto& cfg = config::Get();
    auto entities = game::ScanNearby(cfg.nearby_scan_radius,
                                     cfg.nearby_scan_max_items,
                                     /*actors_only=*/hostiles_only,
                                     /*hostiles_only=*/hostiles_only);
    if (entities.empty()) {
        tolk::Speak(strings::Render(strings::Key::NoNearbyEntities),
                    tolk::Priority::System, true);
        return;
    }

    auto pos = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    for (const auto& e : entities) {
        auto b = game::ComputeBearing(pos, yaw, e.position);
        Announce(e, b);
    }
}

void ScanAll()      { DoScan(false); }
void ScanHostiles() { DoScan(true);  }

// ---- Object scanner: cycle nearby objects, turn to / walk to the pick ----
//
// PgDn / PgUp cycle through nearby named objects sorted by distance; each
// step announces "name, distance, o'clock direction". Home turns the player
// to face the current pick; End starts/stops AutoWalk toward it.

// Scanner categories (cycled with Shift+[ / Shift+]). g_scan_full holds the
// full unfiltered scan; g_scan_list is the current category's view.
enum Category { Cat_All, Cat_Npc, Cat_Item, Cat_Door, Cat_Container,
                Cat_Quest, Cat_COUNT };

const char* CatName(int c)
{
    switch (c) {
    case Cat_All:       return "wszystko";
    case Cat_Npc:       return "postacie";
    case Cat_Item:      return "przedmioty";
    case Cat_Door:      return "drzwi";
    case Cat_Container: return "pojemniki";
    case Cat_Quest:     return "zadania";
    default:            return "?";
    }
}

bool PassesCategory(game::WorldEntity::Kind k, int cat)
{
    switch (cat) {
    case Cat_All:       return true;
    case Cat_Npc:       return k == game::WorldEntity::Kind::Actor;
    case Cat_Item:      return k == game::WorldEntity::Kind::Item ||
                               k == game::WorldEntity::Kind::Note;
    case Cat_Door:      return k == game::WorldEntity::Kind::Door;
    case Cat_Container: return k == game::WorldEntity::Kind::Container;
    default:            return true;
    }
}

std::vector<game::WorldEntity> g_scan_full;   // everything scanned
std::vector<game::WorldEntity> g_scan_list;   // current category view
int         g_scan_index = -1;
int         g_category   = Cat_All;
const void* g_scan_cell  = nullptr;  // cell the full list was built for

bool GameplayAndHud()
{
    if (!poll::IsGameplayActive()) return false;
    auto active = menu::ActiveMenu();
    return active == menu::Id::None || active == menu::Id::HUDMain;
}

void Refilter()
{
    g_scan_list.clear();
    if (g_category == Cat_Quest) {
        // Quests come from the quest list, not the physical scan, and are
        // sorted by distance like everything else.
        g_scan_list = game::GetActiveQuests();
        auto pp = game::GetPlayerPosition();
        std::sort(g_scan_list.begin(), g_scan_list.end(),
                  [&pp](const game::WorldEntity& a, const game::WorldEntity& b){
                      float da = (a.position.x-pp.x)*(a.position.x-pp.x) +
                                 (a.position.y-pp.y)*(a.position.y-pp.y);
                      float db = (b.position.x-pp.x)*(b.position.x-pp.x) +
                                 (b.position.y-pp.y)*(b.position.y-pp.y);
                      return da < db;
                  });
    } else {
        for (const auto& e : g_scan_full)
            if (PassesCategory(e.kind, g_category)) g_scan_list.push_back(e);
    }
    g_scan_index = g_scan_list.empty() ? -1 : 0;
}

void Rescan()
{
    const auto& cfg = config::Get();
    // A wider net than the X-key scan: cycling is how the player explores.
    g_scan_full = game::ScanNearby(cfg.nearby_scan_radius * 2, 80,
                                   false, false);
    g_scan_cell = game::GetPlayerCell();
    F3A_INFO("Scanner rescan: %d objects found.", (int)g_scan_full.size());
    Refilter();
}

// True if the cached scan belongs to a different cell than the player is in
// now (changed save, walked through a door) — then it must be rebuilt.
bool ScanListStale()
{
    return g_scan_full.empty() || g_scan_cell != game::GetPlayerCell();
}

void AnnounceCurrent()
{
    if (g_scan_index < 0 || g_scan_index >= (int)g_scan_list.size()) {
        tolk::Speak(std::string("Brak w kategorii: ") + CatName(g_category),
                    tolk::Priority::System, true);
        return;
    }
    const auto& e = g_scan_list[g_scan_index];
    auto pos  = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto b    = game::ComputeBearing(pos, yaw, e.position);

    char idx[16];
    std::snprintf(idx, sizeof(idx), "%d z %d",
                  g_scan_index + 1, (int)g_scan_list.size());
    std::string line = e.name + ", " +
                       strings::FormatDistance(b.distance) + ", " +
                       strings::ClockDirection(b.relative_yaw) + ", " + idx;
    tolk::Speak(line, tolk::Priority::Ui, true);
}

bool ShiftHeld()
{
    // Use the poll's grace-windowed Shift state so Shift+[ / Shift+] reliably
    // cycle categories instead of occasionally falling through to item cycle.
    return hotkeys::ShiftActive();
}

// Cycle the scanner category (Shift+] / Shift+[), announce it, then the first
// item in that category.
void CategoryStep(int dir)
{
    g_category = (g_category + dir + Cat_COUNT) % Cat_COUNT;
    if (ScanListStale()) Rescan(); else Refilter();
    tolk::Speak(std::string("Kategoria: ") + CatName(g_category),
                tolk::Priority::Ui, true);
    if (!g_scan_list.empty()) AnnounceCurrent();
}

void ScanNext()
{
    if (!GameplayAndHud()) {
        F3A_DEBUG("ScanNext ignored: not gameplay/HUD.");
        return;
    }
    if (ShiftHeld()) { CategoryStep(+1); return; }
    if (ScanListStale() || g_scan_index < 0) {
        Rescan();
    } else if (++g_scan_index >= (int)g_scan_list.size()) {
        // Wrapped — refresh the snapshot (things move) and start over.
        Rescan();
    }
    AnnounceCurrent();
}

void ScanPrev()
{
    if (!GameplayAndHud()) return;
    if (ShiftHeld()) { CategoryStep(-1); return; }
    if (ScanListStale() || g_scan_index < 0) {
        Rescan();
    } else if (--g_scan_index < 0) {
        g_scan_index = (int)g_scan_list.size() - 1;
    }
    AnnounceCurrent();
}

void TurnToCurrent()
{
    if (!GameplayAndHud()) return;
    if (g_scan_index < 0 || g_scan_index >= (int)g_scan_list.size()) return;
    const auto& e = g_scan_list[g_scan_index];
    game::SetPlayerYawTo(e.position);
    tolk::Speak("Skierowano na: " + e.name, tolk::Priority::Ui, true);
}

bool HaveTarget()
{
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size()) return true;
    tolk::Speak("Najpierw wybierz cel skanerem.",
                tolk::Priority::System, true);
    return false;
}

void GuideToggle()
{
    if (!GameplayAndHud()) return;
    if (guide::IsActive()) { guide::Stop(); return; }
    if (!HaveTarget()) return;
    autowalk::Stop();   // the two modes are mutually exclusive
    const auto& e = g_scan_list[g_scan_index];
    guide::StartTo(e.position, e.name);
}

void AutoWalkToggle()
{
    if (!GameplayAndHud()) return;
    if (autowalk::IsWalking()) { autowalk::Stop(); return; }
    if (!HaveTarget()) return;
    guide::Stop();      // the two modes are mutually exclusive
    const auto& e = g_scan_list[g_scan_index];
    autowalk::StartTo(e.position, e.name);
}

} // namespace

void Init()
{
    const auto& h = config::Get().hotkeys;
    hotkeys::Bind(h.scan_nearby,   &ScanAll);
    hotkeys::Bind(h.scan_hostiles, &ScanHostiles);
    hotkeys::Bind(h.scan_next,     &ScanNext);
    hotkeys::Bind(h.scan_prev,     &ScanPrev);
    hotkeys::Bind(h.turn_to,       &TurnToCurrent);
    hotkeys::Bind(h.guide_beacon,  &GuideToggle);
    hotkeys::Bind(h.auto_walk,     &AutoWalkToggle);
    F3A_INFO("World scan module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::worldscan
