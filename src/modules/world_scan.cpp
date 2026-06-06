#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/polling_loop.h"
#include "f3a/console.h"

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

bool CategoryModHeld()
{
    // Ctrl (grace-windowed) is the category modifier: Ctrl+PgUp / Ctrl+PgDn
    // cycle categories, plain PgUp/PgDn cycle objects.
    return hotkeys::CtrlActive();
}

// Cycle the scanner category (Ctrl+PgUp / Ctrl+PgDn). Announce ONLY the
// category name — the player then uses plain PgUp/PgDn to browse objects in
// it. (Announcing an object here made category-cycling sound like it was
// still scrolling objects.)
void CategoryStep(int dir)
{
    g_category = (g_category + dir + Cat_COUNT) % Cat_COUNT;
    if (ScanListStale()) Rescan(); else Refilter();
    tolk::Speak(CatName(g_category), tolk::Priority::Ui, true);
}

void ScanNext()
{
    if (!GameplayAndHud()) {
        F3A_DEBUG("ScanNext ignored: not gameplay/HUD.");
        return;
    }
    if (CategoryModHeld()) { CategoryStep(+1); return; }
    // Quests aren't tied to the physical cell scan and the tracked quest can
    // change in the Pip-Boy any time — re-read it on every press so it never
    // goes stale.
    if (g_category == Cat_Quest) { Refilter(); AnnounceCurrent(); return; }
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
    if (CategoryModHeld()) { CategoryStep(-1); return; }
    if (g_category == Cat_Quest) { Refilter(); AnnounceCurrent(); return; }
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

// Press/release the game's Use/Activate key (default E = scancode 0x12). FO3
// polls the keyboard once per frame in DirectInput immediate mode, so a down+up
// fired in the same instant falls between polls and is seen as "never pressed"
// (that's why held movement works but a tap didn't activate anything). So we
// hold the key DOWN for a few frames and release it.
void SendUse(bool down)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = 0x12;                          // DIK_E
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

void SendMouse(long dx, long dy)
{
    if (!dx && !dy) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx; in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;            // relative
    SendInput(1, &in, sizeof(INPUT));
}

int g_use_hold_ticks = 0;   // single-press path: frames to keep Use held

// Pitch-sweep path for floor/low objects (items, containers): the crosshair
// sits too high to hit something on the ground, so we aim yaw at it and tap Use
// while sweeping the view down (or up) across an arc. When the crosshair
// crosses the object it activates; we stop as soon as a menu opens. Doors / NPCs
// don't need this — they use the single-press path.
enum class Sweep { Idle, Running };
Sweep      g_sweep      = Sweep::Idle;
game::Vec3 g_sweep_tgt{};
int        g_sweep_frame = 0;
int        g_sweep_dir   = 1;     // initial guess: +1 = look "down"
int        g_sweep_applied = 0;   // net pitch counts applied (to undo on stop)
constexpr int kSweepStepFrames = 5;    // frames per E-tap+pitch step
constexpr int kSweepSteps      = 8;    // steps per direction
constexpr long kSweepPitchStep = 50;   // mouse counts of pitch per step
// Total = one pass in the guessed direction, then two passes back the other way
// so the whole vertical arc is covered regardless of the mouse-pitch sign.
constexpr int kSweepTotal      = kSweepSteps * 3;

void StartSweep(const game::Vec3& tgt)
{
    g_sweep       = Sweep::Running;
    g_sweep_tgt   = tgt;
    g_sweep_frame = 0;
    g_sweep_applied = 0;
    auto pp = game::GetPlayerPosition();
    g_sweep_dir = (tgt.z < pp.z + 100.0f) ? 1 : -1;   // below eye -> look down
    // Use the crosshair/camera-following pick so vertical aim actually matters
    // (the default Havok pick fires roughly horizontally from the body and
    // ignores camera pitch — which is why sweeping down never hit the book).
    game::SetIniSettingInt("bActivatePickUseGamebryoPick", 1);
}

void StopSweep(bool restorePitch)
{
    SendUse(false);
    if (restorePitch && g_sweep_applied)
        SendMouse(0, -g_sweep_applied);   // return the view roughly to level
    game::SetIniSettingInt("bActivatePickUseGamebryoPick", 0);  // restore default
    if (restorePitch)   // exhausted the arc without a menu opening
        tolk::Speak("Nie trafiłem w obiekt.", tolk::Priority::Background, false);
    g_sweep = Sweep::Idle;
    g_sweep_applied = 0;
}

void ActivateTarget()
{
    if (!GameplayAndHud()) return;
    if (!HaveTarget()) return;
    const auto& e = g_scan_list[g_scan_index];
    if (e.kind == game::WorldEntity::Kind::Quest) {
        tolk::Speak("Tego nie można aktywować.", tolk::Priority::System, true);
        return;
    }
    // If a future/other build ever exposes the console, prefer precise by-id
    // activation. Otherwise drive the Use key against the crosshair — first
    // widen the activation pick sphere so it forgives imperfect aim. Park facing
    // the target with auto-walk (\) first for best results.
    if (console::Available() && e.form_id != 0) {
        char cmd[80];
        std::snprintf(cmd, sizeof(cmd), "%08X.activate player 1", e.form_id);
        console::Run(cmd);
    } else {
        int r = config::Get().activate_pick_radius;
        if (r > 0) game::SetIniSettingFloat("fActivatePickSphereRadius",
                                            (float)r);
        bool lowObject = e.kind == game::WorldEntity::Kind::Item ||
                         e.kind == game::WorldEntity::Kind::Note ||
                         e.kind == game::WorldEntity::Kind::Container;
        if (lowObject) {
            // Diagnostic: report geometry so we can see whether auto-walk got
            // close and how far below eye level the object sits.
            auto pp = game::GetPlayerPosition();
            float dh = std::sqrt((e.position.x - pp.x) * (e.position.x - pp.x) +
                                 (e.position.y - pp.y) * (e.position.y - pp.y));
            float dz = e.position.z - (pp.z + 100.0f);
            char dbg[128];
            std::snprintf(dbg, sizeof(dbg),
                          "Szukam: %s, odległość %.0f, wysokość %.0f",
                          e.name.c_str(), dh, dz);
            tolk::Speak(dbg, tolk::Priority::Ui, true);
            StartSweep(e.position);   // sweep the view to catch floor objects
            return;
        }
        SendUse(true);                // doors / NPCs: single hold-release
        g_use_hold_ticks = 5;
    }
    tolk::Speak("Używam: " + e.name, tolk::Priority::Ui, true);
}

} // namespace

