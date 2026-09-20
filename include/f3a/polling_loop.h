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

// Make the quest highlighted in the Pip-Boy's quest list the tracked one.
void RequestTrackQuest();

// Click a named tile inside a menu, on the main thread — the engine's own
// "the user pressed this" path.
void RequestMenuClick(uint32_t menuType, const std::string& tileName);


// Request a native SetAngle aim on the main thread (the friend's lead): point
// the player's view via the engine's own SetAngle. Degrees; yaw 0 = north, and
// pitch goes through SetAngle X where NEGATIVE = look up. Set only the axes you
// want changed.
void RequestAim(bool doYaw, float yawDeg, bool doPitch, float pitchDeg);

// True while the main menu (Start) is open or hasn't been left yet via
// HUDMain. Used by polling to suppress phantom Pip-Boy sub-menu opens
// during the splash → main menu → loading transition.
bool IsInMainMenu();

inline constexpr int kPollIntervalMs = 80; // ~12 Hz; tweak via INI later

} // namespace f3a::poll
