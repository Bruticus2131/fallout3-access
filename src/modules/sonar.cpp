#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/audio_beacon.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace f3a::modules::sonar {
namespace {

// The Pip-Boy sonar: one key, and the surroundings are heard all at once.
//
// The scanner reads things out one by one, which is precise but slow — five
// presses to learn that there are two enemies to the left and a crate behind.
// A sighted player takes that in with one glance. The sweep is that glance by
// ear: a sound travels once round the player, clockwise from straight ahead,
// and everything nearby answers as it passes — from its own side, louder when
// close, and in its own voice (an enemy growls, a crate with loot rings, a
// door knocks, a person hums, a loose item ticks). Then one short line counts
// what was heard.
constexpr int kRadius   = 3000;   // game units, ~47 m
constexpr int kSweepMs  = 2400;   // one full turn
constexpr int kMaxItems = 8;      // loose items are many; keep the nearest few

DWORD       g_say_at = 0;
std::string g_say;

bool InWorld()
{
    if (!poll::IsGameplayActive() || poll::MenuModeActive()) return false;
    auto m = menu::ActiveMenu();
    return m == menu::Id::None || m == menu::Id::HUDMain;
}

void AddCount(std::string& out, const char* what, int n)
{
    if (n <= 0) return;
    char buf[48];
    std::snprintf(buf, sizeof(buf), " %s: %d.", what, n);
    out += buf;
}

void Sweep()
{
    if (!InWorld()) return;
    auto ents = game::ScanNearby(kRadius, 40, /*actors_only*/false, false);
    auto  pp  = game::GetPlayerPosition();
    float yaw = game::GetPlayerYaw();

    std::vector<audio::SweepBlip> blips;
    int host = 0, pers = 0, cont = 0, door = 0, item = 0;
    for (const auto& e : ents) {
        int kind;
        switch (e.kind) {
            case game::WorldEntity::Kind::Actor:
                kind = e.hostile ? audio::kSweepHostile : audio::kSweepPerson;
                (e.hostile ? host : pers)++;
                break;
            case game::WorldEntity::Kind::Container:
                kind = audio::kSweepContainer; ++cont; break;
            case game::WorldEntity::Kind::Door:
                kind = audio::kSweepDoor; ++door; break;
            case game::WorldEntity::Kind::Item:
            case game::WorldEntity::Kind::Note:
                if (item >= kMaxItems) continue;
                kind = audio::kSweepItem; ++item; break;
            default:
                continue;               // quest markers have their own guide
        }
        auto b = game::ComputeBearing(pp, yaw, e.position);
        blips.push_back({ b.relative_yaw, b.distance / (float)kRadius, kind });
    }
    audio::Sweep(blips, kSweepMs);

    std::string say = "Sonar.";
    AddCount(say, strings::T("Wrogowie", "Enemies"), host);
    AddCount(say, strings::T("Pojemniki", "Containers"), cont);
    AddCount(say, strings::T("Drzwi", "Doors"), door);
    AddCount(say, strings::T("Postacie", "People"), pers);
    AddCount(say, strings::T("Przedmioty", "Items"), item);
    if (blips.empty()) say = strings::T("Sonar: nic w pobliżu.", "Sonar: nothing nearby.");
    F3A_INFO("Sonar: %u blips (hostile %d, containers %d, doors %d, people %d, items %d)",
             (unsigned)blips.size(), host, cont, door, pers, item);
    // Said once the sweep has gone round, not over it.
    g_say    = say;
    g_say_at = GetTickCount() + kSweepMs + 250;
}

} // namespace

void Tick(float)
{
    if (g_say_at && (LONG)(GetTickCount() - g_say_at) >= 0) {
        g_say_at = 0;
        if (InWorld()) tolk::Speak(g_say, tolk::Priority::Ui, false);
    }
}

void Init()
{
    hotkeys::Bind(config::Get().hotkeys.sonar, &Sweep);
    F3A_INFO("Sonar module ready.");
}

void Shutdown() {}

} // namespace f3a::modules::sonar
