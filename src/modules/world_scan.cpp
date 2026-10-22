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
#include "f3a/audio_beacon.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace f3a::modules::worldscan {
namespace {

// Spoken lock state for doors/containers: "" if no lock, else locked + pick
// difficulty (or "needs key"). Lets the player find lockpick targets by ear.
std::string LockDesc(const game::WorldEntity& e)
{
    if (!e.locked) return "";
    if (e.lock_level >= 250)
        return strings::T(", zamknięte na klucz", ", locked, requires a key");
    int L = e.lock_level;
    const char* d = (L < 25)  ? strings::T("bardzo łatwy", "very easy lock") :
                    (L < 50)  ? strings::T("łatwy", "easy lock") :
                    (L < 75)  ? strings::T("średni", "average lock") :
                    (L < 100) ? strings::T("trudny", "hard lock") :
                                strings::T("bardzo trudny", "very hard lock");
    return std::string(strings::T(", zamknięte, zamek ", ", locked, ")) + d;
}

// A body is something to search, not someone to talk to: Dad's last
// holotape is on Jonas's body in the vault, and nothing said he was one.
// Whether anything is left on it, when that can be told.
std::string BodyDesc(const game::WorldEntity& e)
{
    if (e.kind != game::WorldEntity::Kind::Actor || !e.dead) return "";
    if (e.items == 1) return strings::T(", nie żyje, do przeszukania", ", dead, can be searched");
    if (e.items == 0) return strings::T(", nie żyje, pusty", ", dead, empty");
    return strings::T(", nie żyje", ", dead");
}

// How far, as the crow flies, and whether it is a floor up or down - in a
// vault the thing "two metres away" may be on the level below. The plain
// bearing is flat (autowalk wants it so); the spoken distance is not.
constexpr float kFloorDz = 160.0f;

std::string DistanceText(const game::WorldEntity& e, const game::Bearing& b)
{
    const float dz = e.position.z - game::GetPlayerPosition().z;
    std::string s = strings::FormatDistance(std::sqrt(b.distance * b.distance + dz * dz));
    if (dz > kFloorDz)       s += strings::T(", wyżej", ", above");
    else if (dz < -kFloorDz) s += strings::T(", niżej", ", below");
    return s;
}

// A door that stands open says so; closed is how doors are.
std::string DoorDesc(const game::WorldEntity& e)
{
    if (e.kind == game::WorldEntity::Kind::Door && e.open_state == 1)
        return strings::T(", otwarte", ", open");
    return {};
}

void Announce(const game::WorldEntity& e, const game::Bearing& b)
{
    std::string dist = DistanceText(e, b);
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
        // Terminals, buttons, furniture: their own name says what they are.
        key = game::IsFixture(e) ? strings::Key::NearbyFriendlyFmt
                                 : strings::Key::NearbyItemFmt;
        break;
    }

    std::string name = e.name + LockDesc(e) + BodyDesc(e) + DoorDesc(e);
    tolk::Speak(
        strings::RenderArgs(key, name.c_str(), dist.c_str(), dir.c_str()),
        tolk::Priority::Background, false);
}

