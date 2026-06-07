#pragma once

// Thin layer between Fallout3Access modules and Fallout 3's internal types.
// All real memory reads live behind these accessors so module code stays
// readable. The implementation lives next to the FOSE-specific hook code in
// src/game_access.cpp (TODO file — wires offsets for runtime 1.7.0.3).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace f3a::game {

struct Vec3 { float x, y, z; };

struct ActorStatus {
    int   health      = 0;
    int   max_health  = 0;
    int   ap          = 0;
    int   max_ap      = 0;
    int   radiation   = 0;
    int   caps        = 0;
    int   level       = 1;
    int   xp_to_next  = 0;
    bool  in_combat   = false;
    std::string current_location;  // cell or worldspace name
    std::vector<std::string> crippled_limbs;
};

struct InventoryItem {
    std::string name;
    int         count       = 1;
    int         value_caps  = 0;
    float       weight      = 0.0f;
    int         condition   = 100; // 0..100
    bool        equipped    = false;
    uint32_t    form_id     = 0;
};

struct DialogOption {
    int         index;
    std::string text;
    std::string skill_req;     // "" if no requirement; e.g. "[Wymowa 50]"
    bool        available;
};

struct VatsTarget {
    std::string actor_name;
    std::string body_part;     // "głowa", "tors", "lewa ręka"...
    int         hit_chance;    // 0..100
    int         ap_cost;
    int         damage;
    bool        crippled;
};

struct Bearing { float distance; float relative_yaw; }; // yaw -180..180

// ---- Player ----

bool         IsPlayerValid();
ActorStatus  GetPlayerStatus();
Vec3         GetPlayerPosition();
float        GetPlayerYaw();           // 0..360, 0 = north
std::string  GetCurrentLocationName(); // "Megaton", "Wasteland"

// ---- Quest target ----

struct QuestTarget {
    std::string name;
    Vec3        position;
    bool        valid;
};
QuestTarget GetCurrentQuestTarget();

// ---- World scan ----

struct WorldEntity {
    enum class Kind { Actor, Container, Door, Item, Note, Quest };
    Kind        kind;
    std::string name;
    Vec3        position;
    bool        hostile  = false;
    bool        owned    = false;
    bool        locked   = false;
    uint32_t    form_id  = 0;
    const void* refr     = nullptr;   // live TESObjectREFR* (stale after cell change)
};

// Within `radius` game units of the player; up to `max_results` closest.
std::vector<WorldEntity> ScanNearby(int radius, int max_results,
                                    bool actors_only = false,
                                    bool hostiles_only = false);

// The TESQuest that the player's currently tracked objective belongs to, as an
// opaque pointer (BGSQuestObjective+0x10), plus its display name. ptr is null
// when there is no tracked objective. Used to announce quest changes.
const void* GetTrackedQuestPtr();
std::string GetTrackedQuestName();

// All currently-running quests that have a target marker, as WorldEntities
// (Kind::Quest, name = quest name, position = marker). Lets the player browse
// active quests and pick which to navigate to — instead of the game's
// auto-selected "latest" objective (which DLCs hijack).
std::vector<WorldEntity> GetActiveQuests();

// ---- Bearings ----

Bearing ComputeBearing(const Vec3& from, float from_yaw_deg, const Vec3& to);

// Rotate the player to face `target` (writes the player's yaw directly).
void SetPlayerYawTo(const Vec3& target);

// True if the player is in third-person view (PlayerCharacter+0x5A8).
bool IsThirdPerson();

// Current value of one of the player's actor values (e.g. 5..11 = SPECIAL,
// 12 = ActionPoints, 16 = Health, 54 = RadLevel). Returns -1 on failure.
// Calls the engine's GetActorValue via the avOwner vtable.
float GetPlayerAV(uint32_t avCode);

// Name of the reference currently under the crosshair (InterfaceManager+0xFC),
// "" if none. out_form_id receives its refID, out_type_id its base form type
// (both 0 if none). This is exactly what the game would activate with Use —
// invaluable for debugging whether we're aimed at the right object.
std::string GetCrosshairRefName(uint32_t* out_form_id = nullptr,
                                uint32_t* out_type_id = nullptr);

// refID of the reference under the crosshair (0 if none); out_type (optional)
// gets its base form type. Cheap enough to poll every frame — feedback for aim.
uint32_t GetCrosshairRefID(uint32_t* out_type = nullptr);

// Force the game's crosshair reference (InterfaceManager+0xFC) to `refr` so a
// Use press activates THAT object regardless of aim/occlusion. Validates the
// pointer is readable and its refID still matches expectedRefID (guards against
// a stale/reused pointer). Returns false if it refused to write.
bool ForceCrosshairRef(const void* refr, uint32_t expectedRefID);

// Activation by directly calling the engine's TESObjectREFR::Activate(player) —
// the Skyrim-style "activate by reference", no aiming/occlusion. MUST run on the
// game's main thread (it runs scripts / opens menus), so don't call it from the
// poll thread; queue it instead.
//
// Install the main-thread executor (IAT-hooks DispatchMessageA) once at load:
void InstallMainThreadHook();
// Queue an activation from any thread; it runs on the main thread next message
// dispatch. expectedRefID guards against a stale pointer.
void QueueActivate(const void* refr, uint32_t expectedRefID);