// Drives the deferred Use-key release (single-press path) and the pitch sweep
// (floor-object path). Called every frame.
void Tick(float)
{
    if (g_use_hold_ticks > 0 && --g_use_hold_ticks == 0)
        SendUse(false);

    if (g_sweep != Sweep::Running) return;

    // Leaving gameplay / a menu other than the HUD: if a menu opened we likely
    // activated something — success; stop without restoring pitch.
    if (!poll::IsGameplayActive()) { StopSweep(false); return; }
    auto m = menu::ActiveMenu();
    if (m != menu::Id::None && m != menu::Id::HUDMain) { StopSweep(false); return; }

    // Keep yaw locked on the target (camera mouse-turn, like auto-walk).
    auto pp = game::GetPlayerPosition();
    auto br = game::ComputeBearing(pp, game::GetPlayerYaw(), g_sweep_tgt);
    long dx = (long)(br.relative_yaw * 4.0f);
    if (dx >  150) dx =  150;
    if (dx < -150) dx = -150;
    SendMouse(dx, 0);

    int step  = g_sweep_frame / kSweepStepFrames;
    int phase = g_sweep_frame % kSweepStepFrames;
    if (step >= kSweepTotal) { StopSweep(true); return; }  // whole arc, no hit

    // Per-step: press Use, hold ~3 frames, release, then nudge the pitch. First
    // `kSweepSteps` steps go the guessed direction; the rest reverse, so the
    // crosshair traverses the entire vertical range either way.
    int dir = (step < kSweepSteps) ? g_sweep_dir : -g_sweep_dir;
    if      (phase == 0) SendUse(true);
    else if (phase == 3) SendUse(false);
    else if (phase == 4) {
        SendMouse(0, dir * kSweepPitchStep);
        g_sweep_applied += dir * kSweepPitchStep;
    }
    ++g_sweep_frame;
}

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
    hotkeys::Bind(h.activate_target, &ActivateTarget);
    F3A_INFO("World scan module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::worldscan
