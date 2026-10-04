#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/polling_loop.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdio>

namespace f3a::modules::vats {
namespace {

std::string g_last_sel;         // last announced selection text
bool g_tutorial_done = false;   // play the VATS how-to once per game session
// Edge-state for the in-VATS keyboard nav (A/D = target, W/S = body part,
// Space = queue a shot).
bool g_kW = false, g_kS = false, g_kA = false, g_kD = false, g_kSpace = false;
bool g_kH = false;

// Which limb VATS is aimed at is the GAME's business, not ours: the menu shows
// the name label only on the selected body part, and that is what we read. We
// used to keep our own index into the limb list and announce from it, which
// drifted out of step with the game the moment the player clicked anything.
int  g_last_count = 0;
std::string g_last_pick;
// Where we BELIEVE the cycle is, used only when the game marks no selection.
// Without it a failure to read the marker means total silence, which is the
// worst outcome of the three: wrong is recoverable, silent is not.
int  g_fallback = 0;
bool g_acquired = false;      // a limb is selected, so no need to go hunting
int  g_acquire_tries = 0;

// Step the game's own body-part selector. Its button only cycles FORWARD, so
// going back one means going round the rest of the way.
// A press made before the limb markers exist. VATS draws them a moment after it
// opens, and throwing the press away meant the player had to guess when the
// menu was ready and press again. Holding it costs nothing and makes the keys
// work from the instant VATS appears.
int g_pending_dir = 0;      // +1 / -1 / 0
int g_pending_ttl = 0;

void CycleVatsBodyPart(bool forward)
{
    game::VatsPick p;
    int count = poll::VatsPickState(&p) ? p.count : 0;
    F3A_INFO("VATS key: cycle limb %s (%d part(s))", forward ? "next" : "prev", count);
    if (count <= 0) {
        g_pending_dir = forward ? 1 : -1;
        g_pending_ttl = 40;          // ~3 s for the menu to finish drawing
        return;
    }
    g_fallback = (g_fallback + (forward ? 1 : count - 1)) % count;
    poll::RequestVatsPointAt(g_fallback, /*clickOnArrival=*/false);
    // Nothing is announced here: the selection is read back from the menu on
    // the next ticks, so what you hear is what the game actually did.
}

// Queue a shot at the limb we are pointing at.
void QueueVatsShot()
{
    F3A_INFO("VATS key: queue shot");
    // Point at the chosen limb and click it there: that is the whole gesture a
    // sighted player makes, and the only one this menu answers to.
    poll::RequestVatsPointAt(g_fallback, /*clickOnArrival=*/true);
}

// True on the rising edge of vk; `was` tracks the previous state.
bool KeyEdge(int vk, bool& was)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !was;
    was = down;
    return edge;
}

// Action points decide how many shots you can queue, so they are the number
// that actually matters in VATS — and the tiles don't give them.
int g_last_ap = -1;
int g_last_queued = -1;

// Everything that decides your next shot, on one key. Announced only when
// asked: during a fight this is reference material, not commentary.
void SpeakSummary()
{
    game::VatsInfo v;
    std::string out;
    if (poll::VatsState(&v) && v.has_ap) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d z %d punktów akcji", v.ap, v.max_ap);
        out = buf;
    }
    if (auto sel = game::GetVatsSelectionText()) {
        if (!sel->empty()) {
            if (!out.empty()) out += ", ";
            out += *sel;
        }
    }
    game::VatsPick pick;
    if (poll::VatsPickState(&pick) && pick.name[0]) {
        if (!out.empty()) out += ", ";
        out += pick.name;
        if (pick.chance[0]) out += std::string(", ") + pick.chance;
        char buf[48];
        std::snprintf(buf, sizeof(buf), ", %d z %d", pick.index, pick.count);
        out += buf;
    }
    if (out.empty()) out = "Brak danych VATS.";
    tolk::Speak(out, tolk::Priority::Combat, true);
}

