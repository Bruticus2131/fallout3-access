#include "f3a/modules.h"
#include "f3a/game_access.h"
#include "f3a/polling_loop.h"
#include "f3a/menu_dispatch.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace f3a::modules::explore {
namespace {

// Not missing things.
//
// Auto-walk to a marker goes straight past everything a sighted player would
// notice on the way: the crate by the road, the body in the ditch, a holotape
// on a desk, a place nobody has visited yet. Two answers to that:
//
//  * EXPLORATION MODE: while auto-walk walks, whatever worth taking comes
//    within reach is said once — "Po drodze: Apteczka, 400 jednostek, na
//    godzinie 2". O walks there; O again goes back to where the walk was going.
//  * SWEEP: U says what is left around the player ("pojemniki 3, ciała 1..."),
//    I walks to the nearest of it, then the next, until nothing is left.
//
// "Worth taking" is the same test for both: a loose item that can be picked
// up, a container with something in it (or whose contents cannot be told
// yet - never hidden on a guess), a body with something on it, a place not yet
// discovered.

constexpr float kTickEvery   = 1.2f;    // s between scans while walking
constexpr float kPlaceReach  = 2500.0f; // undiscovered places this near count on a walk
constexpr float kSweepPlaces = 4000.0f; // ...and this near for a sweep
constexpr int   kScanMax     = 3000;

bool  g_on = true;                       // switched with Shift+O (starts as the INI says)
float g_timer = 0.0f;
bool  g_hint_said = false;
std::unordered_set<uint32_t> g_seen;     // said on the way already
std::unordered_set<uint32_t> g_visited;  // walked to by the sweep or O

game::WorldEntity g_last{};               // the latest find said on the way
bool g_have_last = false;

// Where the walk was going before O turned it aside.
struct Resume {
    bool        on = false;
    bool        quest = false;
    game::Vec3  pos{};
    std::string name;
    const void* refr = nullptr;
    uint32_t    form = 0;
} g_resume;

bool InWorld()
{
    if (!poll::IsGameplayActive() || poll::MenuModeActive()) return false;
    auto m = menu::ActiveMenu();
    return m == menu::Id::None || m == menu::Id::HUDMain;
}

// Things a player can pick up (FOSE form types): armour, book, clothing,
// ingredient, misc (caps, bobbleheads...), weapon, ammo, key, aid, note.
bool Pickable(uint8_t t)
{
    switch (t) {
    case 0x18: case 0x19: case 0x1A: case 0x1D: case 0x1F:
    case 0x28: case 0x29: case 0x2E: case 0x2F: case 0x31:
        return true;
    default:
        return false;
    }
}

bool IsFind(const game::WorldEntity& e)
{
    using K = game::WorldEntity::Kind;
    switch (e.kind) {
    case K::Item:
    case K::Note:      return Pickable(e.form_type);
    case K::Container: return e.items != 0;              // 1 = full, -1 = cannot tell
    case K::Actor:     return e.dead && e.items != 0;
    case K::Location:  return !e.discovered;
    default:           return false;
    }
}

std::string Label(const game::WorldEntity& e)
{
    if (e.kind == game::WorldEntity::Kind::Actor) return e.name + strings::T(", ciało", ", body");
    return e.name;
}

float DistTo(const game::Vec3& p)
{
    auto pp = game::GetPlayerPosition();
    float dx = p.x - pp.x, dy = p.y - pp.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Every find within `radius` (and undiscovered places within `places`),
// nearest first, leaving out what the sweep already visited and the walk's
// own target.
std::vector<game::WorldEntity> CollectFinds(float radius, float places)
{
    std::vector<game::WorldEntity> out;
    uint32_t target = autowalk::IsWalking() ? autowalk::TargetFormId() : 0;
    for (auto& e : game::ScanNearby((int)radius, kScanMax, false, false))
        if (IsFind(e) && !g_visited.count(e.form_id) && e.form_id != target)
            out.push_back(std::move(e));
    for (auto& e : game::GetNearbyLocations(places, 64))
        if (IsFind(e) && !g_visited.count(e.form_id) && e.form_id != target)
            out.push_back(std::move(e));
    std::sort(out.begin(), out.end(), [](const game::WorldEntity& a, const game::WorldEntity& b) {
        return DistTo(a.position) < DistTo(b.position);
    });
    return out;
}

std::string Where(const game::WorldEntity& e)
{
    auto b = game::ComputeBearing(game::GetPlayerPosition(), game::GetPlayerYaw(), e.position);
    return strings::FormatDistance(b.distance) + ", " + strings::ClockDirection(b.relative_yaw);
}

// Walk somewhere, remembering the trip it interrupts (once: a second detour
// keeps the original trip, not the first detour).
void Detour(const game::WorldEntity& e)
{
    if (autowalk::IsWalking() && !g_resume.on) {
        g_resume.on    = true;
        g_resume.quest = autowalk::IsFollowingQuest();
        g_resume.pos   = autowalk::TargetPos();
        g_resume.name  = autowalk::TargetName();
        g_resume.refr  = autowalk::TargetRefr();
        g_resume.form  = autowalk::TargetFormId();
    }
    g_visited.insert(e.form_id);
    F3A_INFO("Explore: walking to '%s' (%08X)%s", e.name.c_str(), (unsigned)e.form_id,
             g_resume.on ? ", the trip waits" : "");
    autowalk::StartTo(e.position, Label(e), e.refr, e.form_id);
}

void ResumeTrip()
{
    Resume r = g_resume;
    g_resume = Resume{};
    F3A_INFO("Explore: back to '%s'", r.name.c_str());
    if (r.quest && autowalk::StartToQuest()) return;
    autowalk::StartTo(r.pos, r.name, r.refr, r.form);
}

// ---- keys ---------------------------------------------------------------------

// O: to the latest find said on the way; again: back to the trip. Shift+O
// switches the announcements off and on.
void GoKey()
{
    if (!InWorld()) return;
    if (hotkeys::ShiftHeldNow()) {
        g_on = !g_on;
        tolk::Speak(g_on ? strings::T("Tryb eksploracji włączony.", "Exploration mode on.")
                         : strings::T("Tryb eksploracji wyłączony.", "Exploration mode off."),
                    tolk::Priority::Ui, true);
        return;
    }
    if (g_resume.on) { ResumeTrip(); return; }
    if (!g_have_last) {
        tolk::Speak(strings::T("Nic nowego po drodze.", "Nothing new on the way."), tolk::Priority::Ui, true);
        return;
    }
    g_have_last = false;
    Detour(g_last);
    if (g_resume.on)
        tolk::Speak(strings::T("Potem O wraca do trasy.", "Then O goes back to the route."),
                    tolk::Priority::Background, false);
}

// U: what is left around here.
void SummaryKey()
{
    if (!InWorld()) return;
    auto finds = CollectFinds((float)config::Get().nearby_scan_radius * 2.0f, kSweepPlaces);
    int cont = 0, body = 0, item = 0, note = 0, place = 0;
    for (const auto& e : finds) {
        switch (e.kind) {
        case game::WorldEntity::Kind::Container: ++cont; break;
        case game::WorldEntity::Kind::Actor:     ++body; break;
        case game::WorldEntity::Kind::Note:      ++note; break;
        case game::WorldEntity::Kind::Location:  ++place; break;
        default:                                 ++item; break;
        }
    }
    if (finds.empty()) {
        tolk::Speak(strings::T("Okolica przeszukana.", "Area searched."), tolk::Priority::Ui, true);
        return;
    }
    std::string say = strings::T("W okolicy:", "Around here:");
    auto add = [&](const char* what, int n) {
        if (n <= 0) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %s %d,", what, n);
        say += buf;
    };
    add(strings::T("pojemniki", "containers"), cont);
    add(strings::T("ciała", "bodies"), body);
    add(strings::T("przedmioty", "items"), item);
    add(strings::T("notatki", "notes"), note);
    add(strings::T("nieodkryte miejsca", "undiscovered places"), place);
    say.back() = '.';
    say += strings::T(" Najbliżej: ", " Nearest: ") + Label(finds[0]) + ", " + Where(finds[0]) + ".";
    F3A_INFO("Explore: summary - %d container(s), %d bod(ies), %d item(s), %d note(s), %d place(s)",
             cont, body, item, note, place);
    tolk::Speak(say, tolk::Priority::Ui, true);
}

// I: walk to the nearest thing left. Each press moves on to the next one (a
// press while still walking skips the current one).
void NextKey()
{
    if (!InWorld()) return;
    auto finds = CollectFinds((float)config::Get().nearby_scan_radius * 2.0f, kSweepPlaces);
    if (finds.empty()) {
        tolk::Speak(strings::T("Okolica przeszukana.", "Area searched."), tolk::Priority::Ui, true);
        if (g_resume.on)
            tolk::Speak(strings::T("O wraca do trasy.", "O goes back to the route."),
                        tolk::Priority::Background, false);
        return;
    }
    Detour(finds[0]);
    char left[48];
    std::snprintf(left, sizeof(left), strings::T("Zostało jeszcze: %d.", "Remaining: %d."), (int)finds.size() - 1);
    tolk::Speak(left, tolk::Priority::Background, false);
}

} // namespace

void Tick(float dt)
{
    if (!g_on || !config::Get().explore_mode) return;
    if (!autowalk::IsWalking() || !InWorld()) { g_timer = 0.0f; return; }
    g_timer += dt;
    if (g_timer < kTickEvery) return;
    g_timer = 0.0f;

    auto finds = CollectFinds((float)config::Get().explore_radius, kPlaceReach);
    std::vector<const game::WorldEntity*> fresh;
    for (const auto& e : finds)
        if (g_seen.insert(e.form_id).second) fresh.push_back(&e);
    if (fresh.empty()) return;
    const auto& e = *fresh[0];                      // nearest first
    g_last = e;
    g_have_last = true;
    std::string say = strings::T("Po drodze: ", "On the way: ") + Label(e) + ", " + Where(e);
    if (fresh.size() > 1)
        say += strings::T(", i jeszcze ", ", and ") + std::to_string(fresh.size() - 1) +
               (strings::Polish() ? "" : " more");
    say += ".";
    if (!g_hint_said) {
        g_hint_said = true;
        say += strings::T(" O prowadzi do znaleziska.", " O leads you to the find.");
    }
    F3A_INFO("Explore: on the way - '%s' and %u more", e.name.c_str(),
             (unsigned)(fresh.size() - 1));
    tolk::Speak(say, tolk::Priority::Background, false);
}

void Init()
{
    g_on = config::Get().explore_mode;
    const auto& h = config::Get().hotkeys;
    hotkeys::Bind(h.explore_go, &GoKey);
    hotkeys::Bind(h.sweep_summary, &SummaryKey);
    hotkeys::Bind(h.sweep_next, &NextKey);
    F3A_INFO("Explore module ready (exploration %s, radius %d).", g_on ? "on" : "off",
             config::Get().explore_radius);
}

void Shutdown()
{
    g_seen.clear();
    g_visited.clear();
    g_resume = Resume{};
    g_have_last = false;
}

} // namespace f3a::modules::explore
