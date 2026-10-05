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
    if (e.lock_level >= 250) return ", zamknięte na klucz";
    int L = e.lock_level;
    const char* d = (L < 25) ? "bardzo łatwy" : (L < 50) ? "łatwy" :
                    (L < 75) ? "średni"       : (L < 100) ? "trudny" : "bardzo trudny";
    return std::string(", zamknięte, zamek ") + d;
}

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

    std::string name = e.name + LockDesc(e);   // append lock state for doors/containers
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
        tolk::Speak("Skaner niedostępny tutaj.", tolk::Priority::System, true);
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
    char idx[16];
    std::snprintf(idx, sizeof(idx), "%d z %d",
                  g_scan_index + 1, (int)g_scan_list.size());

    // Active quests with no compass marker ("go talk to X") have no bearing —
    // announce the name only so the distance/direction isn't bogus.
    if (!e.has_marker) {
        tolk::Speak(e.name + ", bez markera, " + idx, tolk::Priority::Ui, true);
        return;
    }

    auto pos  = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto b    = game::ComputeBearing(pos, yaw, e.position);
    std::string line = e.name + LockDesc(e) + ", " +
                       strings::FormatDistance(b.distance) + ", " +
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
void CategoryStep(int dir)
{
    g_category = (g_category + dir + Cat_COUNT) % Cat_COUNT;
    if (ScanListStale()) Rescan(); else Refilter();
    tolk::Speak(CatName(g_category), tolk::Priority::Ui, true);
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
        tolk::Speak("Brak celu w pobliżu.", tolk::Priority::System, true);
        return;
    }

    // NATIVE SetAngle yaw (friend's lead): face the target in one shot via the
    // engine's own SetAngle Z, on the main thread. Works in first & third person
    // (replaces the old mouse-turn). If it snaps to face the target, native yaw
    // works and we can retire the mouse turn.
    g_aim_on = false;
    auto pp = game::GetPlayerPosition();
    float dx = tgt.x - pp.x, dy = tgt.y - pp.y;
    float yawDeg = std::atan2(dx, dy) * 57.2957795f;   // 0 = north(+Y), clockwise
    poll::RequestAim(/*yaw*/true, yawDeg, /*pitch*/false, 0.0f);
    tolk::Speak("Obracam do: " + name, tolk::Priority::Ui, true);
    // Verify ~450 ms later: announce where the target now sits, so we can hear if
    // the native turn actually landed it ahead (≈ "godzina dwunasta").
    g_turn_verify_pos   = tgt;
    g_turn_verify_name  = name;
    g_turn_verify_at    = GetTickCount() + 450;
    g_turn_correct_left = kTurnMaxCorr;   // allow a couple of closed-loop nudges
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
    const auto& e = g_scan_list[g_scan_index];
    if (!e.has_marker) {
        tolk::Speak("To zadanie nie ma markera na mapie.",
                    tolk::Priority::System, true);
        return;
    }
    // The target's marker position is in ITS worldspace. If that isn't the
    // player's (a location out in the wasteland while we're in Megaton, say),
    // walking/beaconing toward those raw coordinates heads off in a meaningless
    // direction — send the player to teleport instead of leading them astray.
    if (e.refr && !game::TargetSharesPlayerSpace(e.refr)) {
        tolk::Speak("Cel jest w innej lokacji — nie dojdziesz tam pieszo stąd. "
                    "Użyj teleportu (Alt+Home) albo najpierw wyjdź na zewnątrz.",
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
        tolk::Speak("Najpierw otwórz ekwipunek.", tolk::Priority::System, true);
        return;
    }
    poll::RequestDropItem();
}