// W/S cycle the body part, A/D switch the target. Runs only while VATS is open
// (this module's OnTick), and VATS pauses movement, so re-using WASD here does
// not fight walking.
void PollVatsKeys()
{
    bool w = KeyEdge('W', g_kW);   // evaluate all (no short-circuit) so every
    bool s = KeyEdge('S', g_kS);   // key's edge-state stays current
    bool a = KeyEdge('A', g_kA);
    bool d = KeyEdge('D', g_kD);
    bool sp = KeyEdge(VK_SPACE, g_kSpace);
    bool h  = KeyEdge('H', g_kH);
    if (w) CycleVatsBodyPart(true);
    if (s) CycleVatsBodyPart(false);
    if (a) { poll::RequestVatsClick(2); g_fallback = 0; }   // previous target
    if (d) { poll::RequestVatsClick(3); g_fallback = 0; }   // next target
    if (h) SpeakSummary();
    if (sp) QueueVatsShot();
}

// Nothing is said when a shot is queued, and that is deliberate. VATS plays its
// own sound for it, which the player already hears — and the only counter we
// could read ("queued_actions" on screen) stays at 0 even on shots that really
// were queued and fired, so anything built on it was either a false success or,
// worse, a false failure over a shot that had gone through.
// Spoken once, the first time VATS opens this session (toggle: [Voice]
// VatsTutorial). Explains what the mod reads and the keys to queue/fire/exit,
// since VATS is unusable blind without knowing the flow.
void SpeakTutorial()
{
    tolk::Speak(
        "Tryb V A T S. Klawiszami A i D zmieniasz cel, klawiszami W i S część "
        "ciała — czytam wybór: część ciała i szansę trafienia. Spacją dodajesz "
        "strzał do kolejki, E wykonuje zakolejkowane strzały i wychodzi z VATS.",
        tolk::Priority::Combat, true);
}

// Announce the current VATS selection (target, body part, hit %) read from the
// menu tiles, but only when it changes — so cycling targets/body parts with the
// game's controls reads out the new pick.
void AnnounceResources(bool force)
{
    game::VatsInfo v;
    if (!poll::VatsState(&v)) return;
    if (!force && v.ap == g_last_ap && v.queued == g_last_queued) return;
    bool apChanged = v.ap != g_last_ap;
    g_last_ap = v.ap;
    g_last_queued = v.queued;
    if (!force && !apChanged) return;      // queue alone is announced by the caller

    if (!v.has_ap) return;                 // offsets didn't check out on this build
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d z %d punktów akcji", v.ap, v.max_ap);
    tolk::Speak(buf, tolk::Priority::Combat, false);
}

// Everything at once, for the opening line and on demand.
std::string ResourceSummary()
{
    game::VatsInfo v;
    if (!poll::VatsState(&v)) return {};
    std::string out;
    char buf[96];
    if (v.has_ap) {
        std::snprintf(buf, sizeof(buf), "%d z %d punktów akcji", v.ap, v.max_ap);
        out = buf;
    }
    // Deliberately NOT the ammo count. VATS gets reopened after every shot, so
    // announcing the magazine each time turns a useful number into noise — and
    // unlike action points, it does not change what you can do in this menu.
    // It is still available on demand from the item card.
    if (v.queued > 0) {
        std::snprintf(buf, sizeof(buf), "%d w kolejce", v.queued);
        if (!out.empty()) out += ", ";
        out += buf;
    }
    return out;
}

void AnnounceSelection()
{
    auto sel = game::GetVatsSelectionText();
    std::string text = sel ? *sel : std::string();
    if (text == g_last_sel) return;
    g_last_sel = text;
    if (text.empty()) {
        tolk::Speak(strings::Render(strings::Key::VatsNoTargets),
                    tolk::Priority::Combat, true);
        return;
    }
    tolk::Speak(text, tolk::Priority::Combat, true);
}

