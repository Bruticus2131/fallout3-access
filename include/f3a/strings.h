#pragma once

#include <string>
#include <string_view>

namespace f3a::strings {

// Keys identify strings by purpose; resolved at runtime against the
// language picked in the INI. Anything not in this list goes through
// the game's own UTF-8 / cp1250 strings directly.
enum class Key {
    ModEnabled,
    ModDisabled,
    ModLoaded,
    NoScreenReader,
    ScreenReaderActive,        // %s = reader name
    MenuOpened,                // %s = menu name
    MenuClosed,
    SelectionFmt,              // %s = item, %s = extra
    EmptyList,
    Loading,
    LoadingDone,
    LowHealth,
    Crippled,                  // %s = body part
    InCombat,
    CombatEnded,
    QuestTargetFmt,            // %s = name, %.1f = distance, %s = direction
    NoQuestTarget,
    CompassFmt,                // %s = facing direction
    PlayerStatusFmt,           // %d HP, %d AP, %d RAD, %d caps, level %d
    NoNearbyEntities,
    NearbyHostileFmt,          // %s = name, %d = distance m, %s = clock pos
    NearbyFriendlyFmt,
    NearbyContainerFmt,
    NearbyDoorFmt,             // %s = name, %s = clock pos
    NearbyItemFmt,
    BarterTotalFmt,            // %d player gives, %d trader gives, %d net
    BarterLossWarn,            // %d caps
    // SPECIAL book (the vault's attribute book). Its pages are images, so the
    // attribute NAMES and DESCRIPTIONS have to come from us. The names match the
    // game's own wording, verified against a Pip-Boy dump; the descriptions are
    // ours, and are replaced by the game's text whenever the Pip-Boy has shown
    // it (see modules/specialbook).
    SpecialBookIntroFmt,       // %s = all seven values
    SpecialStrength, SpecialPerception, SpecialEndurance, SpecialCharisma,
    SpecialIntelligence, SpecialAgility, SpecialLuck,
    SpecialDescStrength, SpecialDescPerception, SpecialDescEndurance,
    SpecialDescCharisma, SpecialDescIntelligence, SpecialDescAgility,
    SpecialDescLuck,
    SpecialPageFmt,            // %d = page number
    LockpickSweetSpot,
    LockpickBrokenSoon,
    LockpickBroken,
    LockpickUnlocked,
    VatsTargetFmt,             // %s = part, %d = chance, %d = ap cost
    VatsTargetSwitched,
    VatsQueueFull,
    VatsNoTargets,
    DialogOptionFmt,           // %d = idx, %s = text, %s = skill req
    DialogNoOptions,
    NpcSpeechFmt,              // %s = speaker, %s = line
    ItemWeightFmt,
    ItemValueFmt,
    ItemNoWeight,
    ItemNoValue,
    LevelUp,
    KeyNotBound,
    // Loot Menu Updated's on-HUD container list.
    LootContainerFmt,          // %s = container name
    LootItemFmt,               // %s = entry, %s = "n z m"
    LootEquipped,
    LootStealing,
    LootWeightFmt,             // %s = carry weight as the overlay shows it
    LootPositionFmt,           // %s = position, %s = total
    LootClosed,
};

// All format directives are positional printf-style for std::snprintf.
const char* Fmt(Key k);

// Render a key with no args (most keys have none).
std::string Render(Key k);

// Render with up to N args. We don't try to be clever — just snprintf.
std::string RenderArgs(Key k, const char* a, const char* b = nullptr,
                       const char* c = nullptr, const char* d = nullptr);

// Direction names (8-wind or 12-clock depending on config).
std::string CompassDirection(float yaw_deg);
std::string ClockDirection(float relative_yaw_deg); // -180..180, 0 = forward

// Distance formatting: in-game units -> meters/steps.
std::string FormatDistance(float game_units);

} // namespace f3a::strings
