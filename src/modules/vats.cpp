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
bool g_kSpace = false, g_kH = false;
// W and S are WATCHED, not handled: the game does the selecting, but a press
// has to produce a spoken answer even when the answer is the same words as
// last time. Watching a key with GetAsyncKeyState does not consume it, so the
// game still gets it.
bool g_kW = false, g_kS = false, g_kA = false, g_kD = false;
int  g_force_sel = 0;    // ticks left in which to speak the TARGET
int  g_force_pick = 0;   // ticks left in which to speak the pick regardless

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
// The part the last W/S press moved the cursor onto, waiting for the game to
// confirm it by marking it selected.
std::string g_want;
// The part last SPOKEN, and the game's selection last SEEN. A change of the
// game's selection is read out at once (as the FNV mod does) unless it is the
// part a W/S press has just announced — then it would only be said twice.
std::string g_spoken_part;
std::string g_seen_sel;

// Body parts and targets are the ENGINE's job, not ours.
//
// Fallout 3's own PC controls already do all of the selecting inside VATS:
// W and S step through body parts, A and D switch between targets. This module
// used to intercept those four keys and drive the mouse cursor to the right
// marker itself, which fought the engine for the selection (moving the mouse
// makes the menu re-pick whatever lies under the cursor) and dragged the system
// cursor out of the game window. Now the keys go straight to the game and this
// module only READS BACK what the game decided — which is also the honest
// signal, because what you hear is then what actually happened.