void DoScan(bool hostiles_only)
{
    // Speak instead of silently returning, so "nothing happens" is never
    // ambiguous: if the scanner is gated off you HEAR why, rather than wonder
    // whether the key even fired.
    if (!game::IsPlayerValid()) {
        tolk::Speak(strings::T("Skaner niedostępny tutaj.", "Scanner not available here."),
                    tolk::Priority::System, true);
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

// Scanner categories (cycled with Shift+PgUp / Shift+PgDn). g_scan_full holds the
// full unfiltered scan; g_scan_list is the current category's view.
enum Category { Cat_All, Cat_Npc, Cat_Corpse, Cat_Item, Cat_Door, Cat_Container,
                Cat_Location, Cat_QuestObj, Cat_Quest, Cat_COUNT };

const char* CatName(int c)
{
    switch (c) {
    case Cat_All:       return strings::T("wszystko", "all");
    case Cat_Npc:       return strings::T("postacie", "people");
    case Cat_Corpse:    return strings::T("ciała", "bodies");
    case Cat_Item:      return strings::T("przedmioty", "items");
    case Cat_Door:      return strings::T("drzwi", "doors");
    case Cat_Container: return strings::T("pojemniki", "containers");
    case Cat_Location:  return strings::T("miejsca", "places");
    case Cat_QuestObj:  return strings::T("przedmioty zadań", "quest objects");
    case Cat_Quest:     return strings::T("zadania", "quests");
    default:            return "?";
    }
}

bool PassesCategory(const game::WorldEntity& e, int cat)
{
    const auto k = e.kind;
    switch (cat) {
    case Cat_All:       return true;
    // People are the living; bodies have their own category, as in the FNV
    // mod — bodies are for searching, the living for talking to.
    case Cat_Npc:       return k == game::WorldEntity::Kind::Actor && !e.dead;
    case Cat_Corpse:    return k == game::WorldEntity::Kind::Actor && e.dead;
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
int         g_aim_cycle  = 0;     // which shooting-range target `,` aims next

// Mouse-feedback aim at a fixed target (shooting range). Writing rotX/rotZ does
// NOT move the first-person camera (proven by a dump: the crosshair never budged
// across 11 pitch writes) — the engine drives the camera from the MOUSE, like
// autowalk's turn. So we aim by injecting mouse motion: converge YAW toward the
// target using GetPlayerYaw feedback, then sweep PITCH with mouse-Y until the
// crosshair ref is the target. Needs FIRST person (mouse turns body+view
// together while standing); third person orbits the camera instead.
bool        g_aim_on    = false;
game::Vec3  g_aim_pos{};
uint32_t    g_aim_id    = 0;
std::string g_aim_label;
int         g_aim_phase = 0;    // 0 = yaw converge, 1 = pitch converge
bool        g_aim_face_only = false;  // just turn to face (no pitch, no fire)
int         g_aim_budget = 0;   // overall tick timeout
int         g_aim_pstep  = 0;   // pitch step counter
int         g_aim_wait   = 0;   // settle ticks between pitch steps
int         g_pitch_sign = 1;   // mouse-Y direction that reduces pitch error
float       g_pitch_prev_err = 0.0f;
// Mouse counts per degree of error. Starts brisk and is halved on every
// overshoot, so a big correction is fast and the last degree is gentle.
float       g_pitch_gain = 6.0f;
float       g_yaw_gain   = 4.0f;
float       g_yaw_prev_err = 0.0f;
bool        g_yaw_prev_set = false;
bool        g_pitch_started  = false;

// "Center camera" (Home): level the first-person view to horizontal. Like the
// aim's pitch phase but the target is pitch 0, and there's no fire afterwards.
bool        g_level_on      = false;
int         g_level_budget  = 0;
int         g_level_sign    = 1;
float       g_level_prev    = 0.0f;
bool        g_level_started = false;

// Turn-verify: after a native SetAngle turn (TurnToCurrent), read the bearing to
// the target a beat later and announce its new clock direction — so a blind
// tester HEARS whether the turn landed (target should end at "12 o'clock").
DWORD       g_turn_verify_at = 0;        // GetTickCount deadline; 0 = idle
game::Vec3  g_turn_verify_pos{};
std::string g_turn_verify_name;
bool        g_turn_verify_quiet = false; // Home: correct the turn, say nothing
// Closed-loop correction: the geometric SetAngle turn can land a few degrees off
// (yaw-convention rounding, or the target/player shifted between the turn and the
// read-back). Rather than only announcing the miss, re-aim yaw from the FRESH
// bearing and verify again, up to a couple of passes — this is what made Home
// "average". Deadband so a good turn is a silent no-op.
int         g_turn_correct_left = 0;     // corrective passes remaining
constexpr float kTurnGoodDeg  = 6.0f;    // within this = landed, stop correcting
constexpr int   kTurnMaxCorr  = 2;       // cap passes to avoid oscillation

void MouseMoveRel(long dx, long dy)
{
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = dx;
    in.mi.dy      = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// Fire the weapon by pressing the game's ATTACK key. FO3's default attack is the
// left mouse button, but the key is remappable (this player uses V), so it's an
// INI setting (Hotkeys/AttackKey, DIK scancode). Sent as a scancode, like W.
void FireKey(bool down)
{
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wScan   = (WORD)config::Get().hotkeys.attack_key;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

// Auto-fire burst state (phase 2). The geometric aim hits the target's ORIGIN;
// the visible target may sit above/below it, so we fire several shots while
// nudging the pitch across a small vertical arc — a pellet lands on the target.
int  g_fire_left    = 0;
bool g_fire_holding = false;
long g_spray_px = 0, g_spray_py = 0;   // current mouse offset from the aim centre
// 2-D spray grid (mouse-count offsets from the aim centre; ~0.085 deg/count, so
// this spans ~+-3 deg yaw x ~+-10 deg pitch). The geometric aim hits the target
// ORIGIN; the visible/hittable target sits somewhere around it, so we walk this
// cone firing a shot at each point. Centre yaw column scanned first.
struct SprayPt { long dx, dy; };
const long kSprayYaw[3]   = { 0, -18, 18 };
const long kSprayPitch[7] = { 0, -40, 40, -80, 80, -120, 120 };
const int  kSprayCount    = 21;        // 3 yaw x 7 pitch
SprayPt SprayOffset(int i)
{
    int col = i / 7, row = i % 7;
    return { kSprayYaw[col], kSprayPitch[row] };
}

// End the mouse-driven aim. If a burst is holding the attack key it is let go
// here: an aim cut off mid-shot (out of time, a menu, a load) used to leave the
// injected key down, and the weapon kept firing.
void StopAimLoop()
{
    if (g_fire_holding) { FireKey(false); g_fire_holding = false; }
    g_fire_left = 0;
    g_spray_px = 0; g_spray_py = 0;
    g_aim_on = false;
}
const void* g_scan_cell  = nullptr;  // cell the full list was built for

bool GameplayAndHud()
{
    if (!poll::IsGameplayActive()) return false;
    // Ask the engine, not only our own dispatch stack: the stack holds just the
    // menus we have handlers for, so an unhandled one reads as "no menu" and
    // every world hotkey stays live inside it.
    if (poll::MenuModeActive()) return false;
    auto active = menu::ActiveMenu();
    return active == menu::Id::None || active == menu::Id::HUDMain;
}

// How many each category view keeps (nearest first). The categories are cut
// from the WHOLE scan, never from a short list: with a cap on the scan itself,
// a category whose things were all a little further off came out empty.
constexpr int kViewMax  = 80;
constexpr int kScanMax  = 3000;     // the scan itself: in practice everything in range

// How far the scanner reaches: as far as the loaded world, as the FNV mod's
// does - a whole interior, and outside [Voice] ScannerRadius. It used to be
// twice NearbyScanRadius, 2400: from Megaton's gate the town's doors are 2000
// to 7000 units away (Craterside Supply 4800), and the scan found three.
constexpr int kWholeCell = 1000000;
int ScannerReach()
{
    return game::PlayerIsInInterior() ? kWholeCell : config::Get().scanner_radius;
}

// The list a category shows, from the current scan. Shared by the browse view
// and by the question "is anything in it at all", so the two can never differ.
std::vector<game::WorldEntity> CategoryView(int cat)
{
    std::vector<game::WorldEntity> v;
    if (cat == Cat_Location) {
        // Places come from the worldspace's map markers, as far as the compass
        // shows them (the FNV mod's range).
        v = game::GetNearbyLocations(20000.0f, kViewMax);
        return v;
    }
    if (cat == Cat_QuestObj) {
        // What a running quest waits for the player to use: the button, the
        // terminal, the door, the item, the spot to walk into, the free seat
        // (questobjs.dat, worked out from the quests' own scripts). A little
        // further than the plain scan - a whole vault level.
        return game::GetQuestObjects(ScannerReach(), kViewMax);
    }
    for (const auto& e : g_scan_full) {
        if (!PassesCategory(e, cat)) continue;
        v.push_back(e);
        if ((int)v.size() >= kViewMax) break;      // g_scan_full is nearest first
    }
    return v;
}

void Refilter()
{
    g_scan_list.clear();
    if (g_category == Cat_Quest) {
        // Quests come from the quest list, not the physical scan, and are
        // sorted by distance like everything else.
        // The quest the player tracks, not every quest running: the Pip-Boy is
        // where quests are chosen, the scanner walks to the chosen one.
        g_scan_list = game::GetActiveQuests(/*trackedOnly*/true);
        auto pp = game::GetPlayerPosition();
        std::sort(g_scan_list.begin(), g_scan_list.end(),
                  [&pp](const game::WorldEntity& a, const game::WorldEntity& b){
                      // Marker-bearing quests first (sorted by distance), then
                      // marker-less ones (no meaningful distance to compare).
                      if (a.has_marker != b.has_marker) return a.has_marker;
                      if (!a.has_marker) return false;
                      float da = (a.position.x-pp.x)*(a.position.x-pp.x) +
                                 (a.position.y-pp.y)*(a.position.y-pp.y);
                      float db = (b.position.x-pp.x)*(b.position.x-pp.x) +
                                 (b.position.y-pp.y)*(b.position.y-pp.y);
                      return da < db;
                  });
    } else {
        g_scan_list = CategoryView(g_category);
    }
    g_scan_index = g_scan_list.empty() ? -1 : 0;
}

void Rescan()
{
    // A wider net than the X-key scan: cycling is how the player explores.
    g_scan_full = game::ScanNearby(ScannerReach(), kScanMax, false, false);
    // Nearest first in three dimensions - and indoors the floor you stand on
    // before the ones above and below, however close they are in plan (the
    // FNV mod's order). The scan itself sorts flat.
    {
        const auto pp = game::GetPlayerPosition();
        const bool indoors = game::PlayerIsInInterior();
        auto key = [&](const game::WorldEntity& e) {
            const float dx = e.position.x - pp.x, dy = e.position.y - pp.y,
                        dz = e.position.z - pp.z;
            const bool otherFloor = indoors && std::fabs(dz) > kFloorDz;
            return std::make_pair(otherFloor, dx * dx + dy * dy + dz * dz);
        };
        std::stable_sort(g_scan_full.begin(), g_scan_full.end(),
                         [&](const game::WorldEntity& a, const game::WorldEntity& b) {
                             return key(a) < key(b);
                         });
    }
    g_scan_cell = game::GetPlayerCell();
    F3A_INFO("Scanner rescan: %d objects found.", (int)g_scan_full.size());
    Refilter();
}

// True if the cached scan belongs to a different cell than the player is in
// now (changed save, walked through a door) — then it must be rebuilt.
bool ScanListStale()
{
    // The category's own list: places and quest objects do not come from the
    // physical scan, and an empty one there made every PgDn start over at 1.
    return g_scan_list.empty() || g_scan_cell != game::GetPlayerCell();
}

void AnnounceCurrent()
{
    if (g_scan_index < 0 || g_scan_index >= (int)g_scan_list.size()) {
        tolk::Speak(std::string(strings::T("Brak w kategorii: ", "Nothing in category: ")) +
                        CatName(g_category),
                    tolk::Priority::System, true);
        return;
    }
    const auto& e = g_scan_list[g_scan_index];
    char idx[24];
    std::snprintf(idx, sizeof(idx), strings::T("%d z %d", "%d of %d"),
                  g_scan_index + 1, (int)g_scan_list.size());

    // Active quests with no compass marker ("go talk to X") have no bearing —
    // announce the name only so the distance/direction isn't bogus.
    if (!e.has_marker) {
        tolk::Speak(e.name + strings::T(", bez markera, ", ", no marker, ") + idx,
                    tolk::Priority::Ui, true);
        return;
    }

    // A target in another place (a quest marker in Megaton while you are in
    // the Wasteland): its coordinates belong to that place, and a distance and
    // a clock bearing to them meant nothing. Where it is, instead; the
    // auto-walk finds the way through the doors.
    if (e.refr && !game::TargetSharesPlayerSpace(e.refr)) {
        std::string where = game::GetRefPlaceName(e.refr);
        tolk::Speak(e.name + strings::T(", w innym miejscu", ", in another place") +
                        (where.empty() ? std::string() : ": " + where) + ", " + idx,
                    tolk::Priority::Ui, true);
        return;
    }

    auto pos  = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto b    = game::ComputeBearing(pos, yaw, e.position);
    std::string line = e.name + LockDesc(e) + BodyDesc(e) + DoorDesc(e) + ", " +
                       DistanceText(e, b) + ", " +
                       strings::ClockDirection(b.relative_yaw) + ", " + idx;
    tolk::Speak(line, tolk::Priority::Ui, true);
}

bool CategoryModHeld()
{
    // Shift is the category modifier: Shift+PgUp / Shift+PgDn cycle categories,
    // plain PgUp/PgDn cycle objects. Ctrl used to do this, but Ctrl is the
    // game's own SNEAK toggle — every category step also crouched or stood the
    // player up. Read live, so releasing Shift counts on the very next press.
    return hotkeys::ShiftHeldNow();
}

// Cycle the scanner category (Shift+PgUp / Shift+PgDn). Announce ONLY the
// category name — the player then uses plain PgUp/PgDn to browse objects in
// it. (Announcing an object here made category-cycling sound like it was
// still scrolling objects.)
//
// Only categories with something in them: the scan is made afresh on every
// step (things move, the player walks — a cell outdoors is 4096 units wide,
// so "same cell" is no proof of "same things"), and a category is skipped
// only when its own view, built exactly as browsing builds it, is empty.
// Said with the count: "pojemniki, 3".
void CategoryStep(int dir)
{
    Rescan();
    int from = g_category;
    for (int i = 1; i <= Cat_COUNT; ++i) {
        int c = ((from + dir * i) % Cat_COUNT + Cat_COUNT) % Cat_COUNT;
        int n = 0;
        if (c == Cat_Quest)         n = (int)game::GetActiveQuests(/*trackedOnly*/true).size();
        else if (c == Cat_Location || c == Cat_QuestObj) n = (int)CategoryView(c).size();
        else for (const auto& e : g_scan_full) if (PassesCategory(e, c)) ++n;   // all, not the view's cap
        if (n <= 0) continue;
        g_category = c;
        Refilter();
        F3A_INFO("Scanner category '%s': %d", CatName(c), n);
        tolk::Speak(std::string(CatName(c)) + ", " + std::to_string(n),
                    tolk::Priority::Ui, true);
        return;
    }
    g_category = Cat_All;
    Refilter();
    tolk::Speak(strings::T("Nic w pobliżu.", "Nothing nearby."), tolk::Priority::Ui, true);
}

// Browse the quest category. The journal list is re-read LIVE on every press
// (quests start/advance any time), but the browse position must survive the
// refresh — Refilter() resets the index to 0, which made PgUp/PgDn forever
// repeat the first quest. So: remember the selected quest by name, refresh,
// find it again, THEN step.
void QuestStep(int dir)
{
    std::string cur = (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size())
                          ? g_scan_list[g_scan_index].name
                          : std::string();
    Refilter();                          // fresh journal; index reset to 0
    int n = (int)g_scan_list.size();
    if (n == 0) { AnnounceCurrent(); return; }       // "Brak w kategorii"
    int base = -1;
    if (!cur.empty()) {
        for (int i = 0; i < n; ++i)
            if (g_scan_list[i].name == cur) { base = i; break; }
    }
    // Previously-selected quest still present -> step from it; otherwise start
    // from the top without stepping (first press announces the first quest).
    g_scan_index = (base >= 0) ? (base + dir + n) % n : 0;
    AnnounceCurrent();
}

void ScanNext()
{
    if (!GameplayAndHud()) {
        F3A_DEBUG("ScanNext ignored: not gameplay/HUD.");
        return;
    }
    if (CategoryModHeld()) { CategoryStep(+1); return; }
    // Quests aren't tied to the physical cell scan and the journal can change
    // in the Pip-Boy any time — re-read it on every press so it never goes
    // stale, preserving the browse position.
    if (g_category == Cat_Quest) { QuestStep(+1); return; }
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
    if (g_category == Cat_Quest) { QuestStep(-1); return; }
    if (ScanListStale() || g_scan_index < 0) {
        Rescan();
    } else if (--g_scan_index < 0) {
        g_scan_index = (int)g_scan_list.size() - 1;
    }
    AnnounceCurrent();
}

// Turn to FACE a target with the mouse (the engine ignores rotZ writes for the
// first-person camera, so SetPlayerYawTo didn't actually turn the view). Faces
// the selected scanner object, or the nearest actor if nothing is selected —
// the latter establishes line-of-sight to an enemy so VATS will engage.
void TurnToCurrent()
{
    if (!GameplayAndHud()) return;
    game::Vec3 tgt{}; std::string name; bool have = false;

    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
        g_scan_list[g_scan_index].has_marker) {
        tgt  = g_scan_list[g_scan_index].position;
        name = g_scan_list[g_scan_index].name;
        have = true;
    } else {
        auto act = game::ScanNearby(config::Get().nearby_scan_radius * 2, 1,
                                    true, false);   // nearest actor
        if (!act.empty()) { tgt = act[0].position; name = act[0].name; have = true; }
    }
    if (!have) {
        tolk::Speak(strings::T("Brak celu w pobliżu.", "No target nearby."),
                    tolk::Priority::System, true);
        return;
    }

    // NATIVE SetAngle yaw (friend's lead): face the target in one shot via the
    // engine's own SetAngle Z, on the main thread. Works in first & third person
    // (replaces the old mouse-turn). If it snaps to face the target, native yaw
    // works and we can retire the mouse turn.
    StopAimLoop();
    auto pp = game::GetPlayerPosition();
    float dx = tgt.x - pp.x, dy = tgt.y - pp.y;
    float yawDeg = std::atan2(dx, dy) * 57.2957795f;   // 0 = north(+Y), clockwise
    poll::RequestAim(/*yaw*/true, yawDeg, /*pitch*/false, 0.0f);
    tolk::Speak(strings::T("Obracam do: ", "Turning to: ") + name, tolk::Priority::Ui, true);
    // Verify ~450 ms later: announce where the target now sits, so we can hear if
    // the native turn actually landed it ahead (≈ "godzina dwunasta").
    g_turn_verify_pos   = tgt;
    g_turn_verify_name  = name;
    g_turn_verify_quiet = false;
    g_turn_verify_at    = GetTickCount() + 450;
    g_turn_correct_left = kTurnMaxCorr;   // allow a couple of closed-loop nudges
}

bool HaveTarget()
{
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size()) return true;
    tolk::Speak(strings::T("Najpierw wybierz cel skanerem.",
                           "Select a target with the scanner first."),
                tolk::Priority::System, true);
    return false;
}

void GuideToggle()
{
    if (!GameplayAndHud()) return;
    if (guide::IsActive()) { guide::Stop(); return; }
    if (!HaveTarget()) return;
    const auto& e = g_scan_list[g_scan_index];
    if (!e.has_marker) {
        tolk::Speak(strings::T("To zadanie nie ma markera na mapie.",
                               "This quest has no map marker."),
                    tolk::Priority::System, true);
        return;
    }
    // The target's marker position is in ITS worldspace. If that isn't the
    // player's (a location out in the wasteland while we're in Megaton, say),
    // walking/beaconing toward those raw coordinates heads off in a meaningless
    // direction — send the player to teleport instead of leading them astray.
    if (e.refr && !game::TargetSharesPlayerSpace(e.refr)) {
        tolk::Speak(strings::T("Cel jest w innej lokacji — sygnał tam nie poprowadzi. "
                               "Użyj automatycznego chodzenia: prowadzi przez przejścia.",
                               "The target is in another location — the beacon can't lead "
                               "there. Use auto-walk: it goes through doors."),
                    tolk::Priority::System, true);
        return;
    }
    autowalk::Stop();   // the two modes are mutually exclusive
    guide::StartTo(e.position, e.name);
}

// Drop the item selected in the inventory (Delete). Overencumbrance is a real
// problem for a blind player — you can't see the weight bar creeping up — and
// the menu already has an "Upuść" button, so we press that rather than moving
// items ourselves: the game then does its normal thing, quantity prompt and all.
void DropItem()
{
    if (menu::ActiveMenu() != menu::Id::Inventory) {
        tolk::Speak(strings::T("Najpierw otwórz ekwipunek.", "Open your inventory first."),
                    tolk::Priority::System, true);
        return;
    }
    poll::RequestDropItem();
}

// Press the highlighted row in whatever menu is open (F2; was Insert, which NVDA takes).
//
// This exists for key rebinding: Fallout's controls page starts listening for a
// new key only when the row is CLICKED, so with the keyboard alone a blind
// player could reach the row, hear it, and then be stuck. It is deliberately
// generic — any list that needs a click rather than Enter benefits.
void PressRow()
{
    // Only in the main/pause menu, and there only on a settings page (checked
    // on the game's thread). Elsewhere F2 belongs to the game - a Pip-Boy page -
    // so it is left alone, silently.
    if (menu::ActiveMenu() != menu::Id::Start || !poll::MenuModeActive()) return;
    poll::RequestPressRow();
}

// Hold / release the game's aim, the same thing the right mouse button does.
//
// Iron sights are a hold, not a press, which is exactly what a player who
// navigates by keyboard cannot do comfortably — so this latches it: one press
// aims, the next lowers the weapon. Mouse BUTTON state is injected (mouse input
// reaches FO3 reliably, unlike keys in menus), so the game sees a genuine held
// right button and all its own logic — zoom, sway, accuracy — applies.
bool g_aim_held = false;
uint32_t g_hold_target = 0;    // the reference the held aim is locked onto
int  g_hold_cue_timer  = 0;    // ticks to the next on-target blip
int  g_hold_los_timer  = 0;    // ticks to the next ray check
bool g_hold_on_target  = false;// was the target on the crosshair last tick

// The aim is held until something ends it, never on a timer — see the Home
// handler. Negative means "until StopAimTrack()"; see RequestAimTrack.
constexpr int kHoldAimFrames = -1;

// Lock the frame-by-frame aim onto whatever the scanner has selected (or the
// nearest actor). Defined further down, beside the target picker.
bool LockAimOnCurrentTarget(std::string* outName, float* outDist = nullptr);

// Trim the held aim up or down.
//
// The engine gives us no honest answer to "is the crosshair on it": the field
// FOSE exposes as the crosshair reference does not follow the view, and the
// height a shot leaves the player at is a guess that is badly wrong for, say,
// the ten-year-old of the prologue. The player, though, can hear whether a shot
// hit. So the last word is theirs: nudge until it lands, and the correction
// stays for every target afterwards, because the error it cancels is constant.
constexpr float kAimTrimStep = 8.0f;

void AimTrim(float units)
{
    if (!GameplayAndHud()) return;
    if (!poll::AimTrackActive()) {
        tolk::Speak(strings::T("Najpierw wybierz cel klawiszem celowania.",
                               "Select a target with the aim key first."),
                    tolk::Priority::System, true);
        return;
    }
    float total = poll::NudgeAim(units);
    char buf[96];
    std::snprintf(buf, sizeof(buf), strings::T("%s, poprawka %+.0f", "%s, correction %+.0f"),
                  units > 0 ? strings::T("Wyżej", "Higher") : strings::T("Niżej", "Lower"),
                  total);
    tolk::Speak(buf, tolk::Priority::Combat, true);
}

void AimTrimUp()   { AimTrim(+kAimTrimStep); }
void AimTrimDown() { AimTrim(-kAimTrimStep); }

// Aiming follows the KEY, not a toggle: hold it and the game's aim button stays
// down, let go and it comes up. A toggle was wrong for this — a weapon left
// raised because a second press was missed is worse than no help at all, and it
// does not match how a sighted player aims.
//
// The right mouse button is used because that IS the game's aim control and a
// keyboard player cannot reach it; the key simply stands in for it.
// Our own aim key does ONE thing now: hold down the game's aim for a player
// who cannot reach the mouse button. Everything that follows from aiming —
// acquiring a target, tracking it, the line-of-fire tone — hangs off the GAME'S
// aim control instead (see AimControlTick), so it happens just the same when
// the player aims the ordinary way. That is how the FNV mod is built, and
// building it the other way round is why holding the right mouse button did
// nothing for us.
void AimHoldPress()
{
    if (!GameplayAndHud() || g_aim_held) return;
    g_aim_held = true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &in, sizeof(in));
    // And the game's own bullet correction, widened for as long as the key is
    // held: our aim only has to get near an enemy, the engine bends the shot
    // the rest of the way. Only on our key — a sighted player's mouse aim
    // keeps the game's normal values.
    poll::SetEngineAimAssist(true);

}

// Release the aim if it is still latched — also called when gameplay ends, so
// the button is never left stuck down.
void AimHoldRelease()
{
    poll::SetEngineAimAssist(false);
    if (!g_aim_held) return;
    g_aim_held = false;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &in, sizeof(in));
}

// THE GAME'S AIM CONTROL DRIVES EVERYTHING.
//
// Edge-detected every tick: pressing the player's own aim control acquires a
// target and starts tracking it; releasing it lets go. Our ']' key feeds the
// same path for free, because it holds the real mouse button down and this
// test then reads it as held — one mechanism, two ways in.
bool g_aim_ctrl_was = false;

void AimControlTick()
{
    // Two ways in, and OUR KEY IS NOT GATED ON THE GAME SEEING A BUTTON.
    //
    // A log of nine presses of the aim key produced not one AimControl line:
    // the player has no mouse, and whatever we inject or whatever control 6
    // holds, the game's own aim never read as held. Making assistance depend on
    // that is making it depend on hardware the player does not have. So the
    // mod's own key engages on its own authority, and the game's control is an
    // additional way in for players who do use the mouse.
    bool byGame = game::IsAimControlHeld() && game::IsWeaponOut();
    bool now = GameplayAndHud() && (g_aim_held || byGame);
    if (now != g_aim_ctrl_was) {
        // One line that names which of the three tests decided it, so "the aim
        // key does nothing" never again needs a guess.
        F3A_INFO("AimControl: %s (our key=%d, game control=%d, weapon out=%d)",
                 now ? "ON" : "off", (int)g_aim_held,
                 (int)game::IsAimControlHeld(), (int)game::IsWeaponOut());
    }
    if (now == g_aim_ctrl_was) return;
    g_aim_ctrl_was = now;

    if (now) {
        g_hold_target    = 0;
        g_hold_on_target = false;
        g_hold_cue_timer = 0;
        std::string name;
        float dist = 0.0f;
        if (!LockAimOnCurrentTarget(&name, &dist)) {
            F3A_INFO("AimControl: held, but there is nothing to lock onto (%s)", name.c_str());
            // Said, so a shot is not fired at whatever happens to be ahead.
            if (!name.empty()) tolk::Speak(name, tolk::Priority::Combat, true);
        } else {
            F3A_INFO("AimControl: held — locked on '%s'", name.c_str());
            // Say WHAT the aim took, the way Home does: "Radrakan, 300
            // jednostek". Without it a lock on the wrong thing is silent.
            tolk::Speak(name + ", " + strings::FormatDistance(dist),
                        tolk::Priority::Combat, true);
        }
    } else {
        g_hold_target    = 0;
        g_hold_on_target = false;
        g_hold_cue_timer = 0;
        poll::WatchAimLos(0);
        poll::StopAimTrack();
        F3A_INFO("AimControl: released");
    }
}

// The hotkey table only reports presses, so the RELEASE is polled: convert the
// configured scancode to a virtual key and watch it while the aim is latched.
void AimHoldPoll()
{
    if (!g_aim_held) return;
    UINT vk = MapVirtualKeyA(config::Get().hotkeys.aim_hold, MAPVK_VSC_TO_VK);
    if (!vk) return;                       // unmappable key: leave the latch alone
    if ((GetAsyncKeyState((int)vk) & 0x8000) == 0) AimHoldRelease();
}

void AutoWalkToggle()
{
    if (!GameplayAndHud()) return;
    if (autowalk::IsWalking()) { autowalk::Stop(); return; }
    if (!HaveTarget()) return;
    const auto& e = g_scan_list[g_scan_index];
    if (!e.has_marker) {
        tolk::Speak(strings::T("To zadanie nie ma markera na mapie.",
                               "This quest has no map marker."),
                    tolk::Priority::System, true);
        return;
    }
    // A cross-worldspace target is fine here: autowalk::StartTo detects it and
    // routes through the connecting load doors (teleporting only if it can't),
    // so the player can always reach the marker.
    guide::Stop();      // the two modes are mutually exclusive
    // A quest marker walks in FOLLOW mode: the objective can advance while we're
    // still on the way, and then the cached position is the wrong destination.
    if (e.kind == game::WorldEntity::Kind::Quest && autowalk::StartToQuest())
        return;
    autowalk::StartTo(e.position, e.name, e.refr, e.form_id);
}

// Press/release the game's Use/Activate key (default E = scancode 0x12). FO3
// polls the keyboard once per frame in DirectInput immediate mode, so a down+up
// fired in the same instant falls between polls and is seen as "never pressed"
// (that's why held movement works but a tap didn't activate anything). So we
// hold the key DOWN for a few frames and release it.
void SendUse(bool down)
{
    // The key the PLAYER has bound, not the default. Pressing E on a keyboard
    // where Use lives somewhere else is a keypress into the void, and from the
    // outside that is indistinguishable from "activating does not work".
    UInt8 dik = game::GetUseKeyScancode();
    if (!dik) dik = 0x12;                        // DIK_E, the default
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = dik;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

int g_use_hold_ticks = 0;   // fallback path: frames to keep Use held

void ActivateTarget()
{
    if (!GameplayAndHud()) return;
    if (!HaveTarget()) return;
    const auto& e = g_scan_list[g_scan_index];
    // A quest objective IS activatable when it points at a real reference (its
    // target ref — the terminal / door / NPC the compass points to). Only a pure
    // map-marker with no ref can't be activated.
    if (e.kind == game::WorldEntity::Kind::Quest && !e.refr) {
        tolk::Speak(strings::T("Ten cel zadania to tylko marker — nie da się aktywować.",
                               "This objective is only a marker — it can't be activated."),
                    tolk::Priority::System, true);
        return;
    }
    // Prefer the console (never available in FO3, but harmless to try).
    if (console::Available() && e.form_id != 0) {
        char cmd[80];
        std::snprintf(cmd, sizeof(cmd), "%08X.activate player 1", e.form_id);
        console::Run(cmd);
        tolk::Speak(strings::T("Używam: ", "Using: ") + e.name, tolk::Priority::Ui, true);
        return;
    }
    // Best path: call the engine's native Activate(player) on our exact ref —
    // no aiming, no occlusion. It must run on the main thread, so queue it; the
    // DispatchMessageA hook runs it. Falls back to a Use keypress if we have no
    // live ref pointer (e.g. quests) or the native call is unavailable.
    if (e.refr && e.form_id) {
        game::QueueActivate(e.refr, e.form_id);
    } else {
        int r = config::Get().activate_pick_radius;
        if (r > 0) game::SetIniSettingFloat("fActivatePickSphereRadius", (float)r);
        SendUse(true);
        g_use_hold_ticks = 5;
    }
    tolk::Speak(strings::T("Używam: ", "Using: ") + e.name, tolk::Priority::Ui, true);
}

// View announce is no longer hooked to the F key. We poll the real camera flag
// (PlayerCharacter+0x5A8) every frame in polling_loop::PollViewChange and
// announce on change — robust to console/scripts/mods/auto camera changes, not
// just our own F press (the friend's point).

// Read the selected item's details (D) — in the Pip-Boy that's the stat card
// (weight, value, damage/DR, condition, effects, ammo). Silent in normal play
// so pressing D to strafe doesn't talk; only acts when a menu is open.
void ItemInfo()
{
    if (!poll::IsGameplayActive()) return;
    // Only where items have a stat card — inventory / container / barter /
    // repair / Pip-Boy. Everywhere else (dialogue, text entry, character
    // creation…) D is a normal key, so do NOTHING — don't hijack it and blurt
    // "Brak opisu" when the player is just typing a "d".
    auto m = menu::ActiveMenu();
    // In a conversation D repeats the other side's last line - voiced by the
    // game and gone once spoken, the text stays in the menu.
    if (m == menu::Id::Dialog) {
        std::string who  = game::GetCurrentDialogSpeaker();
        std::string line = game::GetCurrentDialogLine();
        if (line.empty())
            tolk::Speak(strings::T("Brak kwestii do powtórzenia.", "No line to repeat."),
                        tolk::Priority::Ui, true);
        else
            tolk::Speak(who.empty() ? line : who + ": " + line, tolk::Priority::Ui, true);
        return;
    }
    bool item_menu = (m == menu::Id::Inventory || m == menu::Id::Container ||
                      m == menu::Id::Barter     || m == menu::Id::Repair    ||
                      m == menu::Id::PipBoy     || m == menu::Id::Stats     ||
                      m == menu::Id::RepairServices);
    if (!item_menu) return;
    std::string info = game::GetSelectedItemInfo();
    if (!info.empty()) { tolk::Speak(info, tolk::Priority::Ui, true); return; }
    // No stat card here (e.g. container lists only names): re-read the name.
    // In the container it is the row of the column in use — the generic scan
    // can match the other column's highlight just as well.
    if (m == menu::Id::Container) {
        if (auto cs = game::GetContainerSelection()) {
            std::string t = cs->label;
            if (!cs->value.empty()) t += (t.empty() ? "" : ", ") + cs->value;
            if (!t.empty()) { tolk::Speak(t, tolk::Priority::Ui, true); return; }
        }
    }
    // In a conversation or a message box, with the row's place in the list,
    // as moving onto it says.
    const auto am = menu::ActiveMenu();
    auto sel = game::GetKeyboardSelectionText(am == menu::Id::Dialog || am == menu::Id::Message);
    if (sel && !sel->empty()) tolk::Speak(*sel, tolk::Priority::Ui, true);
    else tolk::Speak(strings::T("Brak opisu.", "No description."), tolk::Priority::System, true);
}

// HP / AP / radiation readout (H). We only REQUEST it here (poll thread); the
// actual GetActorValue reads happen on the main thread (calling game functions
// off the main thread crashes), see polling_loop AnnounceStatus().
void PlayerStatus()
{
    // Shift+H: the level and the experience to the next one, anywhere.
    if (hotkeys::ShiftHeldNow()) { poll::RequestLevelStatus(); return; }
    if (modules::barter::SpeakSummary()) return;   // in barter: the caps and the deal
    if (modules::sleepwait::SpeakSummary()) return; // sleep / wait: the time and the hours
    if (!GameplayAndHud()) return;   // the world only: silent in VATS and menus
    poll::RequestStatus();
}

void CrosshairInfo()
{
    if (!GameplayAndHud()) return;
    uint32_t id = 0, type = 0;
    std::string nm = game::GetCrosshairRefName(&id, &type);
    if (nm.empty()) {
        tolk::Speak(strings::T("Nic pod celownikiem.", "Nothing under the crosshair."),
                    tolk::Priority::System, true);
        return;
    }
    std::string msg = strings::T("Pod celownikiem: ", "Under the crosshair: ") + nm;
    // Is it the object currently selected in the scanner?
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size()) {
        uint32_t sel = g_scan_list[g_scan_index].form_id;
        msg += (sel && sel == id) ? strings::T(", to wybrany cel", ", the selected target")
                                  : strings::T(", inny obiekt", ", a different object");
    }
    char t[32];
    std::snprintf(t, sizeof(t), strings::T(", typ %u", ", type %u"), type);
    msg += t;
    tolk::Speak(msg, tolk::Priority::Ui, true);
}

// Aim the view (yaw + pitch) at a target so the first-person crosshair lands on
// it — for free-aim shooting where VATS is unavailable (the BB-gun target
// practice). Aims at the SELECTED scanner object if there is one (pick it with
// C/X + PgUp/PgDn, so there's no risk of aiming at a friendly NPC); otherwise
// at the nearest actor. You fire yourself; press G afterwards to hear what the
// crosshair caught (and tell me, so we can tune the pitch).
void CommaAim();   // defined below, beside the target picker

void AimAtTarget()
{
    if (!GameplayAndHud()) return;
    CommaAim();
}

// What Home / tracking should point at: the object selected in the scanner (any
// kind that has a position, quest markers included) or, failing that, the nearest
// actor. `up` lifts the aim above the origin so the crosshair lands on a body and
// not at the feet — without it, facing an NPC aims at their shoes and the
// "Rozmawiaj" prompt never appears.
struct AimTarget {
    game::Vec3  pos{};
    std::string name;
    float       up    = 0.0f;
    const void* refr  = nullptr;
    uint32_t    refid = 0;
    bool        actor   = false;   // a person or a creature
    bool        hostile = false;   // ...that the game counts as an enemy
};
// Where on a reference to aim. A scan entry carries the object's ORIGIN, and
// for an actor that is between its feet — hence the flat "+100 for a torso",
// which is a guess that is wrong for a child, a radroach and a Super Mutant in
// three different directions. game::GetAimPointFor asks the target's own
// skeleton instead (upper torso, neck, spine), the way the New Vegas mod
// resolves it. When that works the point is already absolute, so nothing is
// added on top; when it does not, the old guess still stands.
// A quest objective is a MARKER, and a marker is not a thing you can shoot or
// talk to. "Talk to Mr Brotch" puts its marker on or beside him, so aiming at
// the objective pointed the view at a spot in the air next to the man — which
// is why Home found nothing to interact with and End (which skips aiming
// entirely and activates the reference directly) was the only thing that
// worked. The FNV mod never has this problem because it picks its aim target
// from the engine's own actor list. We cannot do that for arbitrary objectives,
// but we can do the useful half: if an ACTOR is standing at the marker, the
// actor is what the objective means.
constexpr float kQuestActorReach = 250.0f;   // units from the marker

bool ActorAtQuestMarker(const game::Vec3& marker, game::WorldEntity* out)
{
    auto actors = game::ScanNearby(config::Get().nearby_scan_radius * 2, 24,
                                   /*actors_only*/true, false);
    const game::WorldEntity* best = nullptr;
    float bestd = kQuestActorReach * kQuestActorReach;
    for (const auto& a : actors) {
        if (!a.refr) continue;
        float dx = a.position.x - marker.x, dy = a.position.y - marker.y,
              dz = a.position.z - marker.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestd) { bestd = d2; best = &a; }
    }
    if (!best) return false;
    *out = *best;
    return true;
}

void FillAimPoint(AimTarget* out, const game::WorldEntity& e)
{
    // Redirect a quest marker onto whoever is standing at it, before anything
    // else looks at the entity.
    if (e.kind == game::WorldEntity::Kind::Quest && !game::RefIsActor(e.refr)) {
        game::WorldEntity who{};
        if (ActorAtQuestMarker(e.position, &who)) {
            F3A_INFO("AimTarget: quest marker '%s' resolved to the actor '%s' "
                     "standing at it", e.name.c_str(), who.name.c_str());
            FillAimPoint(out, who);
            return;
        }
    }
    out->name  = e.name;
    out->refr  = e.refr;
    out->refid = e.form_id;
    out->actor   = e.kind == game::WorldEntity::Kind::Actor || game::RefIsActor(e.refr);
    out->hostile = game::IsKnownHostile(e.form_id);
    const char* how = nullptr;
    game::Vec3 p{};
    if (e.refr && game::GetAimPointFor(e.refr, &p, &how)) {
        out->pos = p;
        out->up  = 0.0f;
        F3A_INFO("AimTarget: '%s' via %s at (%.0f,%.0f,%.0f), origin "
                 "(%.0f,%.0f,%.0f)", e.name.c_str(), how ? how : "?",
                 p.x, p.y, p.z, e.position.x, e.position.y, e.position.z);
        return;
    }
    out->pos = e.position;
    out->up  = (e.kind == game::WorldEntity::Kind::Actor) ? 100.0f : 0.0f;
}

bool PickAimTarget(AimTarget* out)
{
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
        g_scan_list[g_scan_index].has_marker) {
        FillAimPoint(out, g_scan_list[g_scan_index]);
        return true;
    }
    auto act = game::ScanNearby(config::Get().nearby_scan_radius * 2, 1,
                                true, false);   // nearest actor
    if (act.empty()) return false;
    FillAimPoint(out, act[0]);
    return true;
}


