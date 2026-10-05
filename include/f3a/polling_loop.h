#pragma once

#include "f3a/game_access.h"   // game::Vec3 for RequestNativeAim

namespace f3a::poll {

// Starts a worker thread that ticks every ~kPollIntervalMs and dispatches:
//   - menu open/close transitions via menu::OnMenuOpen / OnMenuClose
//   - per-frame work via menu::OnTick(dt)
//   - hotkey polling via hotkeys::Poll()
// We use a dedicated thread instead of hooking Main::Update because polling
// the InterfaceManager singleton + bool array is read-only and safe.
void Start();
void Stop();

// Diagnostic: dump the active top-level menu tile tree to the log. Bound to
// the DumpMenuTree hotkey (F11 by default) — used to figure out the per-menu
// "selected" marker convention when the heuristic doesn't pick it up.
void DumpActiveMenuTree();

// True while the player is actually in the game world (HUDMain has opened
// and Start menu is not currently up). Used by game-state hotkeys to stay
// silent in the main menu / loading screens.
bool IsGameplayActive();

// Request an HP/AP/radiation readout (the H hotkey). The actual actor-value
// reads run on the main thread; safe to call from the poll/hotkey thread.
void RequestStatus();

// Request a menu back/close (Backspace). Menu::HandleClick runs on the main
// thread; safe to call from the poll/hotkey thread.
void RequestMenuBack();

// Request "restore default controls" (R, on the settings/controls page).
// Menu::HandleClick runs on the main thread; safe from the poll/hotkey thread.
void RequestRestoreDefaults();

// Request "skip current objective" (advance the tracked quest's stage). The
// SetStage call runs on the main thread; safe from the poll/hotkey thread.
void RequestSkipObjective();

// Request "cycle VATS body part" (clicks the in-menu Body Part button). The
// HandleClick runs on the main thread; safe from the poll/hotkey thread.
void RequestVatsBodyPart();

// Request a VATS button click on the main thread: 1 = body part, 2 = previous
// target, 3 = next target.
void RequestVatsClick(int code);

// Click a VATS limb tile on the main thread (selects it and queues a shot).
void RequestVatsLimbClick(const void* tile);

// Cycle the VATS body part `steps` times on the main thread. The menu's button
// only moves forward, so going back one means going round the rest of the way.
// Steer the game's cursor onto a limb marker (index into GetVatsLimbs order),
// optionally clicking it on arrival. In FO3's VATS the cursor is what picks the
// body part AND what queues the shot; the on-screen buttons for both are
// gamepad prompts that do nothing on a PC. Ignored unless VATS is open.
void RequestVatsPointAt(int limbIndex, bool clickOnArrival);

// True while that steering is still under way. The cursor passes OVER other
// limbs on its way, and the game selects each one as it goes — announcing them
// all would bury the limb the player actually asked for.
bool VatsPointBusy();

// Which limb VATS is aimed at, sampled on the main thread. False when VATS
// isn't showing any. Read from the game's own state, so it follows the player's
// own clicks as well as ours.
bool VatsPickState(game::VatsPick* out);

// Announce the first/third-person view a moment after the F key is pressed (once
// the camera zoom has settled). Called from the F hotkey on the worker thread;
// the read + speech happen on the main thread in PollViewChange.
void RequestViewAnnounce();

// Start the full mouse-free aim at a target (world point + its ref id): the
// main-thread state machine points the view via SetAngle, then verifies with
// GetCrosshairRef and micro-scans until the crosshair is on the target.
void RequestNativeAim(const game::Vec3& pos, uint32_t refid);

// Teleport the player to a reference (Alt+Home), main-thread MoveTo. Last-resort
// navigation when a target can't be reached on foot. NOTE: the engine refuses to
// move the player while a menu owns the screen — close it first.
void RequestTeleport(const void* refr);

// Turn the player toward a world point on the main thread, using the engine's
// own actor-facing routine (see game::FacePointNative).
void RequestFacePoint(const game::Vec3& p);

// Aim at a REFERENCE's body on the main thread: the aim point comes from its
// collision shape and the angles are measured from the camera (see
// game::AimAtReference). `refid` guards the pointer against a cell change.
void RequestAimAtRef(const void* refr, uint32_t refid);

// Press the world map's "travel to" button on the main thread, after a marker
// has been selected (clicking a marker only selects it).
void RequestMapTravel();

// Select a world-map marker on the main thread, by its own tile (MapMarker::tile).
void RequestMapMarkerClick(const void* markerTile);

// Drop the item selected in the inventory (clicks the menu's own Drop button).
void RequestDropItem();

// The quantity prompt's amount and maximum, sampled on the main thread each
// frame. False when that prompt isn't up. Modules MUST use this instead of
// reading the menu themselves: the prompt closes on the main thread under the
// player's own key press, and reading its tiles from the poll thread as they are
// freed crashed the game.
bool QuantityState(int* amount, int* maximum);

// VATS numbers sampled on the main thread each frame (see game::GetVatsInfo).
// False when VATS isn't open. Modules must use this rather than reading the
// menu themselves — VATS is dismissed with a key, and reading it from the poll
// thread races with the main thread freeing it.
bool VatsState(game::VatsInfo* out);

// Loot Menu Updated's on-HUD container list, sampled on the main thread each
// frame. False when it isn't showing. Same rule as VatsState/QuantityState: the
// loot mod rebuilds its item tiles while the player scrolls, so the poll thread
// must not read them directly.
bool LootMenuState(game::LootMenuInfo* out);

// Make the quest highlighted in the Pip-Boy's quest list the tracked one.
void RequestTrackQuest();

// Press the row the keyboard highlight is on, in whatever menu is open. This is
// how key rebinding becomes possible without sight: the controls page enters its
// "press a new key" mode only on a mouse click.
void RequestPressRow();

// Click a named tile inside a menu, on the main thread — the engine's own
// "the user pressed this" path.
void RequestMenuClick(uint32_t menuType, const std::string& tileName);


// Request a native SetAngle aim on the main thread (the friend's lead): point
// the player's view via the engine's own SetAngle. Degrees; yaw 0 = north, and
// pitch goes through SetAngle X where NEGATIVE = look up. Set only the axes you
// want changed.
void RequestAim(bool doYaw, float yawDeg, bool doPitch, float pitchDeg);

// Hold the aim ON a reference, re-applied every main-thread frame. A single
// aim cannot survive the engine's own view update, so anything that has to be
// still pointing at the target when the trigger is pulled goes through here.
// `frames` counts main-thread frames; negative means "until StopAimTrack()".
void RequestAimTrack(const void* refr, uint32_t refid, const game::Vec3& at,
                     float up, int frames);

// Apply the held aim for this frame. Called from the player-movement hook,
// which runs inside the engine's own update — see the definition for why the
// message-pump work queue is the wrong place for it.
void AimTick();
void StopAimTrack();
bool AimTrackActive();

// Trim the held aim up (+) or down (-) by `units` of height at the target.
// Returns the new total correction. The mod has no reliable way to see where a
// shot lands — the engine's crosshair reference does not follow the view — so
// the player's ear is the sensor and this is how they use it.
float NudgeAim(float units);
float AimCorrection();

// True once, when the reference being aimed at has visibly reacted — it moved,
// turned, or vanished. A shot that lands is the one thing neither of us could
// see, and without it we were both guessing whether the aim was wrong or the
// feedback was missing.
bool ConsumeAimTargetReacted();

// True once, when a held aim was released because the mouse took over.
bool ConsumeAimReleased();

// Ask for a ray map around a point. The sweep itself runs on the GAME's thread
// — casting the engine's rays from the polling thread crashed the game, which
// is the rule this mod has broken once before and should not break again.
void RequestRayMap(const game::Vec3& centre);

// Check a held aim against the engine's own rays and re-lock onto the exact
// spot one of them reached on that reference. The records say where an object's
// ORIGIN is, which can be inside the floor or at a creature's feet; a ray says
// where a bullet would actually land. Runs on the game's thread.
void RequestRayAimRefine(uint32_t refid);

// Sweep the rays across the view and report what can actually be shot. Runs on
// the game's thread; the result is spoken and logged.
void RequestRayScan();

// True while the main menu (Start) is open or hasn't been left yet via
// HUDMain. Used by polling to suppress phantom Pip-Boy sub-menu opens
// during the splash → main menu → loading transition.
bool IsInMainMenu();

inline constexpr int kPollIntervalMs = 80; // ~12 Hz; tweak via INI later

} // namespace f3a::poll