// Queue a shot at the limb we are pointing at.
void QueueVatsShot()
{
    F3A_INFO("VATS key: queue shot");
    poll::RequestVatsAttack();   // at whatever the game has selected
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
    // W/S/A/D are not HANDLED here — the game does that. W and S are merely
    // watched, so that a press is always answered out loud: the engine often
    // lands on a part whose name and hit chance read exactly the same as the
    // one before, and staying silent then is indistinguishable from the key
    // having done nothing at all. That is what "w i s nie czytaj\u0105" was.
    // W and S step the selection by moving the ENGINE's cursor onto the next
    // marker — which is how Fallout 3 picks a body part. The keys themselves are
    // not consumed (watching a key does not eat it), so if a future build of
    // the game ever does answer them directly, both paths agree.
    // A and D belong to the GAME. It switches targets with them by itself, and
    // the error beep that looked like a bug was the game saying there was no
    // other target to switch to — a true answer to a fair question. We only
    // watch the keys, so the target gets read out when it changes.
    bool ka = KeyEdge('A', g_kA), kd = KeyEdge('D', g_kD);
    if (ka || kd) {
        g_fallback   = 0;      // a new target has its own parts
        g_force_sel  = 20;     // ~1.5 s: answer the press out loud
        g_force_pick = 0;
    }
    bool kw = KeyEdge('W', g_kW), ks = KeyEdge('S', g_kS);
    if (kw || ks) {
        // Answered AT ONCE, like the FNV mod: the next part and its hit chance,
        // read straight from the part markers (each carries its own chance).
        // No waiting for the game to confirm first — the cursor is moved onto
        // the part in the same breath, and the game follows it.
        g_want.clear();
        game::VatsPick p;
        int count = poll::VatsPickState(&p) ? p.count : 0;
        if (count <= 0) {
            tolk::Speak("Brak wyboru.", tolk::Priority::Combat, true);
        } else {
            // Step from the part the GAME has selected when it marks one, so the
            // order follows what is really selected; from our own counter
            // otherwise.
            int base = (p.index > 0 && p.index <= count) ? p.index - 1 : g_fallback;
            if (base < 0 || base >= count) base = 0;
            g_fallback = (base + (kw ? 1 : count - 1)) % count;
            if (g_fallback < 8 && p.names[g_fallback][0]) {
                g_want = p.names[g_fallback];
                std::string say = g_want;
                if (p.chances[g_fallback][0])
                    say += std::string(", ") + p.chances[g_fallback];
                g_spoken_part = g_want;
                tolk::Speak(say, tolk::Priority::Combat, true);
            }
            poll::RequestVatsCursorTo(g_fallback);
        }
    }
    bool sp = KeyEdge(VK_SPACE, g_kSpace);
    bool h  = KeyEdge('H', g_kH);
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

// The target. Spoken when VATS opens and whenever A or D changes it — never
// on its own.
//
// VATS used to narrate: the target, then the body part, then the action
// points, and again whenever any of them drifted. In a fight that is three
// voices over each other and none of them is the one you pressed a key for.
// Now a key press is a question and the answer is one line.
void AnnounceSelection(bool force = false)
{
    auto sel = game::GetVatsSelectionText();
    std::string text = sel ? *sel : std::string();
    bool changed = (text != g_last_sel);
    g_last_sel = text;
    if (!force && !(changed && g_force_sel > 0)) return;
    g_force_sel = 0;
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
    g_kSpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    g_kW = (GetAsyncKeyState('W') & 0x8000) != 0;
    g_kS = (GetAsyncKeyState('S') & 0x8000) != 0;
    g_kA = (GetAsyncKeyState('A') & 0x8000) != 0;
    g_kD = (GetAsyncKeyState('D') & 0x8000) != 0;
    g_force_sel = 0;
    g_force_pick = 0;
    g_kH     = (GetAsyncKeyState('H') & 0x8000) != 0;
    g_last_pick.clear();
    g_last_count = 0;
    g_fallback = 0;
    g_want.clear();
    g_spoken_part.clear();
    g_seen_sel.clear();

    // First open this session: play the tutorial. The selection is read on the
    // following ticks (tutorial isn't interrupted because it's higher-priority
    // and AnnounceSelection only speaks on change).
    if (config::Get().vats_tutorial && !g_tutorial_done) {
        g_tutorial_done = true;
        SpeakTutorial();
        return;
    }
    // Opening VATS says WHO is targeted, and nothing else. The resource
    // summary used to follow it here; H reads that on demand, and having it
    // arrive unasked at the moment a fight starts is noise over the one fact
    // that matters.
    AnnounceSelection(/*force*/true);
}

void OnClose() { g_last_sel.clear(); g_last_ap = -1; g_last_queued = -1; }
// Announce the selected body part whenever the GAME changes it — by our key,
// by a mouse click, by switching target. Reading it back this way is also the
// proof that a key did something: if nothing is announced, nothing moved.
// Read out the game's own selection whenever it changes — by a mouse, by the
// game itself, or by our cursor landing somewhere other than where W/S meant.
// Silent when it is the part just announced by a key press.
void AnnouncePick()
{
    game::VatsPick p;
    if (!poll::VatsPickState(&p) || p.index <= 0 || !p.name[0]) return;
    if (g_seen_sel == p.name) return;
    bool first = g_seen_sel.empty();
    g_seen_sel = p.name;
    g_fallback = p.index - 1;
    // The part VATS opens on is taken in silently, as the FNV mod does: the
    // target's name is what opening VATS says.
    if (first) { g_spoken_part = p.name; return; }
    if (g_spoken_part == p.name) return;          // already said by W/S
    if (!g_want.empty())
        F3A_INFO("VATS pick: W/S announced '%s', the game selected '%s'",
                 g_want.c_str(), p.name);
    std::string say = p.name;
    if (p.chance[0]) say += std::string(", ") + p.chance;
    g_spoken_part = p.name;
    g_last_pick = say;
    // Queued, not cutting in: after A/D this follows the target's name.
    tolk::Speak(say, tolk::Priority::Combat, false);
}

void OnTick(float)
{
    PollVatsKeys();        // sets the "a key was pressed" windows below
    AnnounceSelection();   // the target — only after A or D
    AnnouncePick();        // the body part — only after W or S
    // Action points are NOT announced here any more. They changed constantly
    // and talked over the answer to whichever key had just been pressed; H
    // reads the full summary on demand.
}

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::VATS, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("VATS module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::vats