// ---- Continuous target tracking (Shift+Home) -------------------------------
//
// Home aims ONCE. That's enough for a door, but an NPC walks away and the
// crosshair is left pointing at empty ground. Tracking keeps re-aiming every
// tick so the camera follows a moving target — the FNV mod does the same on its
// aim button (retarget every 6 frames), which is what makes free-aim combat
// possible without sight. Toggle off with Shift+Home, or it drops itself when
// the target dies / leaves.
bool        g_track_on    = false;
const void* g_track_refr  = nullptr;
uint32_t    g_track_refid = 0;
std::string g_track_name;

// Aim the camera at a world point, lifting `up` units above its origin so the
// crosshair lands on a body rather than at the feet. Returns the distance.
float AimAtPoint(const game::Vec3& tgt, float up)
{
    auto pp = game::GetPlayerPosition();
    // Measure FROM the camera when the engine will tell us where it is. The
    // "+100 units above the feet" below is an invented eye height; the engine
    // keeps the real one, and a log of this very shot put the camera at z=7136
    // with the player's feet at 7041 — 95, not 100. Five units is a degree of
    // elevation at pistol range, and it is a degree we were adding for free.
    game::Vec3 eye{ pp.x, pp.y, pp.z + 100.0f };
    game::GetCameraPos(&eye);

    float dx = tgt.x - eye.x, dy = tgt.y - eye.y;
    float horiz = std::sqrt(dx * dx + dy * dy);
    const float R2D = 57.2957795f;
    float yawDeg  = std::atan2(dx, dy) * R2D;        // 0 = north(+Y), clockwise
    float dz      = (tgt.z + up) - eye.z;
    float pitchUp = std::atan2(dz, horiz > 1.0f ? horiz : 1.0f) * R2D;

    // Preferred path: the engine's own actor-facing routine, the one Command
    // Extender's FaceObject calls. It turns the player the way the game turns
    // any actor, instead of writing angle fields and hoping the rest of the
    // engine follows. The pitch write still goes on top, since facing a point
    // is about heading. [Voice] NativeFace = 0 falls back to angles only.
    if (config::Get().native_face) {
        game::Vec3 aim{ tgt.x, tgt.y, tgt.z + up };
        poll::RequestFacePoint(aim);
        poll::RequestAim(/*yaw*/false, 0.0f, /*pitch*/true, -pitchUp);
    } else {
        poll::RequestAim(/*yaw*/true, yawDeg, /*pitch*/true, -pitchUp);  // X inverted
    }
    return horiz;
}