// Press the highlighted row in whatever menu is open (Insert).
//
// This exists for key rebinding: Fallout's controls page starts listening for a
// new key only when the row is CLICKED, so with the keyboard alone a blind
// player could reach the row, hear it, and then be stuck. It is deliberately
// generic — any list that needs a click rather than Enter benefits.
void PressRow()
{
    if (menu::ActiveMenu() == menu::Id::None) {
        tolk::Speak("Ta funkcja działa w menu.", tolk::Priority::System, true);
        return;
    }
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
bool LockAimOnCurrentTarget(std::string* outName);

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
        tolk::Speak("Najpierw wybierz cel klawiszem celowania.",
                    tolk::Priority::System, true);
        return;
    }
    float total = poll::NudgeAim(units);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s, poprawka %+.0f",
                  units > 0 ? "Wyżej" : "Niżej", total);
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
void AimHoldPress()
{
    if (!GameplayAndHud() || g_aim_held) return;
    g_aim_held = true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &in, sizeof(in));

    // Raising the weapon also LOCKS the aim onto the target and keeps it there,
    // frame by frame, until the weapon is lowered. That is what makes shooting
    // possible without a mouse: the gun has to still be on the target when the
    // trigger is pulled, not when we announced it.
    g_hold_target = 0;
    g_hold_on_target = false;
    g_hold_cue_timer = 0;
    std::string name;
    if (!LockAimOnCurrentTarget(&name)) {
        // Nothing to lock onto — the button still goes down, so ordinary aiming
        // works, but there is no tracking to report.
        F3A_INFO("AimHold: no target to lock");
    }
}

// Release the aim if it is still latched — also called when gameplay ends, so
// the button is never left stuck down.
void AimHoldRelease()
{
    if (!g_aim_held) return;
    g_aim_held = false;
    g_hold_target = 0;
    g_hold_on_target = false;
    g_hold_cue_timer = 0;
    poll::StopAimTrack();
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &in, sizeof(in));
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
        tolk::Speak("To zadanie nie ma markera na mapie.",
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
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = 0x12;                          // DIK_E
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
        tolk::Speak("Ten cel zadania to tylko marker — nie da się aktywować.",
                    tolk::Priority::System, true);
        return;
    }
    // Prefer the console (never available in FO3, but harmless to try).
    if (console::Available() && e.form_id != 0) {
        char cmd[80];
        std::snprintf(cmd, sizeof(cmd), "%08X.activate player 1", e.form_id);
        console::Run(cmd);
        tolk::Speak("Używam: " + e.name, tolk::Priority::Ui, true);
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
    tolk::Speak("Używam: " + e.name, tolk::Priority::Ui, true);
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
    bool item_menu = (m == menu::Id::Inventory || m == menu::Id::Container ||
                      m == menu::Id::Barter     || m == menu::Id::Repair    ||
                      m == menu::Id::PipBoy);
    if (!item_menu) return;
    std::string info = game::GetSelectedItemInfo();
    if (!info.empty()) { tolk::Speak(info, tolk::Priority::Ui, true); return; }
    // No stat card here (e.g. container lists only names): re-read the name.
    auto sel = game::GetKeyboardSelectionText();
    if (sel && !sel->empty()) tolk::Speak(*sel, tolk::Priority::Ui, true);
    else tolk::Speak("Brak opisu.", tolk::Priority::System, true);
}

// HP / AP / radiation readout (H). We only REQUEST it here (poll thread); the
// actual GetActorValue reads happen on the main thread (calling game functions
// off the main thread crashes), see polling_loop AnnounceStatus().
void PlayerStatus()
{
    if (!poll::IsGameplayActive()) return;
    poll::RequestStatus();
}

void CrosshairInfo()
{
    if (!GameplayAndHud()) return;
    uint32_t id = 0, type = 0;
    std::string nm = game::GetCrosshairRefName(&id, &type);
    if (nm.empty()) {
        tolk::Speak("Nic pod celownikiem.", tolk::Priority::System, true);
        return;
    }
    std::string msg = "Pod celownikiem: " + nm;
    // Is it the object currently selected in the scanner?
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size()) {
        uint32_t sel = g_scan_list[g_scan_index].form_id;
        msg += (sel && sel == id) ? ", to wybrany cel" : ", inny obiekt";
    }
    char t[32];
    std::snprintf(t, sizeof(t), ", typ %u", type);
    msg += t;
    tolk::Speak(msg, tolk::Priority::Ui, true);
}