// Register a callback run every frame ON THE MAIN THREAD (from the DispatchMessageA
// hook), so it can safely call game functions like GetActorValue. Pass nullptr
// to clear. The callback must be cheap.
void SetMainThreadCallback(void (*cb)());

// Aim the player's view: write yaw (rotZ) and pitch (rotX) directly, the way
// SkyrimAccessMod's SetHeading/SetLooking aim the crosshair at a target. Angles
// in radians; yaw 0 = +Y (north), clockwise. Used to point the crosshair at an
// object for activation (first person).
void SetPlayerLook(float yawRad, float pitchRad);

// Set a float INI setting's in-memory value by name (e.g.
// "fActivatePickSphereRadius"). Searches both Fallout.ini and FalloutPrefs.ini
// collections; matches the name up to an optional ":Section" suffix. Returns
// true if a setting was found and written.
bool SetIniSettingFloat(const char* name, float value);

// Read a float INI setting's in-memory value. Returns true and fills `out` if
// found; false otherwise. Used to verify the collection addresses are right.
bool GetIniSettingFloat(const char* name, float& out);

// Set an integer/bool INI setting's in-memory value (stored in the same union;
// written as uint, not float). Returns true if the setting was found.
bool SetIniSettingInt(const char* name, uint32_t value);

// Opaque token identifying the player's current cell — changes when the
// player moves to another cell or a different save is loaded. Used to drop a
// stale scan list. nullptr if there is no player.
const void* GetPlayerCell();

// ---- Menus & UI state ----

// Generic accessor for "what is currently highlighted in the active menu".
// Modules can use this when they have no menu-specific way to ask the engine.
// Resolves via the mouse-hover activeTile pointer.
std::optional<std::string> GetActiveMenuSelectionText();

// Keyboard-driven selection: F3 menus mark the focused row with a floating
// highlight box. We locate it and return the focused row's label plus, for
// slider/meter rows, its current value (split so the caller can announce
// "label, value" on focus change but just "value" on a left/right tweak).
struct MenuSelection {
    std::string label;   // e.g. "Muzyka", "New", "Yes"
    std::string value;   // e.g. "25" for a slider; empty for plain rows
    // Static text of the surrounding panel — e.g. the question of a
    // confirmation box ("Rozpocząć nową grę?"). Announced once when focus
    // enters a new panel, before the focused row.
    std::string context;
    // Opaque identity of the panel (its listbox tile) so the caller can
    // detect "focus moved into a different panel".
    const void* container = nullptr;
};
std::optional<MenuSelection> GetKeyboardSelection();

// Convenience: the focused row as one string ("label" or "label, value").
// Used by the diagnostic dump.
std::optional<std::string> GetKeyboardSelectionText();

// "Go back one menu level": find the visible Back button in the open menu
// and click it through the game's own Menu::HandleClick — so the engine's
// panel-stack logic decides where back leads. Returns false if no visible
// back button was found (nothing to go back from).
bool ClickMenuBack();

// ---- Pip-Boy ----

enum class PipBoyTab { Stats, Items, Data };
PipBoyTab    GetActivePipBoyTab();
std::string  GetActivePipBoyTabName();
// Labels of the currently ACTIVE sub-tab buttons (the game dims the active
// tab strip button to alpha 32) — e.g. "Status, KND" on the Stats page.
std::string  GetActivePipBoySubTabName();
// "POZ. 1, PW 156/200, PA 79/79, PD 158/200" — read off the Stats page's
// info boxes. Empty when the Stats page isn't the visible Pip-Boy page.
std::string  GetPipBoyVitals();
std::optional<InventoryItem> GetSelectedInventoryItem();

// ---- Dialog ----

std::string                 GetCurrentDialogSpeaker();
std::string                 GetCurrentDialogLine();
std::vector<DialogOption>   GetDialogOptions();
int                         GetHighlightedDialogOption();

// ---- Barter ----

struct BarterState {
    int  player_total_value;
    int  vendor_total_value;
    int  player_caps;
    int  vendor_caps;
    bool player_side_active;     // true if focus is on player's inventory
    std::optional<InventoryItem> selected;
};
BarterState GetBarterState();

// ---- Lockpick ----

struct LockpickState {
    float pick_angle_deg;        // -90..90, 0 = vertical
    float sweet_spot_deg;        // the value pick_angle_deg should match
    float tension;               // 0..1
    int   picks_left;
    bool  broken;
    bool  unlocked;
};
LockpickState GetLockpickState();

// ---- VATS ----

std::vector<VatsTarget> GetVatsTargets();
int                     GetVatsSelectedIndex();
int                     GetVatsQueueLength();
int                     GetVatsQueueCapacity();

// Full text of an open Message popup (title + body + button labels), e.g.
// an OK notification or the character-creation gender prompt. Empty if no
// Message menu is visible.
std::string GetActiveMessageText();

// ---- Subtitles / voice lines ----

// Returns the most-recently-spoken NPC line if not yet announced.
std::optional<std::string> PollNewSubtitle();

} // namespace f3a::game