// Learn the aim from someone who can see it.
//
// Everything this mod knew about aiming, it had written itself: the angles read
// back were the angles we set, so "the camera agrees with the body" proved
// nothing at all. A sighted player putting the crosshair on a target by hand is
// the one measurement that comes from outside — and from it the two unknowns
// fall out by arithmetic:
//
//   * the height a shot leaves the player at, which we had been GUESSING and
//     which is wildly different for the ten-year-old of the prologue, follows
//     from the observed pitch and the known range:
//         eye = target_z + range * tan(pitch)
//   * whether our idea of which way is "up" and which field the view actually
//     lives in matches the engine's, by comparing every rotation value the
//     engine holds against the one we would have set ourselves.
//
// Usage: pick the target with the scanner, have the sighted player aim at it,
// press the key. The measurement is applied at once and written to the log.
// A correction measured from a sighted aim: the vector from a reference's
// ORIGIN to the point a person actually puts the crosshair on. It is kept
// against the base form, so measuring one of three identical targets fixes all
// three — and it is the honest way round, because the origin of a placed object
// is wherever its model happens to be anchored, not its middle.
// Keyed by the base form's ID rather than its address, so the same corrections
// can be written to the INI and read back on the next launch. Measuring is a
// favour asked of someone who can see; asking for it again after every restart
// would make the feature worthless.
std::vector<config::AimOffset> g_aimoffs;