// Aim the view (yaw + pitch) at a target so the first-person crosshair lands on
// it — for free-aim shooting where VATS is unavailable (the BB-gun target
// practice). Aims at the SELECTED scanner object if there is one (pick it with
// C/X + PgUp/PgDn, so there's no risk of aiming at a friendly NPC); otherwise
// at the nearest actor. You fire yourself; press G afterwards to hear what the
// crosshair caught (and tell me, so we can tune the pitch).
void AimAtTarget()
{
    if (!GameplayAndHud()) return;

    // Combat first: if any HOSTILE is nearby, aim at the NEAREST one regardless
    // of a (possibly stale) scanner selection. This is an instant geometric aim
    // (SetAngle), so WASD keeps working — reposition freely, then shoot. Repeat
    // the key to re-lock a moving enemy (e.g. a charging dog).
    {
        auto host = game::ScanNearby(config::Get().nearby_scan_radius * 2, 1,
                                     /*actors_only=*/true, /*hostiles_only=*/true);
        if (!host.empty()) {
            game::AimLookAt(host[0].position);
            auto pp = game::GetPlayerPosition();
            float dx = host[0].position.x - pp.x, dy = host[0].position.y - pp.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            tolk::Speak("Cel: " + host[0].name + ", " +
                        strings::FormatDistance(dist) + ". Strzelaj.",
                        tolk::Priority::Combat, true);
            g_aim_cycle = 0;
            return;
        }
    }

    game::Vec3 tgt{}; std::string name; bool have = false;

    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
        g_scan_list[g_scan_index].has_marker &&
        g_scan_list[g_scan_index].kind != game::WorldEntity::Kind::Quest) {
        // 1) A scanner object you picked with C/X + PgUp/PgDn.
        tgt  = g_scan_list[g_scan_index].position;
        name = g_scan_list[g_scan_index].name;
        have = true;
        g_aim_cycle = 0;
    } else {
        // 2) "Shoot the N targets": the targets are unnamed (scanner-invisible)
        //    but the objective marks one, and the rest share its base object.
        //    Aim at one per press and CYCLE, so you can hit all of them with
        //    repeated `,` + shoot.
        auto tg = game::GetShootingTargets(4000);
        if (!tg.empty()) {
            F3A_INFO("AimTargets: %d found", (int)tg.size());
            for (size_t i = 0; i < tg.size(); ++i)
                F3A_INFO("  target[%d] id=0x%08X pos=(%.0f,%.0f,%.0f)",
                         (int)i, tg[i].form_id, tg[i].position.x,
                         tg[i].position.y, tg[i].position.z);
            if (g_aim_cycle >= (int)tg.size()) g_aim_cycle = 0;
            const auto& t = tg[g_aim_cycle];
            char idx[24];
            std::snprintf(idx, sizeof(idx), "Cel %d z %d", g_aim_cycle + 1,
                          (int)tg.size());
            bool third = game::IsThirdPerson();
            F3A_INFO("AimStart %s third=%d id=0x%08X", idx, (int)third, t.form_id);
            // Don't hard-bail on third person any more — always run the aim (and
            // log it). If the POV read is wrong it shouldn't block aiming; if
            // it's right, the mouse turn just works less well and we'll see it.
            g_aim_on    = true;
            g_aim_id    = t.form_id;
            g_aim_pos   = t.position;
            g_aim_phase = 0;
            g_aim_face_only = false;
            g_aim_budget = 150;
            g_aim_pstep  = 0;
            g_aim_wait   = 0;
            g_aim_label  = idx;
            tolk::Speak(std::string(idx) + ", celuję.", tolk::Priority::Ui, true);
            g_aim_cycle = (g_aim_cycle + 1) % (int)tg.size();
            return;
        }
        // 3) The nearest actor as a last resort.
        auto act = game::ScanNearby(config::Get().nearby_scan_radius * 2, 1,
                                    true, false);   // actors only, nearest
        if (!act.empty()) { tgt = act[0].position; name = act[0].name; have = true; }
        g_aim_cycle = 0;
    }
    if (!have) {
        tolk::Speak("Brak celu. Zaznacz cel skanerem (C lub X).",
                    tolk::Priority::System, true);
        return;
    }

    game::AimLookAt(tgt);
    auto pp = game::GetPlayerPosition();
    float dx = tgt.x - pp.x, dy = tgt.y - pp.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    tolk::Speak("Celuję w: " + name + ", " + strings::FormatDistance(dist) +
                ". Strzelaj.", tolk::Priority::Ui, true);
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
};
bool PickAimTarget(AimTarget* out)
{
    if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
        g_scan_list[g_scan_index].has_marker) {
        const auto& e = g_scan_list[g_scan_index];
        out->pos   = e.position;
        out->name  = e.name;
        out->up    = (e.kind == game::WorldEntity::Kind::Actor) ? 100.0f : 0.0f;
        out->refr  = e.refr;
        out->refid = e.form_id;
        return true;
    }
    auto act = game::ScanNearby(config::Get().nearby_scan_radius * 2, 1,
                                true, false);   // nearest actor
    if (act.empty()) return false;
    out->pos   = act[0].position;
    out->name  = act[0].name;
    out->up    = 100.0f;            // an actor → aim at the torso
    out->refr  = act[0].refr;
    out->refid = act[0].form_id;
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
    float dx = tgt.x - pp.x, dy = tgt.y - pp.y;
    float horiz = std::sqrt(dx * dx + dy * dy);
    const float R2D = 57.2957795f;
    const float kEye = 100.0f;                       // eye/weapon height above feet
    float yawDeg  = std::atan2(dx, dy) * R2D;        // 0 = north(+Y), clockwise
    float dz      = (tgt.z + up) - (pp.z + kEye);
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