void OnOpen()
{
    g_last_sel.clear();
    g_last_ap = -1;
    g_last_queued = -1;
    // Seed key edge-state to the live state so a key held as VATS opens doesn't
    // fire a spurious cycle on the first tick.
    g_kW = (GetAsyncKeyState('W') & 0x8000) != 0;
    g_kS = (GetAsyncKeyState('S') & 0x8000) != 0;
    g_kA = (GetAsyncKeyState('A') & 0x8000) != 0;
    g_kD = (GetAsyncKeyState('D') & 0x8000) != 0;
    g_kSpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    g_kH     = (GetAsyncKeyState('H') & 0x8000) != 0;
    g_last_pick.clear();
    g_last_count = 0;
    g_fallback = 0;
    g_pending_dir = 0;
    g_pending_ttl = 0;
    g_acquired = false;
    g_acquire_tries = 6;

    // First open this session: play the tutorial. The selection is read on the
    // following ticks (tutorial isn't interrupted because it's higher-priority
    // and AnnounceSelection only speaks on change).
    if (config::Get().vats_tutorial && !g_tutorial_done) {
        g_tutorial_done = true;
        SpeakTutorial();
        return;
    }
    AnnounceSelection();
    std::string res = ResourceSummary();
    if (!res.empty()) tolk::Speak(res, tolk::Priority::Combat, false);
}

void OnClose() { g_last_sel.clear(); g_last_ap = -1; g_last_queued = -1; }
// Announce the selected body part whenever the GAME changes it — by our key,
// by a mouse click, by switching target. Reading it back this way is also the
// proof that a key did something: if nothing is announced, nothing moved.
void AnnouncePick()
{
    if (poll::VatsPointBusy()) return;   // mid-move: wait for it to settle
    game::VatsPick p;
    if (!poll::VatsPickState(&p)) { g_last_pick.clear(); g_last_count = 0; return; }
    g_last_count = p.count;

    std::string say;
    if (p.index > 0 && p.name[0]) {
        say = p.name;                       // the game's own selection
        if (p.chance[0]) say += std::string(", ") + p.chance;
        g_fallback = p.index - 1;           // keep our counter honest
    } else if (p.count > 0) {
        if (g_fallback < 0 || g_fallback >= p.count) g_fallback = 0;
        if (g_fallback < 8) say = p.names[g_fallback];
    }
    if (say.empty() || say == g_last_pick) return;
    g_last_pick = say;
    tolk::Speak(say, tolk::Priority::Combat, true);
}

// Replay a press that arrived before the menu was ready.
void FlushPending()
{
    if (!g_pending_dir) return;
    if (--g_pending_ttl <= 0) { g_pending_dir = 0; return; }
    game::VatsPick p;
    if (!poll::VatsPickState(&p) || p.count <= 0) return;
    int dir = g_pending_dir;
    g_pending_dir = 0;
    CycleVatsBodyPart(dir > 0);
}

// Get a body part selected WITHOUT being asked. The markers appear a moment
// after VATS opens and the game commits to none of them, so the first press
// used to spend its time merely acquiring one — which is what the wait before
// the keys answered actually was. Doing it as soon as the list exists means
// the first press already has somewhere to go.
void AcquireSelection()
{
    if (g_acquired || poll::VatsPointBusy()) return;
    game::VatsPick p;
    if (!poll::VatsPickState(&p) || p.count <= 0) return;
    if (p.index > 0) { g_acquired = true; return; }   // the game got there first
    if (--g_acquire_tries < 0) return;                // don't hunt forever
    poll::RequestVatsPointAt(0, /*clickOnArrival=*/false);
}

void OnTick(float)
{
    AcquireSelection();
    FlushPending();
    AnnounceSelection();   // the target, when it changes (A/D)
    PollVatsKeys();
    AnnouncePick();        // the body part, when the GAME changes it (W/S)
    AnnounceResources(false);
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::VATS, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("VATS module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::vats