void ApplyAimOffset(AimTarget* at)
{
    if (!at || !at->refr || g_aimoffs.empty()) return;
    uint32_t id = game::BaseFormIdOf(at->refr);
    if (!id) return;
    for (const auto& o : g_aimoffs) {
        if (o.base_form_id != id) continue;
        at->pos.x += o.x;
        at->pos.y += o.y;
        at->pos.z += o.z;
        return;
    }
}

void RememberAimOffset(uint32_t id, const game::Vec3& off)
{
    for (auto& o : g_aimoffs) {
        if (o.base_form_id == id) { o.x = off.x; o.y = off.y; o.z = off.z; 
                                    config::SaveAimOffset(id, off.x, off.y, off.z);
                                    return; }
    }
    g_aimoffs.push_back({ id, off.x, off.y, off.z });
    config::SaveAimOffset(id, off.x, off.y, off.z);
}

void AimCalibrate()
{
    if (!GameplayAndHud()) return;

    poll::StopAimTrack();   // do not force an aim while measuring

    // With no target chosen, still sweep the view: "what can I shoot from
    // here" is useful on its own, and it is the question a player stuck at a
    // firing range actually has.
    AimTarget at;
    if (!PickAimTarget(&at)) {
        poll::RequestRayScan();
        return;
    }

    // Map what the engine's ray really finds around this target.
    //
    // The reference we were aiming at is never hit by ANY ray — ninety-six of
    // them failed — which means it has no collision at all: an invisible
    // trigger, not something a bullet can touch. Something else there IS solid,
    // and this is how we find out what it is and where.
    //
    // The sweep is REQUESTED, not run here. This function is called from the
    // key poller, which is not the game's thread, and casting the engine's rays
    // from it took the whole game down.
    poll::RequestRayMap(at.pos);
    poll::RequestRayScan();
}

// Turn to face the nearest enemy, and say who it is.
//
// The point is not precision — it is that VATS and a shot need the enemy to be
// IN FRONT of the player at all. When the ordinary aim has drifted, lost its
// target or picked the wrong thing, this puts the body back on the fight, and
// VATS can take it from there. Falls back to the nearest actor when nobody is
// actually in combat with us, because standing in a corridor with a radroach
// that has not noticed you yet still means turning towards the radroach.
void LockNearestEnemy()
{
    if (!GameplayAndHud()) return;
    int reach = config::Get().nearby_scan_radius * 3;
    auto host = game::ScanNearby(reach, 1, /*actors_only=*/true,
                                 /*hostiles_only=*/true);
    bool real_enemy = !host.empty();
    if (!real_enemy)
        host = game::ScanNearby(reach, 1, /*actors_only=*/true, false);
    if (host.empty()) {
        tolk::Speak(strings::T("Nikogo w pobliżu.", "No one nearby."),
                    tolk::Priority::Combat, true);
        return;
    }
    AimTarget at{};
    FillAimPoint(&at, host[0]);
    ApplyAimOffset(&at);
    AimAtPoint(at.pos, at.up);

    auto  pp = game::GetPlayerPosition();
    float dx = host[0].position.x - pp.x, dy = host[0].position.y - pp.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    F3A_INFO("LockEnemy: '%s' %s at %.0f units", host[0].name.c_str(),
             real_enemy ? "(fighting us)" : "(nearest actor, nobody in combat)",
             dist);
    // Turned towards it - nothing checked whether it is in the sights, so
    // "on target" is not claimed; holding the aim key says that.
    tolk::Speak(strings::T("Obrócony do: ", "Turned to: ") + host[0].name + ", " +
                    strings::FormatDistance(dist) +
                    (real_enemy ? std::string() : std::string(strings::T(", nie walczy", ", not fighting"))),
                tolk::Priority::Combat, true);
}


