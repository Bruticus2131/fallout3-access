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
    if (loc.empty()) loc = strings::T("Pustkowia", "Capital Wasteland");
    float yaw = game::GetPlayerYaw();
    std::string compass = strings::CompassDirection(yaw);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s. %s",
                  loc.c_str(),
                  strings::RenderArgs(strings::Key::CompassFmt, compass.c_str()).c_str());
    tolk::Speak(buf, tolk::Priority::System, true);
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
    // Prefix the quest name so it's clear WHICH quest this marker belongs to
    // (the tracked-objective slot can be taken over by a different quest).
    std::string qname = game::GetTrackedQuestName();
    std::string label = qname.empty() ? qt.name : (qname + ": " + qt.name);

    // No marker position decoded yet → read the objective text only.
    bool has_pos = qt.position.x != 0.0f || qt.position.y != 0.0f ||
                   qt.position.z != 0.0f;
    if (!has_pos) {
        tolk::Speak(strings::T("Zadanie: ", "Quest: ") + label, tolk::Priority::System, true);
        return;
    }
    // In another place: where, not a bearing to that place's coordinates.
    if (qt.refr && !game::TargetSharesPlayerSpace(qt.refr)) {
        std::string where = game::GetRefPlaceName(qt.refr);
        tolk::Speak(strings::T("Zadanie: ", "Quest: ") + label +
                        strings::T(", w innym miejscu", ", in another place") +
                        (where.empty() ? std::string() : ": " + where),
                    tolk::Priority::System, true);
        return;
    }
    auto pos = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();
    auto br = game::ComputeBearing(pos, yaw, qt.position);
    std::string dist = strings::FormatDistance(br.distance);
    std::string dir  = strings::ClockDirection(br.relative_yaw);
    tolk::Speak(
        strings::RenderArgs(strings::Key::QuestTargetFmt,
                            label.c_str(), dist.c_str(), dir.c_str()),
        tolk::Priority::System, true);
}

void GuideToQuest()
{
    if (SkipIfNotInGame()) return;
    auto qt = game::GetCurrentQuestTarget();
    if (!qt.valid) {
        tolk::Speak(strings::Render(strings::Key::NoQuestTarget),
                    tolk::Priority::System, true);
        return;
    }
    bool has_pos = qt.position.x != 0.0f || qt.position.y != 0.0f ||
                   qt.position.z != 0.0f;
    if (!has_pos) {
        tolk::Speak(strings::T("Cel zadania bez markera na mapie: ",
                              "Quest objective with no map marker: ") + qt.name,
                    tolk::Priority::System, true);
        return;
    }
    // Marker in another worldspace (a wasteland location while we're in Megaton,
    // etc.): its coordinates are in a different frame, so a beacon toward them
    // would lead nowhere. The auto-walk routes through the connecting doors;
    // the Alt+Home teleport is not advised — its native MoveTo has crashed
    // the game.
    if (qt.refr && !game::TargetSharesPlayerSpace(qt.refr)) {
        tolk::Speak(strings::T("Cel zadania jest w innej lokacji — sygnał tam nie "
                               "poprowadzi. Użyj automatycznego chodzenia: prowadzi "
                               "przez przejścia.",
                               "The quest objective is in another location — the beacon "
                               "can't lead you there. Use auto-walk: it goes through "
                               "the doors on the way."),
                    tolk::Priority::System, true);
        return;
    }
    modules::autowalk::Stop();
    // Follow the LIVE quest marker (re-read each tick) so the beacon retargets
    // if the objective advances mid-walk, instead of a position cached at start.
    modules::guide::StartToQuest();
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
    hotkeys::Bind(h.quest_target,     &SpeakQuestTarget);
    hotkeys::Bind(h.describe_compass, &SpeakCompass);
    hotkeys::Bind(h.guide_quest,      &GuideToQuest);
    F3A_INFO("Nav assist module ready.");
}

void Shutdown() {}
void Tick(float) {}

} // namespace f3a::modules::nav
