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
float        GetPlayerPitch();         // look pitch, degrees (read-only feedback)
std::string  GetCurrentLocationName(); // "Megaton", "Wasteland"

// ---- Quest target ----

struct QuestTarget {
    std::string name;
    Vec3        position;
    bool        valid;
    const void* refr = nullptr;   // objective target ref (for cross-space checks)
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
    bool        has_marker = true;    // false = quest with no map-marker position
    uint32_t    form_id  = 0;
    const void* refr     = nullptr;   // live TESObjectREFR* (stale after cell change)
    int         lock_level = -1;      // -1 = no lock; 0-100 pick difficulty; >=250 = needs key
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

// Advance the tracked quest to its next defined stage — to skip a scripted
// objective that's inaccessible blind (forced free-aim, unreachable target).
// Runs the stage's result script via TESQuest::SetStage, so MUST run on the
// main thread. Returns false if there's no tracked quest or no later stage.
bool AdvanceTrackedQuestStage();

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

// refID of the crosshair ref if it's an actor (NPC/Creature), else 0 — for the
// line-of-sight aim cue (a sound when the crosshair is on an enemy).
uint32_t GetCrosshairActorID();

// ---- World map ----
//
// A map marker as the map screen shows it. `discovered` = the player has found
// it (fast travel allowed); `type` is the raw icon id (town/vault/cave/…).
struct MapMarker {
    std::string name;
    Vec3        pos{};            // world position (for distance / walking)
    const void* refr    = nullptr;
    uint32_t    form_id = 0;
    uint16_t    type    = 0;
    uint16_t    flags   = 0;      // raw MarkerData flags
    bool        discovered = false;
    float       dist    = 0.0f;   // from the player, game units
    // Where the marker sits on screen, from the map's own tile (the world map
    // draws each marker as a tile NAMED after the location). Lets us click it
    // for a real fast travel instead of teleporting. has_tile == false when no
    // matching tile was found (e.g. reading the list outside the map screen).
    bool        has_tile = false;
    float       screen_x = 0.0f;
    float       screen_y = 0.0f;
    // The marker's own tile. Kept so clicking it needs no name lookup: tile
    // names live in the game's code page (1250 here) while our names are UTF-8,
    // so matching by name silently failed for any location with a Polish
    // character. Valid only while the same map screen stays open.
    const void* tile = nullptr;
};

// Every marker the open map menu is showing, nearest first. Empty when the map
// isn't open (or the marker list couldn't be located). Reading it also refreshes
// the session cache below.
std::vector<MapMarker> GetMapMarkers();

// The markers seen the last time the map was open — usable during gameplay, so
// travel planning doesn't need the menu on screen. Empty until the player opens
// the map once.
const std::vector<MapMarker>& CachedMapMarkers();

// Screen position of the world map's cursor — feedback for steering the mouse
// onto a marker tile to click it (a real fast travel, dialog and all). False if
// the map isn't open.
bool GetMapCursor(float* x, float* y);

// Every prose line currently visible on the HUD — tutorials, hints and the
// game's "you can't do that because…" notices. Counters that tick constantly
// (health, AP, ammo, compass, clock) and the tiles other pollers already read
// are filtered out, so the caller can simply announce lines it hasn't seen yet.
std::vector<std::string> GetHudProse();

// What VATS knows beyond the tiles: action points (which decide how many shots
// you can queue), ammo, how many shots are already queued, and the hit chance
// from the body-part record. Each `has_*` flag says whether that value passed a
// sanity check — the field offsets come from New Vegas and are unverified here,
// so anything implausible is reported as absent rather than spoken.
struct VatsInfo {
    bool has_ap = false;     int ap = 0;          int max_ap = 0;
    bool has_ammo = false;   int clip_ammo = 0;   int reserve_ammo = 0;
    bool has_chance = false; int hit_chance = 0;
    int  queued = 0;         // shots already queued
};
bool GetVatsInfo(VatsInfo* out);

// Play a movement animation group on an actor, the way the game's own PlayGroup
// command does: 0 = Idle, 3 = Forward (walk), 7 = FastForward (run). Needed
// because moving the player directly plays no animation — and the game's
// footstep sounds ride on that animation. MAIN THREAD only. False if the address
// guards rejected this build.
bool PlayActorAnimGroup(void* actor, uint32_t animGroup);

// ---- Aiming at an object's real body ----
//
// The centre of a reference's COLLISION shape, in world coordinates — the point
// a bullet would meet. False when the object has no rigid body (many props) or
// its 3D isn't loaded. MUST run on the main thread: it calls into the model.
bool GetCollisionAABBCenter(const void* refr, float* outX, float* outY, float* outZ);

// The best available aim point for a reference: the collision centre, else the
// render bound's centre, else the origin lifted to chest height. `how` receives
// which one was used, so the choice can be reported instead of guessed at.
// MUST run on the main thread.
bool GetAimPointFor(const void* refr, Vec3* out, const char** how = nullptr);

// Where the rendering camera is. False if it can't be read or lands implausibly
// far from the player — callers then use the player's eye height instead.
bool GetCameraWorldPos(float* x, float* y, float* z);

// Point the view at a reference's body: aim point resolved as above, angles
// measured from the camera, heading applied through the engine's own turning
// routine and elevation through SetAngle. No fudge factors. MUST run on the
// main thread. `detail` receives which aim point was used.
bool AimAtReference(const void* refr, std::string* detail = nullptr);

// ---- VATS limbs ----
//
// One tile per body part, each with its own position, name and hit chance.
// The menu's body-part button is invisible on this build, so cycling limbs
// means moving between THESE, not pressing a button.
struct VatsLimb {
    const void* tile = nullptr;
    std::string name;      // "Korpus", "Prawe nogi", ...
    std::string chance;    // "95%"
    int  part_id = 0;      // the game's own body-part id, stable per limb
    bool selected = false; // this is the limb VATS is currently aimed at
    float x = 0.0f, y = 0.0f;
};
std::vector<VatsLimb> GetVatsLimbs();

// Click a limb's tile. Kept for diagnostics only: those tiles carry id 0 and no
// target trait, so this does nothing — queueing goes through ClickVatsSelect.
bool ClickVatsLimb(const void* tile);

// The limb VATS is currently aimed at, plus where it sits in the cycle. This
// is read from the GAME's own state rather than from an index of ours, so the
// player's own clicks and wheel are reflected too. Sampled on the main thread.
struct VatsPick {
    char name[64]   = {};
    char chance[16] = {};
    int  count = 0;    // distinct body parts on this target
    int  index = 0;    // 1-based position of the selected one; 0 = none shown
    int  queued = -1;  // shots on the VATS queue list; -1 = couldn't be read
    // The whole cycle, in order. Needed because `index` can legitimately be 0:
    // if the game ever stops marking a selection, the reader still has to say
    // SOMETHING rather than go silent, and it says this list's entry instead.
    char names[8][48] = {};
};
bool GetVatsPick(VatsPick* out);

// Shots currently on the VATS queue, counted from the on-screen list. -1 when
// it can't be read. Main thread only.
int GetVatsQueuedCount();

// Screen position of the selected limb's marker — where the cursor must be for
// a queueing click to land. False when nothing is selected. Main thread only.
bool GetVatsSelectedLimbPos(float* x, float* y);

// Queue a shot at the selected limb ("Wybierz", id 7). Main thread only.
bool ClickVatsSelect();

// ---- Loot Menu Updated (F3LootMenu.dll) ------------------------------------
// That mod draws a container's contents as an overlay ON the HUD rather than
// opening a menu, so none of our menu handlers ever see it; it keeps its state
// in custom tile traits named _JLM*. Reading those is the only way a blind
// player gets at a list that is, for everyone else, simply on screen.
//
// Strings are fixed buffers, not std::string: this is sampled on the game's
// thread and read on the polling thread, and handing a std::string across that
// boundary is a bug this project has already paid for once.
struct LootMenuInfo {
    bool visible   = false;
    char title[96] = {};     // container or corpse name
    char item[160] = {};     // the entry currently highlighted
    char weight[48]= {};     // carry weight, as the overlay shows it ("200/300")
    int  index     = 0;      // highlight position within the visible window
    int  offset    = 0;      // how far the list has scrolled
    int  total     = 0;      // entries in the container
    bool equipped  = false;  // the highlighted entry is worn/wielded
    bool stealing  = false;  // taking from here counts as theft
};

// Sample the overlay. False when it isn't showing. MUST run on the main thread
// — the loot mod rebuilds those item tiles as the player scrolls.
bool GetLootMenuInfo(LootMenuInfo* out);

// Replace the game's control markers in a piece of its text (e.g.
// "&-sUActnMenumode;") with the key the player has bound to that action. Hints
// from the tutorial carry these unexpanded on a keyboard, and read as gibberish
// otherwise. Returns the text unchanged when it has no markers.
std::string ExpandControlTokens(const std::string& text);

// Is the player holding the game's AIM control (whatever they bound it to)?
bool IsAimControlHeld();

// Is a weapon drawn?
bool IsWeaponOut();

// Turn the player toward a world point using the engine's OWN actor-facing
// routine (the one Command Extender's FaceObject calls). Goes through the
// actor's turning code rather than writing angles, so the engine stays in sync.
// MUST run on the main thread. False if the address guard rejected it.
bool FacePointNative(const Vec3& p);

// Press the row the keyboard highlight is on, via the engine's own click path.
// listNameSubstr limits which list it may fire in (matched against the row's
// ancestors). Used to make a selected quest the TRACKED one: highlighting a
// quest does nothing by itself, the game tracks it when the row is clicked.
// MUST run on the main thread.
bool ClickSelectedRowIn(const char* listNameSubstr);

// Is the keyboard highlight inside a list whose name contains this substring?
// Read-only, safe from any thread — lets a key decide what it should do.
bool IsKeyboardSelectionIn(const char* listNameSubstr);

// Every distinct visible line of text in a menu, in tree order. For screens
// with no list to walk — a page of a book, an info panel — where the useful
// thing is simply "what does this say now".
std::vector<std::string> GetMenuTextLines(uint32_t menuType);

// The objectives shown for the quest selected in the Pip-Boy's Quests tab, as
// one string. Empty when that list isn't on screen or has no text.
std::string GetQuestObjectivesText();

// The quantity prompt ("Ile?"): the chosen amount, and the maximum when it can
// be determined reliably (0 when it can't — say just the number then).
// False when that menu isn't open.
bool GetQuantityState(int* amount, int* maximum);

// Screen centre of a named tile in a menu, and the game cursor's position —
// together they let us move the cursor onto a control and click it the way a
// player does. Needed for buttons that CLOSE a menu or change the inventory:
// calling those through HandleClick from our frame hook crashed the game.
bool GetMenuTileCenter(uint32_t menuType, const char* tileName, float* x, float* y);
bool GetMenuCursorPos(float* x, float* y);

// Live position of a reference. `expectedRefID` guards against a stale pointer
// (cell unload can free it and the address gets reused) — false if it no longer
// matches. For tracking a moving target between scans.
bool GetRefPosition(const void* refr, uint32_t expectedRefID, Vec3* out);

// What the camera is pointing at, for speaking it aloud: name, distance and
// whether it's an actor. Unlike the HUD activation prompt this also resolves
// targets with no "activate" verb (enemies), so a blind player can sweep the
// view and hear what's there. False when nothing is picked.
struct CrosshairTarget {
    uint32_t    refid = 0;
    std::string name;
    float       dist = 0.0f;
    bool        is_actor = false;
};
bool GetCrosshairTarget(CrosshairTarget* out);

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

// Aim the view (yaw + pitch) straight at a world position, so the first-person
// crosshair lands on it. For free-aim shooting where VATS is unavailable.
void AimLookAt(const Vec3& target);

// Same, but add `pitchBiasRad` to the computed pitch — used to sweep the
// vertical aim up/down when the exact eye height / target origin is uncertain.
void AimLookAtBiased(const Vec3& target, float pitchBiasRad);

// Where a shot actually leaves from: the camera's world position, read from the
// engine instead of estimated as "the player's feet plus a guess". The guess is
// what made every shot miss — an adult and a ten-year-old do not share an eye
// height. False when the 3D is not ready.
bool GetCameraPos(Vec3* out);

// The engine's own view raycast — the one that decides what is under the
// crosshair. Returns true if anything was hit; `outRefId` names it (0 for
// scenery with no reference) and `outDist` gives the range. This is the mod's
// only way to SEE, and it settles questions that guessing never could: is the
// aim on the target, or is something in the way.
bool RayCastView(const Vec3& from, const Vec3& dir, float maxDist,
                 uint32_t* outRefId, float* outDist,
                 const void** outRef = nullptr);

// What one ray straight down the view hits: its reference id (0 for scenery
// with no reference) and the range. Use THIS to ask "am I on target", never the
// InterfaceManager crosshair field - that field was measured holding the same
// value through a 21-degree swing of the aim, so it does not follow the view.
bool RayPickAhead(uint32_t* outRefId, float* outDist);

// A point on the object at `origin` that the view can actually reach, found
// with the engine's own ray. An object's origin is not its middle, and asking a
// sighted player to measure that for every kind of object in the game is not an
// answer — this is. False if nothing on it can be reached, which means
// something is in the way.
bool SolveAimPoint(const Vec3& origin, uint32_t wantRefId, Vec3* out);

// Sweep the engine's ray across a window around `centre` and write a map of
// everything it finds to the log: which references are there, how far, and
// where in the window. When an object refuses to be hit, this says what IS
// being hit instead — which no amount of reasoning about coordinates can.
void RayMapAround(const Vec3& centre);

// One thing the rays found in front of the player: what it is, where it is, and
// how big it looks from here.
struct RayHit {
    uint32_t refid = 0;
    std::string name;
    float dist = 0.0f;       // game units
    float yaw = 0.0f;        // degrees from straight ahead, + = right
    float pitch = 0.0f;      // degrees, + = down
    int   samples = 0;       // how many rays landed on it
};

// Sweep the engine's ray across a cone in front of the player and report every
// distinct thing it finds, nearest first.
//
// This answers the question the mod could never answer before: WHAT IS THERE.
// Coordinates from the game's own records say where an object's origin is, and
// an origin can sit inside a wall, behind scenery, or nowhere near the part you
// are supposed to shoot. Rays say what a bullet would actually meet.
std::vector<RayHit> RayScanAhead(float yawSpanDeg, float pitchSpanDeg,
                                 float stepDeg, float maxDist);

// Aim the player's view the way the engine itself does: the native pitch clamp
// and yaw-delta functions PLUS the camera rotation globals the renderer reads.
// Writing the angle fields alone does not move the weapon — the engine
// recomputes the view from its own state every frame and overwrites them.
// Main thread only (it calls into the game). Returns false on a build whose
// prologue guards do not match, so callers can fall back.
bool AimPlayerAtRad(float pitchRad, float yawRad);

// Same, expressed as "look at this world point"; `up` lifts the aim point off
// the target's origin (torso height, typically 100).
// `dzBias` shifts the aim point vertically, in game units. It corrects for the
// eye height and the target's aim point in one number — see the definition.
bool AimPlayerAtPoint(const Vec3& target, float up, float dzBias = 0.0f);

// How far the player's view has drifted from the last aim we applied, in
// radians (yaw and pitch). False when no aim has been applied yet. Measured a
// frame later than the write, which is the only way to see the engine undoing
// it.
bool AimDriftSinceLast(float* outYaw, float* outPitch);

// The two camera rotation values the renderer reads, in radians. Reading these
// back is how we tell whether the view really follows the aim or only the
// player's body does.
bool GetCameraAngles(float* outPitch, float* outYaw);

// The yaw the ENGINE considers the player to be facing, through its own
// accessor (vtable+0x2BC) rather than the raw field. In New Vegas this is the
// body rotation PLUS a separate free-aim offset, and if Fallout 3 does the same
// then the raw field is not where the view points.
bool GetEffectiveYawRad(float* out);

// Height above the player's origin that a shot is assumed to leave from. It is
// a guess by nature — an adult and the ten-year-old of the prologue are nowhere
// near each other — and getting it wrong tilts every shot. CalibrateEyeHeight
// replaces the guess with a measurement taken from a real, working aim.
float EyeHeight();
void  SetEyeHeight(float units);

// The base form a reference was built from — the identity shared by every copy
// of an object, so a correction measured on one of three identical targets
// applies to the other two.
const void* BaseFormOf(const void* refr);

// The base form's FORM ID — the same identity, but a number that stays valid
// between sessions, so a correction can be written to the INI.
uint32_t BaseFormIdOf(const void* refr);

// Native engine SetAngle (`Player.SetAngle X/Z`) via 0x00522B50 — writes the
// rotation AND runs the post-update + node-transform steps our raw field write
// skipped, which (per the friend's GECK-wiki lead) is what actually pushes the
// first-person camera. axisAscii = 'X' (pitch; NEGATIVE = look up) or 'Z' (yaw;
// 0 = north/+Y, clockwise), degrees. MUST be called on the MAIN thread.
void SetPlayerAngleDeg(int axisAscii, float degrees);

// Native teleport (`Player.MoveTo`) via 0x00528730 — move the player to the
// given reference (cell-aware). Last-resort navigation when a target can't be
// walked to. MUST be called on the MAIN thread.
void TeleportPlayerToRef(const void* targetRefr);

// True when a target reference shares the player's coordinate frame (same cell,
// or both in the same exterior worldspace). On-foot guidance walks toward a
// target's raw world position, which is only valid within one frame — a marker
// in another worldspace (e.g. a Wasteland location while you're in Megaton) must
// be reached by teleport, not by walking. Fails safe (false) on unreadable refs.
bool TargetSharesPlayerSpace(const void* targetRefr);

// Load-door helpers for cross-worldspace autowalk: is this ref a load door
// (teleports on use), does walking through it reach the target's coordinate
// frame, and does it lead back into a given cell's frame (used to avoid
// re-entering the door we just came through). All fail safe (false).
// Diagnostic label for a ref's location (cell name + worldspace name/ptr, or
// "wnetrze"). Used by the autowalk dump to show where the target actually lives.
std::string GetRefSpaceLabel(const void* refr);

bool IsLoadDoor(const void* doorRefr);
bool DoorLeadsToTargetSpace(const void* doorRefr, const void* targetRefr);
bool DoorLeadsToCellSpace(const void* doorRefr, const void* cellPtr);
bool DoorLeadsToExterior(const void* doorRefr);

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

// True while the player is in an interior cell (no worldspace). Interiors have
// no world map and no fast travel, so anything that suggests travelling must
// check this first.
bool PlayerIsInInterior();

// All refs sharing the base object of the currently-marked quest target, within
// `radius` (game units), nearest first. For "shoot the N targets" objectives:
// the targets are unnamed (scanner-invisible) but identical placements, so this
// returns the whole set to aim through. Empty if there's no marked target ref.
std::vector<WorldEntity> GetShootingTargets(int radius);

// Diagnostic: every reference in the player's cell within `radius` (game units),
// unfiltered by type/name — including statics/activators the scanner skips.
// One formatted line each ("0xID typ=N dist=.. dz=.. flags=.. 'name'"), nearest
// first. For figuring out what's shootable when the scanner/VATS find nothing.
std::vector<std::string> DebugNearbyRefs(int radius);

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

// The two halves of a container menu, read straight from their tiles.
//
// A container screen is two lists side by side — CM_Container_InventoryList
// holds what the container has, CM_Items_InventoryList what you are carrying —
// and nothing in the audio told the player which one they were in, or whether
// the container had anything in it at all. `rows` counts real entries, so an
// empty locker can say so instead of leaving someone pressing keys at nothing.
struct ContainerSide { std::string title; int rows = 0; };
bool GetContainerSides(ContainerSide* mine, ContainerSide* theirs);

// Move the keyboard focus to the other list by pressing its first row the way
// a mouse click would. Returns false when that list is empty.
bool FocusContainerList(bool container_side);

// Convenience: the focused row as one string ("label" or "label, value").
// Used by the diagnostic dump.
std::optional<std::string> GetKeyboardSelectionText();

// "Go back one menu level": find the visible Back button in the open menu
// and click it through the game's own Menu::HandleClick — so the engine's
// panel-stack logic decides where back leads. Returns false if no visible
// back button was found (nothing to go back from).
bool ClickMenuBack();

// Click the "Restore Defaults" button on the settings/controls page (StartMenu)
// via the game's Menu::HandleClick. Returns false if no such button is visible
// (i.e. you're not on the controls page). MUST run on the main thread.
bool ClickRestoreDefaults();

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

// The selected item's stat card (weight, value, damage/DR, condition, effects,
// ammo) read from the Pip-Boy's IM_ItemInfoRect. "" if no card is visible
// (e.g. the container menu, which only lists names).
std::string GetSelectedItemInfo();

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

// Current VATS selection read from the menu tiles: "<enemy>, <body part>,
// <hit %>" for the highlighted limb, or nullopt if VATS isn't open / no target.
std::optional<std::string> GetVatsSelectionText();

// Cycle the targeted body part in VATS (clicks the in-menu "Body Part" button).
// Returns false if VATS isn't open. MUST run on the main thread.
bool ClickVatsBodyPart();

// Click a named button inside the VATS menu ("BodyPart_button", "left_arrow",
// "right_arrow"). Returns false if VATS isn't open. MUST run on the main thread.
bool ClickVatsButton(const char* btnName);

// Click any named button in a menu via the engine's own HandleClick path.
// MUST run on the main thread (it runs game code / opens dialogs).
bool ClickMenuButton(uint32_t menuType, const char* btnName, int depth = 6);

// Click a button by id but WITHOUT handing the engine the tile — the form
// Command Extender uses. Needed for buttons that close a menu or move items;
// passing the tile there crashed the game. MUST run on the main thread.

// Press the world map's action button — the "Podróżować do <place>" press that
// actually starts fast travel after a marker has been selected. All the game's
// own rules (overencumbered, in combat, undiscovered) still apply.
// MUST run on the main thread.
bool ClickMapTravelButton();

// Select a world-map location by clicking its marker tile. Takes the tile from
// MapMarker::tile rather than a name — see the note there about code pages.
// MUST run on the main thread.
bool ClickMapMarkerTile(const void* tile);

// Full text of an open Message popup (title + body + button labels), e.g.
// an OK notification or the character-creation gender prompt. Empty if no
// Message menu is visible.
std::string GetActiveMessageText();

// Text-entry popup (TextEditMenu — character name, etc.): the prompt question
// and the text typed so far (caret glyph stripped). nullopt if it isn't open.
std::optional<std::string> GetTextEditPrompt();
std::optional<std::string> GetTextEditText();

// Terminal/computer screen body (TM_text in the Computers menu) — the page of
// text shown (logs, messages, unlock prompts). nullopt if no terminal/empty.
std::optional<std::string> GetTerminalText();

// HUD corner-notification text (the "Messages" tile: quest updates, items, XP,
// discovered locations). Empty when nothing is showing.
std::string GetHudMessage();

// The crosshair activate prompt (HUD "Info" tile): the verb + target you'd
// interact with — "Rozmawiaj", "Weź", "Okradnij", "Otwórz"… Empty when nothing
// is targeted. Lets a blind player hear the action before pressing Use.
std::string GetActivatePrompt();

// True while a computer terminal (ComputersMenu) is open.
bool IsTerminalOpen();

// Terminal "chrome" lines: the boot/welcome banner, ROBCO headers, login lines,
// command-result echoes — everything shown EXCEPT the opened entry body and the
// selectable command list. Ordered top-to-bottom. Read live as each appears.
std::vector<std::string> CollectTerminalChrome();

// Character creation (RaceSexMenu) focused category/option text — the
// RSM_list_item whose selection indicator is visible. nullopt if not in/focused.
std::optional<std::string> GetRaceSexSelection();

// True while the player's controls are disabled (char creation, cutscenes,
// forced dialogue) — used to gate gameplay-only cues.
bool ArePlayerControlsDisabled();

// Lockpick sweet-spot cue: fills *offset = |pin - sweet-spot centre| and
// *tol = tolerance (within it the lock opens). Returns false if the lockpick
// menu isn't open. For an audio "you're on the sweet spot" beep.
bool GetLockpickCue(float* offset, float* tol);

// Terminal hacking minigame: the candidate password words + attempts remaining.
// `active` is false when the hacking menu isn't open (or the grid is empty and
// the terminal isn't locked out). `locked` = out of attempts (lock-out screen).
// `highlighted` = the word the game cursor currently sits on (see
// GetHackingHighlightedWord); empty on a garbage symbol.
struct HackingInfo {
    bool                     active = false;
    bool                     locked = false;
    int                      attempts = 0;    // guesses remaining
    std::vector<std::string> words;           // candidate passwords (all same length)
    std::string              highlighted;     // word under the cursor, "" if none
};
HackingInfo GetHackingInfo();

// The candidate word currently under the hacking cursor, read from the grid
// tile's user trait — the engine writes the hovered word there. Lets us VERIFY
// what we're about to click instead of trusting geometry. Empty when the cursor
// is on a garbage symbol (or if this build of the game doesn't fill the trait).
std::string GetHackingHighlightedWord();

// The hacking attempt log as separate entries, oldest first (ordered by the
// rows' `listindex`). For diffing new feedback against what's been read.
std::vector<std::string> GetHackingLogEntries();

// Diagnostic (F11): every non-`string` string trait in the hacking menu, so we
// can see WHERE this build keeps the word under the cursor if the user0 probe
// misses it. One line per trait.
std::vector<std::string> DebugHackingTraits();

// Mouse-driven guess helpers (the minigame is mouse-only): the UI position of a
// candidate word's letter, the cursor's current position, and whether the pause
// menu is up (to stop hacking input). Used by the hacking module to drive the
// cursor onto a word and click it.
bool GetHackWordTarget(int idx, float* x, float* y);
bool GetHackingCursor(float* x, float* y);
bool IsPauseMenuOpen();

// The hacking attempt log joined (guesses + likeness / denied / granted).
std::string GetHackingLog();

// All readable text lines of the hacking screen (header + log), for browsing
// (left/right) and live reading of text as it appears.
std::vector<std::string> GetHackingTextLines();

// ---- Subtitles / voice lines ----

// Returns the most-recently-spoken NPC line if not yet announced.
std::optional<std::string> PollNewSubtitle();

} // namespace f3a::game