// What the AIM KEY locks onto. In a fight that is the enemy — never a quest
// marker, never a friend. The scanner selection is usually whatever was last
// browsed: a log showed the aim locking onto the objective "Wejdź do gabinetu
// Nadzorcy", 3900 units away through walls, while radroaches were biting — so
// every shot went into a wall and the only way to hit was to teleport next to
// them. Same rule as the FNV mod's AutoAim: actors fighting the player, the
// nearest first; a hostile the player picked in the scanner wins. With no
// fight on, the old choice stands (practice targets, a picked object).
//
// NEVER a person or creature who is not an enemy. With no fight on, the
// scanner's pick stands only if it is a thing (a practice target, an object):
// a log showed the aim key locking onto Butch's mother — the last thing walked
// to — while radroaches bit her, and four shots hit her. `why` says what to
// tell the player when nothing was taken.
bool PickCombatTarget(AimTarget* out, std::string* why = nullptr)
{
    auto host = game::ScanNearby(config::Get().nearby_scan_radius * 2, 8,
                                 /*actors_only=*/true, /*hostiles_only=*/true);
    if (!host.empty()) {
        if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size()) {
            uint32_t sel = g_scan_list[g_scan_index].form_id;
            for (const auto& h : host)
                if (sel && h.form_id == sel) { FillAimPoint(out, h); return true; }
        }
        FillAimPoint(out, host[0]);          // nearest enemy
        return true;
    }
    if (!PickAimTarget(out)) {
        if (why) *why = strings::T("Brak wroga w pobliżu.", "No enemy nearby.");
        return false;
    }
    // One exception: a creature the objective itself points at ("Zabij
    // radrakana za pomocą wiatrówki") before the game counts it as hostile.
    // Only the creature the objective itself names - not whoever stands near
    // its marker (Dogmeat by a quest marker is no target).
    bool objective = g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
                     g_scan_list[g_scan_index].kind == game::WorldEntity::Kind::Quest &&
                     g_scan_list[g_scan_index].form_id == out->refid;
    if (out->actor && !out->hostile && objective && game::RefIsCreature(out->refr)) {
        F3A_INFO("AimTarget: '%s' is a creature the objective points at - taken",
                 out->name.c_str());
        return true;
    }
    if (out->actor && !out->hostile) {
        F3A_INFO("AimTarget: '%s' is not an enemy - the aim key leaves them alone",
                 out->name.c_str());
        // Named only when the player picked them; the nearest bystander is
        // just "no enemy".
        bool picked = g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
                      g_scan_list[g_scan_index].has_marker;
        if (why) *why = picked ? out->name + strings::T(" nie jest wrogiem. Nie celuję.",
                                                        " is not an enemy. Not aiming.")
                               : std::string(strings::T("Brak wroga w pobliżu.",
                                                        "No enemy nearby."));
        return false;
    }
    return true;
}

// Turn the view onto a target and hold it there for `frames` main-thread
// frames (negative = until StopAimTrack) — the FNV mod's look-at: the middle
// of the body, measured from the camera, re-applied every frame so the
// engine's own view update cannot undo it.
void LockAimOn(AimTarget& at, int frames)
{
    ApplyAimOffset(&at);
    // TURN THE VIEW at once; the held aim keeps it there.
    AimAtPoint(at.pos, at.up);
    // A reference is preferred (it tracks a target that moves), but a bare
    // position is enough — and it is all a quest marker or a fixed practice
    // target ever has. Refusing those is what made the aim lock do nothing.
    poll::RequestAimTrack(at.refr, at.refid, at.pos, at.up, frames);
    // Then let the engine's rays have the last word on WHERE on the target a
    // shot can land. Only for a real reference: a bare quest-marker position
    // has nothing for a ray to confirm.
    if (at.refid) poll::RequestRayAimRefine(at.refid);
    g_hold_target = at.refid;
    poll::WatchAimLos(at.refid);     // the ray runs on the game's thread
}

bool LockAimOnCurrentTarget(std::string* outName, float* outDist)
{
    AimTarget at;
    std::string why;
    if (!PickCombatTarget(&at, &why)) {
        if (outName) *outName = why;
        return false;
    }
    if (outDist) {
        auto pp = game::GetPlayerPosition();
        auto b  = game::ComputeBearing(pp, game::GetPlayerYaw(), at.pos);
        *outDist = b.distance;
    }
    LockAimOn(at, kHoldAimFrames);
    if (outName) *outName = at.name;
    return true;
}

void TrackStop(const char* why)
{
    if (!g_track_on) return;
    g_track_on = false;
    g_track_refr = nullptr;
    g_track_refid = 0;
    if (!g_aim_held) poll::StopAimTrack();   // the aim key keeps its own lock
    if (why) tolk::Speak(why, tolk::Priority::Ui, true);
}

// `,` — turn the view onto a target, the FNV mod's way (LockAimOn), and keep
// it there long enough to fire. Enemies first: the one picked in the scanner,
// else the nearest. With no fight on, the practice targets of the vault's
// BB-gun objective, one per press in turn; else the scanner's pick or the
// nearest person. It used to aim at an enemy's FEET (its origin, one frame)
// and swept the practice targets with the mouse, firing an automatic burst.
constexpr int kCommaHoldFrames = 300;   // ~5 s: time to fire

void CommaAim()
{
    StopAimLoop();
    TrackStop(nullptr);
    AimTarget at;
    std::string label;
    auto host = game::ScanNearby(config::Get().nearby_scan_radius * 2, 8,
                                 /*actors_only=*/true, /*hostiles_only=*/true);
    if (!host.empty() && PickCombatTarget(&at)) {
        label = strings::T("Cel: ", "Target: ") + at.name;
        g_aim_cycle = 0;
    } else {
        auto tg = game::GetShootingTargets(4000);
        if (!tg.empty()) {
            if (g_aim_cycle >= (int)tg.size()) g_aim_cycle = 0;
            FillAimPoint(&at, tg[g_aim_cycle]);
            char idx[32];
            std::snprintf(idx, sizeof(idx), strings::T("Cel %d z %d", "Target %d of %d"),
                          g_aim_cycle + 1, (int)tg.size());
            label = idx;
            g_aim_cycle = (g_aim_cycle + 1) % (int)tg.size();
        } else if (PickAimTarget(&at)) {
            if (at.actor && !at.hostile) {
                // `,` is for shooting ("Strzelaj."): never at a friend.
                F3A_INFO("CommaAim: '%s' is not an enemy - not aiming", at.name.c_str());
                tolk::Speak(at.name + strings::T(" nie jest wrogiem. Nie celuję.",
                                                 " is not an enemy. Not aiming."),
                            tolk::Priority::Combat, true);
                return;
            }
            label = strings::T("Celuję w: ", "Aiming at: ") + at.name;
            g_aim_cycle = 0;
        } else {
            tolk::Speak(strings::T("Brak celu. Zaznacz cel skanerem (C lub X).",
                                   "No target. Select one with the scanner (C or X)."),
                        tolk::Priority::System, true);
            return;
        }
    }
    LockAimOn(at, kCommaHoldFrames);
    auto pp = game::GetPlayerPosition();
    float dist = game::ComputeBearing(pp, game::GetPlayerYaw(), at.pos).distance;
    F3A_INFO("CommaAim: %s (refid %08X) at (%.0f,%.0f,%.0f)", label.c_str(),
             (unsigned)at.refid, at.pos.x, at.pos.y, at.pos.z);
    tolk::Speak(label + ", " + strings::FormatDistance(dist) +
                    strings::T(". Strzelaj.", ". Fire."),
                tolk::Priority::Combat, true);
}

// Re-aim at the tracked target. Called every tick while tracking.
void TrackTick()
{
    if (!g_track_on) return;
    if (!GameplayAndHud()) { TrackStop(nullptr); return; }
    game::Vec3 p{};
    if (!game::GetRefPosition(g_track_refr, g_track_refid, &p)) {
        TrackStop(strings::T("Cel zniknął. Koniec śledzenia.", "Target lost. Tracking stopped."));
        return;
    }
    AimAtPoint(p, 100.0f);   // torso
}

// Center / level the view (Home) — the FNV mod's "center camera" done our way.
// Writing rotX doesn't move the first-person camera, so we converge the pitch to
// horizontal with mouse-Y feedback (same trick as the aim's pitch phase), target
// pitch 0. After leveling, an eye-level enemy lands on the crosshair the instant
// you face it; the LOS cue handles the fine sweep. First person only (in third
// person the mouse orbits the camera, not the look pitch).
// What Home says: the target's name and how far it is — "Radrakan, 123
// jednostek" — and nothing else. The same distance the scanner reads out.
void SayNameAndDistance(const AimTarget& at)
{
    auto  pp = game::GetPlayerPosition();
    auto  b  = game::ComputeBearing(pp, game::GetPlayerYaw(), at.pos);
    tolk::Speak(at.name + ", " + strings::FormatDistance(b.distance),
                tolk::Priority::Ui, true);
}

