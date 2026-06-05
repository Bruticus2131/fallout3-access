#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/polling_loop.h"

#include <cstdio>
#include <string>

namespace f3a::modules::nav {
namespace {

// Game-state hotkeys silently no-op while we're in the main menu or a
// loading screen — there's no player session to query.
bool SkipIfNotInGame()
{
    if (!poll::IsGameplayActive()) {
        F3A_DEBUG("nav: hotkey ignored (not in gameplay).");
        return true;
    }
    return false;
}

void SpeakWhereAmI()
{
    if (SkipIfNotInGame()) return;
    std::string loc = game::GetCurrentLocationName();
    if (loc.empty()) loc = "Pustkowia";
    float yaw = game::GetPlayerYaw();
    std::string compass = strings::CompassDirection(yaw);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s. %s",
                  loc.c_str(),
                  strings::RenderArgs(strings::Key::CompassFmt, compass.c_str()).c_str());
    tolk::Speak(buf, tolk::Priority::System, true);
}

void SpeakPlayerStatus()
{
    if (SkipIfNotInGame()) return;
    auto s = game::GetPlayerStatus();
    char hp[16], ap[16], rad[16], caps[16];
    std::snprintf(hp,   sizeof(hp),   "%d", s.health);
    std::snprintf(ap,   sizeof(ap),   "%d", s.ap);
    std::snprintf(rad,  sizeof(rad),  "%d", s.radiation);
    std::snprintf(caps, sizeof(caps), "%d", s.caps);
    tolk::Speak(
        strings::RenderArgs(strings::Key::PlayerStatusFmt, hp, ap, rad, caps),
        tolk::Priority::System, true);
}

void SpeakQuestTarget()
{
    if (SkipIfNotInGame()) return;
    auto qt = game::GetCurrentQuestTarget();
    if (!qt.valid) {
        tolk::Speak(strings::Render(strings::Key::NoQuestTarget),
                    tolk::Priority::System, true);
        return;
    }
    // No marker position decoded yet → read the objective text only.
    bool has_pos = qt.position.x != 0.0f || qt.position.y != 0.0f ||
                   qt.position.z != 0.0f;
    if (!has_pos) {
        tolk::Speak("Zadanie: " + qt.name, tolk::Priority::System, true);
        return;
    }
    auto pos = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto br = game::ComputeBearing(pos, yaw, qt.position);
    std::string dist = strings::FormatDistance(br.distance);
    std::string dir  = strings::ClockDirection(br.relative_yaw);
    tolk::Speak(
        strings::RenderArgs(strings::Key::QuestTargetFmt,
                            qt.name.c_str(), dist.c_str(), dir.c_str()),
        tolk::Priority::System, true);
}

void SpeakCompass()
{
    if (SkipIfNotInGame()) return;
    float yaw = game::GetPlayerYaw();
    tolk::Speak(
        strings::RenderArgs(strings::Key::CompassFmt,
                            strings::CompassDirection(yaw).c_str()),
        tolk::Priority::Ui, true);
}

} // namespace

void Init()
{
    const auto& h = config::Get().hotkeys;
    hotkeys::Bind(h.where_am_i,       &SpeakWhereAmI);
    hotkeys::Bind(h.player_status,    &SpeakPlayerStatus);
    hotkeys::Bind(h.quest_target,     &SpeakQuestTarget);
    hotkeys::Bind(h.describe_compass, &SpeakCompass);
    F3A_INFO("Nav assist module ready.");
}

void Shutdown() {}
void Tick(float) {}

} // namespace f3a::modules::nav