bool LockAimOnCurrentTarget(std::string* outName)
{
    AimTarget at;
    if (!PickAimTarget(&at)) return false;
    ApplyAimOffset(&at);
    // A reference is preferred (it tracks a target that moves), but a bare
    // position is enough — and it is all a quest marker or a fixed practice
    // target ever has. Refusing those is what made the aim lock do nothing.
    poll::RequestAimTrack(at.refr, at.refid, at.pos, at.up, kHoldAimFrames);
    // Then let the engine's rays have the last word on WHERE on the target a
    // shot can land. Only for a real reference: a bare quest-marker position
    // has nothing for a ray to confirm.
    if (at.refid) poll::RequestRayAimRefine(at.refid);
    g_hold_target = at.refid;
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

// Re-aim at the tracked target. Called every tick while tracking.
void TrackTick()
{
    if (!g_track_on) return;
    if (!GameplayAndHud()) { TrackStop(nullptr); return; }
    game::Vec3 p{};
    if (!game::GetRefPosition(g_track_refr, g_track_refid, &p)) {
        TrackStop("Cel zniknął. Koniec śledzenia.");
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
void CenterCamera()
{
    if (!GameplayAndHud()) return;
    g_aim_on = false; g_level_on = false;   // stop any mouse-driven aim/level

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
            autowalk::Stop(); guide::Stop();
            poll::RequestTeleport(r);
            tolk::Speak("Teleportuję do: " + nm, tolk::Priority::Ui, true);
        } else if (g_scan_index >= 0 && g_scan_index < (int)g_scan_list.size() &&
                   g_scan_list[g_scan_index].refr) {
            const auto& e = g_scan_list[g_scan_index];
            autowalk::Stop(); guide::Stop();
            poll::RequestTeleport(e.refr);
            tolk::Speak("Teleportuję do: " + e.name, tolk::Priority::Ui, true);
        } else {
            tolk::Speak("Najpierw wybierz obiekt skanerem, żeby się teleportować.",
                        tolk::Priority::System, true);
        }
        return;
    }
    // Shift+Home = keep the camera ON the target while it moves (see TrackTick).
    if (hotkeys::ShiftActive()) {
        if (g_track_on) { TrackStop("Koniec śledzenia."); return; }
        AimTarget at;
        if (!PickAimTarget(&at) || !at.refr) {
            tolk::Speak("Nie ma kogo śledzić. Wybierz cel skanerem.",
                        tolk::Priority::System, true);
            return;
        }
        g_track_on    = true;
        g_track_refr  = at.refr;
        g_track_refid = at.refid;
        g_track_name  = at.name;
        float d = AimAtPoint(at.pos, at.up);
        tolk::Speak("Śledzę: " + at.name + ", " + strings::FormatDistance(d) +
                    ". Shift Home kończy.", tolk::Priority::Ui, true);
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
        tolk::Speak("Poziomuję widok. Brak celu w pobliżu.",
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
        game::Vec3 point{};
        const char* how = "origin";
        if (!game::GetAimPointFor(at.refr, &point, &how)) point = at.pos;

        poll::RequestFacePoint(point);      // fast first pass on the heading
        g_aim_on        = true;             // then close BOTH angles by mouse
        g_aim_id        = at.refid;
        g_aim_pos       = point;
        g_aim_phase     = 0;                // yaw first, then pitch
        g_aim_face_only = true;             // converge pitch, then stop
        g_aim_budget    = 150;
        g_aim_pstep     = 0;
        g_aim_wait      = 2;
        g_pitch_started = false;
        g_pitch_sign    = 1;
        g_pitch_gain    = 6.0f;             // fresh gain for each aim
        g_yaw_gain      = 4.0f;
        g_yaw_prev_set  = false;
        g_aim_label     = at.name;
        F3A_INFO("Home aim: '%s' point=%s (%.0f,%.0f,%.0f)",
                 at.name.c_str(), how, point.x, point.y, point.z);
        tolk::Speak("Celuję w: " + at.name, tolk::Priority::Ui, true);
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
    tolk::Speak("Celuję w: " + at.name, tolk::Priority::Ui, true);
    g_turn_verify_pos   = at.pos;
    g_turn_verify_name  = at.name;
    g_turn_verify_at    = GetTickCount() + 450;
    g_turn_correct_left = kTurnMaxCorr;   // allow a couple of closed-loop nudges
}

} // namespace

// Drives the deferred Use-key release (single-press path) and the pitch sweep
// (floor-object path). Called every frame.
// Line-of-sight aim cue. In FIRST person the read-back yaw(rotZ)+pitch(rotX)
// reflect the camera direction, so we compute the angle from camera-forward to
// each nearby actor — a LONG-RANGE "crosshair on an enemy" check (the engine's
// own crosshair pick is short-range). Blips faster and higher-pitched the more
// centred, so you can sweep onto a target by ear. Third person (camera orbits
// independent of the body) falls back to the short-range pick.
std::vector<game::Vec3> g_cue_actors;   // cached actor positions
int   g_cue_refresh = 0;                // ticks until the actor list refresh
int   g_cue_timer   = 0;                // ticks until the next blip
uint32_t g_los_last = 0;                // 3rd-person fallback de-dupe
constexpr float kCueConeDeg = 6.0f;     // half-angle of the "on target" cone
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
    AimHoldPoll();   // let go of the aim key -> lower the weapon

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
                // Landed (or budget spent): announce where it ended up.
                tolk::Speak(g_turn_verify_name + ", teraz " +
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
            tolk::Speak("Widok wypoziomowany.", tolk::Priority::Ui, true);
        } else {
            float cur = game::GetPlayerPitch();
            float err = -cur;                         // want pitch 0 (horizontal)
            if (std::fabs(err) <= 1.5f) {
                g_level_on = false;
                tolk::Speak("Widok wypoziomowany.", tolk::Priority::Ui, true);
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
    if (g_aim_held && g_hold_target != 0 && config::Get().target_cue &&
        GameplayAndHud()) {
        // ~20 Hz, like the FNV mod's LOS loop: fast enough to notice the target
        // stepping behind cover, cheap enough to run while the gun is up.
        if (--g_hold_los_timer <= 0) {
            g_hold_los_timer = 2;
            uint32_t hitId = 0;
            float    hitDist = 0.0f;
            bool     have = game::RayPickAhead(&hitId, &hitDist);
            bool     on = have && hitId == g_hold_target;
            if (on != g_hold_on_target) {
                F3A_INFO("AimHold: ray %s %08X (hit %08X at %.0f units)",
                         on ? "ON" : "off", g_hold_target, hitId, hitDist);
                g_hold_on_target = on;
            }
        }
        if (g_hold_on_target) {
            // Keep sounding for as long as it stays true, so the player can
            // hold and squeeze rather than catch a single blip.
            if (--g_hold_cue_timer <= 0) {
                audio::Cue(config::Get().target_cue_hz, 55);
                g_hold_cue_timer = 3;
            }
        } else {
            g_hold_cue_timer = 0;   // silent the moment it stops being true
        }
    }

    // LOS cue: only in normal gameplay (not menus/VATS) and not while auto-aim
    // or leveling is driving the view. Suppressed while the aim key is held —
    // that case is answered above, by the game rather than by a cone.
    if (config::Get().target_cue && !g_aim_on && !g_level_on && !g_aim_held &&
        GameplayAndHud() &&
        !game::ArePlayerControlsDisabled()) {   // silent in scripted scenes (char creation)
        if (game::IsThirdPerson()) {
            // Camera != body in 3rd person — use the short-range engine pick.
            uint32_t id = game::GetCrosshairActorID();
            if (id != 0) {
                if (id != g_los_last || --g_cue_timer <= 0) {
                    audio::Cue(config::Get().target_cue_hz, 70);
                    g_cue_timer = 4;
                }
            } else g_cue_timer = 0;
            g_los_last = id;
        } else {
            // First person: angle from camera-forward to nearest actor.
            if (--g_cue_refresh <= 0) {
                // Hostiles only — don't beep at friendly NPCs (the "dziwne
                // pikanie" while just walking around town).
                auto act = game::ScanNearby(3000, 16, /*actors_only*/true,
                                            /*hostiles_only*/true);
                g_cue_actors.clear();
                for (auto& e : act) g_cue_actors.push_back(e.position);
                g_cue_refresh = 8;
            }
            auto pp = game::GetPlayerPosition();
            const float D2R = 0.01745329f;
            float yaw   = game::GetPlayerYaw()   * D2R;   // 0 = +Y, clockwise
            float pitch = game::GetPlayerPitch() * D2R;
            float cp = std::cos(pitch);
            float fx = std::sin(yaw) * cp, fy = std::cos(yaw) * cp,
                  fz = std::sin(pitch);
            float ex = pp.x, ey = pp.y, ez = pp.z + 100.0f;   // eye height
            float best = 999.0f;
            for (const auto& a : g_cue_actors) {
                float dx = a.x - ex, dy = a.y - ey, dz = (a.z + 50.0f) - ez;
                float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (len < 1.0f) continue;
                float dot = (fx*dx + fy*dy + fz*dz) / len;
                if (dot > 1.0f) dot = 1.0f; else if (dot < -1.0f) dot = -1.0f;
                float ang = std::acos(dot) * 57.2957795f;
                if (ang < best) best = ang;
            }
            if (best <= kCueConeDeg) {
                if (--g_cue_timer <= 0) {
                    float center = 1.0f - best / kCueConeDeg;       // 0..1
                    int hz = config::Get().target_cue_hz + (int)(center * 400.0f);
                    audio::Cue(hz, 55);
                    g_cue_timer = 2 + (int)((1.0f - center) * 8.0f);  // 2..10
                }
            } else {
                g_cue_timer = 0;
            }
        }
    }

    // Mouse-feedback aim (see note at g_aim_on). Phase 0: turn yaw toward the
    // target with mouse-X until we're facing it. Phase 1: sweep pitch with
    // mouse-Y until the crosshair ref is the target, then say "na celu".
    if (g_aim_on) {
        if (!poll::IsGameplayActive()) { g_aim_on = false; return; }
        if (--g_aim_budget <= 0) {
            g_aim_on = false;
            tolk::Speak(g_aim_label +
                ", nie mogę namierzyć. Stań przodem do tarcz, pierwsza osoba.",
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
                        // Home: aiming is the whole task. Report honestly
                        // whether the crosshair converged or the loop ran out.
                        g_aim_on = false;
                        tolk::Speak(g_aim_label +
                                    (onTarget ? ", na celu."
                                              : ", nie mogę ustawić pionu."),
                                    tolk::Priority::Ui, true);
                        return;
                    }
                    // Aimed at the target's height — start the auto-fire burst.
                    g_aim_phase    = 2;
                    g_fire_left    = kSprayCount;
                    g_fire_holding = false;
                    g_spray_px = 0; g_spray_py = 0;
                    g_aim_wait     = 0;
                    tolk::Speak(g_aim_label + ", strzelam.",
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
                    ", oddałem serię. Sprawdź zadanie.", tolk::Priority::Ui, true);
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
    g_aim_on       = false;
    g_level_on     = false;
    g_turn_verify_at    = 0;
    g_turn_correct_left = 0;
    AimHoldRelease();
    g_track_on     = false;
    g_track_refr   = nullptr;
    g_track_refid  = 0;
    g_cue_actors.clear();
    F3A_INFO("World scan session reset (new game / load).");
}

} // namespace f3a::modules::worldscan