void CenterCamera()
{
    if (!GameplayAndHud()) return;
    StopAimLoop(); g_level_on = false;      // stop any mouse-driven aim/level

    // Alt+Home = TELEPORT to the selected object (last resort when you can't walk
    // there — doors, broken navmesh). Native MoveTo to the scanner pick's ref.
    //
    // Logged because a plain Home once teleported the player onto the rifle
    // range mid-session, which only makes sense if Alt was read as held. If it
    // happens again the log says so instead of looking like magic.
    if (hotkeys::AltActive()) {
        F3A_INFO("CenterCamera: Alt held -> TELEPORT branch.");
        // Prefer WHERE YOU'RE WALKING: if autowalk is active, teleport to its
        // destination — not the scanner selection, which drifts as DLC quests
        // auto-start/track (that's how Alt+Home landed on the Megaton bomb quest
        // instead of the tracked "Idąc jego śladami").
        if (autowalk::IsWalking() && autowalk::TargetRefr()) {
            std::string nm = autowalk::TargetName();
            const void* r  = autowalk::TargetRefr();
            uint32_t    rid = autowalk::TargetFormId();
            autowalk::Stop(); guide::Stop();
            poll::RequestTeleport(r, rid);
            tolk::Speak(strings::T("Teleportuję do: ", "Teleporting to: ") + nm,
                        tolk::Priority::Ui, true);
        } else if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
                   g_scan_list[g_scan_index].refr) {
            const auto& e = g_scan_list[g_scan_index];
            autowalk::Stop(); guide::Stop();
            poll::RequestTeleport(e.refr, e.form_id);
            tolk::Speak(strings::T("Teleportuję do: ", "Teleporting to: ") + e.name,
                        tolk::Priority::Ui, true);
        } else {
            tolk::Speak(strings::T("Najpierw wybierz obiekt skanerem, żeby się teleportować.",
                                   "Select an object with the scanner first to teleport."),
                        tolk::Priority::System, true);
        }
        return;
    }
    // Shift+Home = keep the camera ON the target while it moves (see TrackTick).
    if (hotkeys::ShiftActive()) {
        if (g_track_on) {
            TrackStop(strings::T("Koniec śledzenia.", "Tracking stopped."));
            return;
        }
        AimTarget at;
        if (!PickAimTarget(&at) || !at.refr) {
            tolk::Speak(strings::T("Nie ma kogo śledzić. Wybierz cel skanerem.",
                                   "Nothing to track. Select a target with the scanner."),
                        tolk::Priority::System, true);
            return;
        }
        g_track_on    = true;
        g_track_refr  = at.refr;
        g_track_refid = at.refid;
        g_track_name  = at.name;
        float d = AimAtPoint(at.pos, at.up);
        tolk::Speak(strings::T("Śledzę: ", "Tracking: ") + at.name + ", " +
                        strings::FormatDistance(d) +
                        strings::T(". Shift Home kończy.", ". Shift Home stops."),
                    tolk::Priority::Ui, true);
        return;
    }
    TrackStop(nullptr);   // a plain Home means "aim once" — drop any tracking

    // Home = native "center & aim" (the FNV flow): turn to face the selected
    // scanner target — or the nearest actor — AND pitch onto it, via the engine's
    // own SetAngle. With no target, just level the pitch. The turn-verify read-
    // back then says where the target ended up ("12 o'clock" = it landed).
    // Target: the selected scanner object (ANY kind WITH a marker, incl. quest
    // markers) or the nearest actor. Quest markers are now allowed so Home can
    // turn you toward the objective direction.
    AimTarget at;
    if (!PickAimTarget(&at)) {
        poll::RequestAim(/*yaw*/false, 0.0f, /*pitch*/true, 0.0f);   // just level
        tolk::Speak(strings::T("Poziomuję widok. Brak celu w pobliżu.",
                               "Leveling the view. No target nearby."),
                    tolk::Priority::Ui, true);
        return;
    }

    // Aim at the target's BODY, using only mechanisms proven separately:
    //   * the aim POINT comes from the object's own bounds (plain data, so a
    //     rifle-range target is aimed at its middle and not at the foot of its
    //     post — a fixed height offset only ever fitted one object);
    //   * HEADING goes through the engine's actor-turning routine, which is the
    //     part that already worked;
    //   * ELEVATION is driven by the MOUSE in a closed loop, because the camera
    //     follows the mouse and does NOT follow a written pitch value. That is
    //     why the view used to report the target dead ahead while the shot went
    //     somewhere else: the body had turned, the camera had not.
    // Nothing is fired: when the crosshair is on the target the mod says so and
    // the player shoots.
    if (at.refr) {
        // The FNV mod's look-at (LockAimOn): the body's middle, measured from
        // the camera, re-applied every frame and held — it used to be closed
        // by a mouse loop that only converged when nothing moved.
        StopAimLoop();
        F3A_INFO("Home aim: '%s' at (%.0f,%.0f,%.0f)",
                 at.name.c_str(), at.pos.x, at.pos.y, at.pos.z);
        LockAimOn(at, kHoldAimFrames);
        SayNameAndDistance(at);
        return;
    }
    // Plain GEOMETRIC aim: turn + pitch straight at the target via SetAngle (the
    // version proven on the door). No micro-scan sweep — that swung the crosshair
    // onto neighbours (wrong activation) and announced "na oko" late.
    ApplyAimOffset(&at);   // measured correction, if one was taken
    AimAtPoint(at.pos, at.up);
    // ...and KEEP it there. Not for a couple of seconds — until the target
    // changes, the weapon is lowered, or a walk starts. Lining up a shot
    // without sight takes as long as it takes, and an aim that quietly lapses
    // in the middle of it is worse than no aim at all: you would be shooting
    // at wherever the view had drifted to, with nothing to tell you.
    poll::RequestAimTrack(at.refr, at.refid, at.pos, at.up, kHoldAimFrames);
    SayNameAndDistance(at);
    g_turn_verify_pos   = at.pos;
    g_turn_verify_name  = at.name;
    g_turn_verify_quiet = true;
    g_turn_verify_at    = GetTickCount() + 450;
    g_turn_correct_left = kTurnMaxCorr;   // allow a couple of closed-loop nudges
}

} // namespace

bool AimHeld() { return g_aim_ctrl_was; }

// Drives the deferred Use-key release (single-press path) and the pitch sweep
// (floor-object path). Called every frame.
// The "on target" tone is the held aim's (a ray along the view, in Tick). A
// free-look cone used to beep at every enemy the view swept past - all through
// an auto-walk, which turns the view - and is gone: who is in the sights is
// told only while the player aims (] or the game's aim control).
int   g_lp_timer    = 0;                // lockpick beep pacing

void Tick(float)
{
    if (g_use_hold_ticks > 0 && --g_use_hold_ticks == 0)
        SendUse(false);

    // No "hit!" announcement here any more: it came from our own aim routine
    // guessing that the target reacted, not from the game reporting a hit, and
    // it fired on misses. A wrong call-out is worse than silence.
    poll::ConsumeAimTargetReacted();
    poll::ConsumeAimReleased();

    TrackTick();     // Shift+Home target tracking: re-aim at a moving target
    AimControlTick();
    AimHoldPoll();   // let go of the aim key -> lower the weapon
    {
        // Someone who is not an enemy stepped between the aim and its target.
        // Only while the player aims: a walk's look-at on arrival tracks a
        // target too, and "in the line of fire" then meant nothing.
        std::string who;
        if (poll::TakeLineOfFireWarning(&who) && g_aim_ctrl_was)
            tolk::Speak(strings::T("Uwaga, na linii strzału: ", "Warning, in the line of fire: ") +
                            who + ".",
                        tolk::Priority::Combat, true);
    }

    // Lockpick sweet-spot cue: beep faster + higher the closer the pin is to the
    // sweet spot; a steady high tone when ON it — then hold the force key (W) to
    // turn. Rotate with A/D and follow the beep by ear.
    {
        float off = 0.0f, tol = 1.0f;
        if (game::GetLockpickCue(&off, &tol)) {
            if (off <= tol) {                         // on the sweet spot
                if (--g_lp_timer <= 0) { audio::Cue(1200, 60); g_lp_timer = 3; }
            } else {
                float farness = off / 500.0f; if (farness > 1.0f) farness = 1.0f;
                int hz  = 500 + (int)((1.0f - farness) * 500.0f);  // 500..1000 Hz
                int gap = 4   + (int)(farness * 16.0f);            // 4..20 ticks
                if (--g_lp_timer <= 0) { audio::Cue(hz, 45); g_lp_timer = gap; }
            }
            return;   // lockpicking — skip the LOS/aim logic below
        }
    }

    // Turn-verify read-back (see TurnToCurrent): a beat after a native SetAngle
    // turn, say where the target now sits — "12 o'clock" means the turn landed.
    if (g_turn_verify_at != 0 && GetTickCount() >= g_turn_verify_at) {
        g_turn_verify_at = 0;
        if (GameplayAndHud()) {
            auto  pp  = game::GetPlayerPosition();
            float yaw = game::GetPlayerYaw();
            auto  b   = game::ComputeBearing(pp, yaw, g_turn_verify_pos);
            // Still off and correction budget left → nudge yaw straight at the
            // target again (absolute SetAngle from the fresh bearing) and re-check.
            if (std::fabs(b.relative_yaw) > kTurnGoodDeg && g_turn_correct_left > 0) {
                --g_turn_correct_left;
                float dx = g_turn_verify_pos.x - pp.x, dy = g_turn_verify_pos.y - pp.y;
                float yawDeg = std::atan2(dx, dy) * 57.2957795f;   // 0 = north(+Y)
                poll::RequestAim(/*yaw*/true, yawDeg, /*pitch*/false, 0.0f);
                g_turn_verify_at = GetTickCount() + 350;           // verify the fix
            } else {
                // Landed (or budget spent): announce where it ended up —
                // except after Home, which has already said all it says.
                F3A_INFO("Turn verify: '%s' now %.1f deg off",
                         g_turn_verify_name.c_str(), b.relative_yaw);
                if (!g_turn_verify_quiet)
                    tolk::Speak(g_turn_verify_name + strings::T(", teraz ", ", now ") +
                                strings::ClockDirection(b.relative_yaw),
                                tolk::Priority::Ui, true);
            }
        }
    }

    // Center/level the view (Home): converge the first-person pitch to horizontal
    // with mouse-Y. Takes precedence over the aim (CenterCamera clears g_aim_on)
    // and bails if an aim later starts.
    if (g_level_on && !g_aim_on) {
        if (!poll::IsGameplayActive() || game::IsThirdPerson()) {
            g_level_on = false;
        } else if (--g_level_budget <= 0) {
            g_level_on = false;
            tolk::Speak(strings::T("Widok wypoziomowany.", "View leveled."), tolk::Priority::Ui, true);
        } else {
            float cur = game::GetPlayerPitch();
            float err = -cur;                         // want pitch 0 (horizontal)
            if (std::fabs(err) <= 1.5f) {
                g_level_on = false;
                tolk::Speak(strings::T("Widok wypoziomowany.", "View leveled."), tolk::Priority::Ui, true);
            } else {
                if (g_level_started &&
                    std::fabs(err) > std::fabs(g_level_prev) + 0.3f)
                    g_level_sign = -g_level_sign;     // auto-calibrate mouse-Y dir
                g_level_prev    = err;
                g_level_started = true;
                long mv = (long)(err * 6.0f) * g_level_sign;
                if (mv >  150) mv =  150;
                if (mv < -150) mv = -150;
                if (mv > -3 && mv < 3) mv = (err > 0 ? 3 : -3) * g_level_sign;
                MouseMoveRel(0, mv);
            }
        }
    }

    // While the aim key is HELD, the cue answers one question only: would a
    // shot fired now hit what I locked onto? That is asked with a RAY along the
    // view - the same thing the New Vegas mod does (it calls JIP's raycast for
    // this, not the crosshair field).
    //
    // The crosshair field was tried here first and was wrong: a log caught it
    // reporting the same reference at the same 170 units through a 21-degree
    // swing of the aim. It does not follow the view, so the "target obscured,
    // 170 units" this announced the instant the key went down was not an
    // obstruction at all - it was a stale value, repeated every time.
    if (g_aim_ctrl_was && g_hold_target != 0 && config::Get().target_cue &&
        GameplayAndHud()) {
        // The ray itself is cast on the GAME's thread and the answer read back
        // here. This module runs on the polling thread, and casting an engine
        // ray from here is what crashed the game the moment the aim key was
        // pressed — the project had already written that rule down, and this
        // code broke it anyway.
        uint32_t hitId = 0;
        float    hitDist = 0.0f;
        bool     watching = poll::AimLosHit(&hitId, &hitDist);
        bool     on = watching && hitId == g_hold_target;
        g_hold_on_target = on;
        // No line of fire. Said out loud because the alternative is a player
        // standing behind a desk emptying a magazine into it with nothing to
        // explain why nothing happens.
        float bd = 0.0f, bh = 0.0f;
        if (poll::ConsumeNoLineOfFire(&bd, &bh)) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          strings::T("Brak linii strzału. Przeszkoda %.0f jednostek przed "
                                     "tobą, %.0f nad podłogą. Podejdź bliżej albo obejdź.",
                                     "No line of fire. Obstacle %.0f units ahead of you, "
                                     "%.0f above the floor. Move closer or go around."),
                          bd, bh);
            tolk::Speak(buf, tolk::Priority::Combat, true);
        }

        if (on) {
            // Keep sounding for as long as it stays true, so the player can
            // hold and squeeze rather than catch a single blip.
            // Pulsing, not a single blip: about six a second, so it reads as
            // a steady "yes, now" that you can hold a trigger against.
            if (--g_hold_cue_timer <= 0) {
                audio::Cue(config::Get().target_cue_hz, 45);
                g_hold_cue_timer = 2;
            }
        } else {
            g_hold_cue_timer = 0;   // silent the moment it stops being true
        }
    }

    // Mouse-feedback aim (see note at g_aim_on). Phase 0: turn yaw toward the
    // target with mouse-X until we're facing it. Phase 1: sweep pitch with
    // mouse-Y until the crosshair ref is the target, then say "na celu".
    if (g_aim_on) {
        // Any menu (VATS, Pip-Boy, pause, a load) ends it: mouse moves and an
        // attack key sent into a menu act on the menu.
        if (!GameplayAndHud()) { StopAimLoop(); return; }
        if (--g_aim_budget <= 0) {
            StopAimLoop();
            F3A_INFO("Aim loop: '%s' ran out of budget", g_aim_label.c_str());
            if (!g_aim_face_only)
                tolk::Speak(g_aim_label +
                    strings::T(", nie mogę namierzyć. Stań przodem do tarcz, pierwsza osoba.",
                               ", can't lock on. Face the targets, in first person."),
                    tolk::Priority::System, true);
            return;
        }
        if (g_aim_phase == 0) {                       // ---- yaw ----
            auto pp = game::GetPlayerPosition();
            float curYaw = game::GetPlayerYaw();
            auto br = game::ComputeBearing(pp, curYaw, g_aim_pos);
            float rel = br.relative_yaw;              // -180..180, + = right
            F3A_INFO("AimYaw rel=%.1f curYaw=%.1f budget=%d", rel, curYaw,
                     g_aim_budget);
            if (std::fabs(rel) <= 1.2f) {
                g_aim_phase = 1; g_aim_pstep = 0; g_aim_wait = 2;
                g_pitch_started = false; g_pitch_sign = 1;
                F3A_INFO("AimYaw converged -> pitch converge");
            } else {
                // Same overshoot protection as the pitch loop: back the step off
                // whenever the error flips sign, so a large turn is quick and the
                // last degree does not bounce.
                if (g_yaw_prev_set && rel * g_yaw_prev_err < 0.0f) {
                    g_yaw_gain *= 0.5f;
                    if (g_yaw_gain < 0.5f) g_yaw_gain = 0.5f;
                }
                g_yaw_prev_err = rel;
                g_yaw_prev_set = true;

                long dx = (long)(rel * g_yaw_gain);
                if (dx >  200) dx =  200;
                if (dx < -200) dx = -200;
                if (dx > -2 && dx < 2) dx = (rel > 0 ? 2 : -2);
                MouseMoveRel(dx, 0);
            }
        } else if (g_aim_phase == 1) {                // ---- pitch converge ----
            if (g_aim_wait > 0) { --g_aim_wait; }
            else {
                auto  pp    = game::GetPlayerPosition();
                float dx    = g_aim_pos.x - pp.x, dy = g_aim_pos.y - pp.y;
                float horiz = std::sqrt(dx*dx + dy*dy);
                const float kEye = 100.0f;            // eye/weapon above feet
                float dz      = g_aim_pos.z - (pp.z + kEye);
                float desired = std::atan2(dz, horiz > 1.0f ? horiz : 1.0f)
                                * 57.2957795f;        // + = target above eye
                float cur     = game::GetPlayerPitch();
                float err     = desired - cur;
                F3A_INFO("AimPitch step=%d desired=%.1f rotX=%.1f err=%.1f sign=%d",
                         g_aim_pstep, desired, cur, err, g_pitch_sign);

                // Auto-calibrate which mouse-Y direction reduces the error...
                if (g_pitch_started &&
                    std::fabs(err) > std::fabs(g_pitch_prev_err) + 0.3f) {
                    g_pitch_sign = -g_pitch_sign;
                }
                // ...and back off when we jumped over the target: an error that
                // changed sign means the last move was too big.
                if (g_pitch_started && err * g_pitch_prev_err < 0.0f) {
                    g_pitch_gain *= 0.5f;
                    if (g_pitch_gain < 0.6f) g_pitch_gain = 0.6f;
                }
                g_pitch_prev_err = err;
                g_pitch_started  = true;

                if (std::fabs(err) <= 1.5f || g_aim_pstep >= 60) {
                    bool onTarget = std::fabs(err) <= 1.5f;
                    if (g_aim_face_only) {
                        // Home: aiming is the whole task, and Home has already
                        // said the name and distance — nothing more is spoken.
                        g_aim_on = false;
                        F3A_INFO("Home aim: '%s' %s", g_aim_label.c_str(),
                                 onTarget ? "on target" : "pitch did not converge");
                        return;
                    }
                    // Aimed at the target's height — start the auto-fire burst.
                    // It gets its own time: 21 shots of ~6 ticks each would
                    // outlast what is left of the aim's budget.
                    g_aim_phase    = 2;
                    g_aim_budget   = kSprayCount * 8 + 20;
                    g_fire_left    = kSprayCount;
                    g_fire_holding = false;
                    g_spray_px = 0; g_spray_py = 0;
                    g_aim_wait     = 0;
                    tolk::Speak(g_aim_label + strings::T(", strzelam.", ", firing."),
                                tolk::Priority::Ui, true);
                } else {
                    long mv = (long)(err * g_pitch_gain) * g_pitch_sign;
                    if (mv >  150) mv =  150;
                    if (mv < -150) mv = -150;
                    if (mv > -3 && mv < 3) mv = (err > 0 ? 3 : -3) * g_pitch_sign;
                    MouseMoveRel(0, mv);
                    ++g_aim_pstep;
                    g_aim_wait = 1;
                }
            }
        } else if (g_aim_phase == 2) {                // ---- auto-fire burst ----
            if (g_aim_wait > 0) { --g_aim_wait; }
            else if (g_fire_left <= 0) {
                FireKey(false);                       // ensure released
                MouseMoveRel(-g_spray_px, -g_spray_py); // recentre the view
                g_spray_px = 0; g_spray_py = 0;
                g_aim_on = false;
                tolk::Speak(g_aim_label +
                    strings::T(", oddałem serię. Sprawdź zadanie.",
                               ", burst fired. Check the quest."),
                    tolk::Priority::Ui, true);
            } else if (!g_fire_holding) {
                // Move to this grid point (relative to where we are) and fire.
                int i = kSprayCount - g_fire_left;
                SprayPt pt = SprayOffset(i);
                MouseMoveRel(pt.dx - g_spray_px, pt.dy - g_spray_py);
                g_spray_px = pt.dx;
                g_spray_py = pt.dy;
                FireKey(true);
                g_fire_holding = true;
                g_aim_wait = 3;                       // hold the trigger briefly
            } else {
                uint32_t ctype = 0;
                uint32_t cur = game::GetCrosshairRefID(&ctype);
                F3A_INFO("AimFire off=(%ld,%ld) crosshair=0x%08X type=%u want=0x%08X",
                         g_spray_px, g_spray_py, cur, ctype, g_aim_id);
                FireKey(false);
                g_fire_holding = false;
                --g_fire_left;
                g_aim_wait = 2;                       // gap before the next shot
            }
        }
    }
}

void Init()
{
    g_aimoffs = config::LoadAimOffsets();
    if (!g_aimoffs.empty())
        F3A_INFO("Aim corrections loaded from the INI: %u",
                 (unsigned)g_aimoffs.size());

    const auto& h = config::Get().hotkeys;
    hotkeys::Bind(h.scan_nearby,   &ScanAll);
    hotkeys::Bind(h.scan_hostiles, &ScanHostiles);
    hotkeys::Bind(h.scan_next,     &ScanNext);
    hotkeys::Bind(h.scan_prev,     &ScanPrev);
    hotkeys::Bind(h.turn_to,       &TurnToCurrent);
    hotkeys::Bind(h.guide_beacon,  &GuideToggle);
    hotkeys::Bind(h.auto_walk,     &AutoWalkToggle);
    hotkeys::Bind(h.activate_target, &ActivateTarget);
    hotkeys::Bind(h.crosshair_info,  &CrosshairInfo);
    hotkeys::Bind(h.view_toggle,     &poll::RequestViewAnnounce);  // F → announce settled POV
    hotkeys::Bind(h.player_status,   &PlayerStatus);
    hotkeys::Bind(h.item_info,       &ItemInfo);
    hotkeys::Bind(h.aim_target,      &AimAtTarget);
    hotkeys::Bind(h.lock_enemy,      &LockNearestEnemy);
    hotkeys::Bind(h.center_camera,   &CenterCamera);
    hotkeys::Bind(h.drop_item,       &DropItem);
    hotkeys::Bind(h.press_row,       &PressRow);
    hotkeys::Bind(h.aim_hold,        &AimHoldPress);
    hotkeys::Bind(h.aim_calibrate,   &AimCalibrate);
    hotkeys::Bind(h.aim_up,          &AimTrimUp);
    hotkeys::Bind(h.aim_down,        &AimTrimDown);
    F3A_INFO("World scan module ready.");
}
void Shutdown() {}

// Clear all per-session cache so a new game / loaded save never reports objects
// from the previous one. Called when the Loading screen closes (polling_loop).
// The cell pointer alone can't be trusted to detect this — the engine may reuse
// the freed cell's address — so we wipe explicitly.
void ResetSession()
{
    g_scan_full.clear();
    g_scan_list.clear();
    g_scan_index   = -1;
    g_category     = Cat_All;
    g_scan_cell    = nullptr;
    g_aim_cycle    = 0;
    StopAimLoop();
    g_level_on     = false;
    g_turn_verify_at    = 0;
    g_turn_correct_left = 0;
    AimHoldRelease();
    g_track_on     = false;
    g_track_refr   = nullptr;
    g_track_refid  = 0;
    F3A_INFO("World scan session reset (new game / load).");
}

void StopInput()
{
    // The mod is being switched off: its Tick stops running, so nothing it
    // holds down (attack key, aim button) would ever be let go.
    StopAimLoop();
    g_level_on = false;
    AimHoldRelease();
    g_track_on = false;
}

} // namespace f3a::modules::worldscan
