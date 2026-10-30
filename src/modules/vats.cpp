#include "f3a/modules.h"
#include "f3a/menu_dispatch.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"
#include "f3a/polling_loop.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdint>
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
// Ticks the opening line may wait for VATS to show its target. The menu is
// sampled on the main thread, so at the moment it opens there may be nothing
// to read yet; "no targets" is said only if there still is none after this.
int  g_open_wait = 0;
// The opening line comes after the tutorial, not over it.
bool g_open_queue = false;
// Ticks until the target is said after A or D, whether its name changed or
// not: two raiders are both "Bandyta", and the switch between them said
// nothing at all (log 2026-10-24).
int  g_ad_answer = 0;

// Which limb VATS is aimed at is the GAME's business, not ours: the menu shows
// the name label only on the selected body part, and that is what we read. We
// used to keep our own index into the limb list and announce from it, which
// drifted out of step with the game the moment the player clicked anything.
int  g_last_count = 0;
std::string g_last_pick;
// W and S belong to the GAME: its VATS input steps the selected part to the
// next or previous one of the target's own list on the press of W or S (or
// the mouse wheel). A press is answered with the part the game selected, read
// back; nothing is predicted. Predicting the next part from a list of our own
// and putting the cursor on it fought the game's own step - a bloatfly's
// "Głowa" was said twice while the game went to "Mózg" (log 2026-10-24).
int  g_ws_wait = 0;            // ticks a W/S press still waits for its answer
DWORD g_park_until = 0;        // keep the game's cursor in the corner until then
bool  g_park_said = false;
constexpr int kWsAnswerTicks = 6;
// The game's selection last seen: which part (an identity, so two parts with
// one name still count as a change) and its name.
uintptr_t   g_seen_key = 0;
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
        std::snprintf(buf, sizeof(buf),
                      strings::T("%d z %d punktów akcji", "%d of %d Action Points"),
                      v.ap, v.max_ap);
        out = buf;
    }
    std::string sel;
    if (poll::VatsTargetState(&sel) && !sel.empty()) {
        if (!out.empty()) out += ", ";
        out += sel;
    }
    game::VatsPick pick;
    if (poll::VatsPickState(&pick) && pick.name[0]) {
        if (!out.empty()) out += ", ";
        out += pick.name;
        if (pick.chance[0]) out += std::string(", ") + pick.chance;
        char buf[48];
        std::snprintf(buf, sizeof(buf), strings::T(", %d z %d", ", %d of %d"),
                      pick.index, pick.count);
        out += buf;
    }
    if (out.empty()) out = strings::T("Brak danych VATS.", "No V.A.T.S. data.");
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
    // A and D belong to the GAME. It switches targets with them by itself, and
    // the error beep that looked like a bug was the game saying there was no
    // other target to switch to — a true answer to a fair question. We only
    // watch the keys, so the target gets read out when it changes.
    bool ka = KeyEdge('A', g_kA), kd = KeyEdge('D', g_kD);
    if (ka || kd) {
        g_force_sel  = 20;     // ~1.5 s: answer the press out loud
        g_force_pick = 0;
        g_ad_answer  = 5;      // the game switches, then the name is said
    }
    bool kw = KeyEdge('W', g_kW), ks = KeyEdge('S', g_kS);
    if (kw || ks) {
        // The game steps; AnnouncePick says where it went.
        game::VatsPick p;
        if (!poll::VatsPickState(&p) || p.count <= 0)
            tolk::Speak(strings::T("Brak wyboru.", "Nothing to select."),
                        tolk::Priority::Combat, true);
        else
            g_ws_wait = kWsAnswerTicks;
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
    tolk::Speak(strings::T(
        "Tryb V A T S. Klawiszami A i D zmieniasz cel, klawiszami W i S część "
        "ciała — czytam wybór: część ciała i szansę trafienia. Spacją dodajesz "
        "strzał do kolejki, E wykonuje zakolejkowane strzały i wychodzi z VATS.",
        "V.A.T.S. mode. A and D change the target, W and S change the body part. "
        "I read the selection: body part and hit chance. Space queues a shot, "
        "E fires the queued shots and leaves V.A.T.S."),
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
    std::snprintf(buf, sizeof(buf),
                  strings::T("%d z %d punktów akcji", "%d of %d Action Points"),
                  v.ap, v.max_ap);
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
        std::snprintf(buf, sizeof(buf),
                      strings::T("%d z %d punktów akcji", "%d of %d Action Points"),
                      v.ap, v.max_ap);
        out = buf;
    }
    // Deliberately NOT the ammo count. VATS gets reopened after every shot, so
    // announcing the magazine each time turns a useful number into noise — and
    // unlike action points, it does not change what you can do in this menu.
    // It is still available on demand from the item card.
    if (v.queued > 0) {
        std::snprintf(buf, sizeof(buf), strings::T("%d w kolejce", "%d queued"),
                      v.queued);
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
void AnnounceSelection()
{
    // Read from the main thread's sample: VATS is closed by a key press on that
    // thread, and reading its tiles from here raced with it being freed.
    std::string text;
    bool have = poll::VatsTargetState(&text);
    bool force = false;
    if (g_open_wait > 0) {
        // VATS has just opened: say who is targeted as soon as it shows, and
        // "no targets" only if it still shows nobody after a moment.
        if ((!have || text.empty()) && --g_open_wait > 0) return;
        g_open_wait = 0;
        force = true;
    } else if (!have) {
        return;
    }
    bool changed = (text != g_last_sel);
    g_last_sel = text;
    // A or D always gets its answer a moment after the press: the name read
    // once the game has switched, the same name too (another "Bandyta").
    bool answer = false;
    if (g_ad_answer > 0 && --g_ad_answer == 0) answer = true;
    if (!force && !answer && !(changed && g_force_sel > 0)) return;
    g_force_sel = 0;
    g_ad_answer = 0;
    const bool queue = g_open_queue;
    g_open_queue = false;
    if (text.empty()) {
        tolk::Speak(strings::Render(strings::Key::VatsNoTargets),
                    tolk::Priority::Combat, !queue);
        return;
    }
    tolk::Speak(text, tolk::Priority::Combat, !queue);
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
    g_open_wait = 0;
    g_open_queue = false;
    g_ad_answer = 0;
    g_kH     = (GetAsyncKeyState('H') & 0x8000) != 0;
    g_last_pick.clear();
    g_last_count = 0;
    g_ws_wait = 0;
    g_seen_key = 0;
    g_seen_sel.clear();
    // VATS also picks the part under the game's mouse cursor, and the camera
    // closing in slides the markers under a cursor left mid-screen: W stepped
    // the part, then it changed again by itself (log 2026-10-30). The cursor
    // goes to the corner for the opening zoom, then is left alone, for a
    // player who does use the mouse.
    g_park_until = GetTickCount() + 1500;
    g_park_said = false;

    // First open this session: play the tutorial. The selection is read on the
    // following ticks (tutorial isn't interrupted because it's higher-priority
    // and AnnounceSelection only speaks on change).
    // The target follows it, queued: the first VATS of a session used to
    // say the tutorial and then never who was targeted.
    if (config::Get().vats_tutorial && !g_tutorial_done) {
        g_tutorial_done = true;
        SpeakTutorial();
        g_open_queue = true;
        g_open_wait = 6;
        return;
    }
    // Opening VATS says WHO is targeted, and nothing else. The resource
    // summary used to follow it here; H reads that on demand, and having it
    // arrive unasked at the moment a fight starts is noise over the one fact
    // that matters.
    g_open_wait = 6;       // ~0.5 s; spoken by AnnounceSelection on the ticks
}

void OnClose()
{
    g_last_sel.clear(); g_last_ap = -1; g_last_queued = -1;
    g_force_sel = 0; g_open_wait = 0; g_open_queue = false; g_ad_answer = 0;
}
// Announce the selected body part whenever the GAME changes it — by our key,
// by a mouse click, by switching target. Reading it back this way is also the
// proof that a key did something: if nothing is announced, nothing moved.
// Read out the game's own selection whenever it changes — by a mouse, by the
// game itself, or by our cursor landing somewhere other than where W/S meant.
// Silent when it is the part just announced by a key press.
void AnnouncePick()
{
    game::VatsPick p;
    const bool have = poll::VatsPickState(&p) && p.index > 0 && p.name[0];
    std::string say;
    if (have) {
        say = p.name;
        if (p.chance[0]) say += std::string(", ") + p.chance;
        const bool changed = p.sel_key ? p.sel_key != g_seen_key : g_seen_sel != p.name;
        if (changed) {
            const bool first = !g_seen_key && g_seen_sel.empty();
            if (g_ws_wait > 0 && !first)
                F3A_INFO("VATS W/S: the game went from '%s' to '%s' (%d of %d)",
                         g_seen_sel.c_str(), p.name, p.index, p.count);
            g_seen_key = p.sel_key;
            g_seen_sel = p.name;
            // The part VATS opens on is taken in silently, as the FNV mod
            // does: the target's name is what opening VATS says.
            if (first) return;
            // The answer to W or S cuts in; a change of the game's own (a new
            // target after A or D, the wheel) is queued after what is said.
            const bool answer = g_ws_wait > 0;
            g_ws_wait = 0;
            g_last_pick = say;
            tolk::Speak(say, tolk::Priority::Combat, answer);
            return;
        }
    }
    // A press that changed nothing (a target with one part): the part it
    // stays on is said, so the key is never met with silence. The game's step
    // skips parts whose marker is hidden - a radroach behind the shooting
    // range's counter showed only its head - so with one part left that is
    // said too, or it sounds like W and S do nothing.
    if (g_ws_wait > 0 && --g_ws_wait == 0) {
        // The game takes W/S only for a gun aimed at a target with parts
        // (see game::VatsPartChoice); otherwise it keeps the part it shows,
        // and the reason is said - not the same part, again and again.
        char detail[200];
        int why = 0;
        const int choice = game::VatsPartChoice(&why, detail, sizeof(detail));

        F3A_INFO("VATS W/S: the game stayed on '%s' (%d part(s) to choose from; part choice %d, "
                 "reason %d: %s)", have ? p.name : "", p.count, choice, why, detail);
        if (choice == 0) {
            const char* r =
                why == 4 || why == 3
                    ? strings::T("Bronią białą i wręcz nie wybierzesz części ciała.",
                                 "Melee and unarmed attacks can't pick a body part.")
                : why == 2 ? strings::T("Ten cel nie ma części ciała do wyboru.",
                                        "This target has no body parts to pick.")
                           : strings::T("Gra nie pozwala teraz wybrać części ciała.",
                                        "The game doesn't let you pick a body part now.");
            tolk::Speak(r, tolk::Priority::Combat, true);
            return;
        }
        if (have && p.count == 1)
            say += strings::T(". Tylko tę część widać.", ". Only this part is in view.");
        if (have) tolk::Speak(say, tolk::Priority::Combat, true);
        else tolk::Speak(strings::T("Brak wyboru.", "Nothing to select."),
                         tolk::Priority::Combat, true);
    }
}

void OnTick(float)
{
    if (g_park_until && (LONG)(GetTickCount() - g_park_until) < 0) {
        if (game::ParkCursorForVats() && !g_park_said) {
            g_park_said = true;
            F3A_INFO("VATS: the game's cursor parked in the corner, clear of the markers");
        }
    } else {
        g_park_until = 0;
    }
    PollVatsKeys();        // sets the "a key was pressed" windows below
    AnnounceSelection();   // the target — only after A or D
    // The A/D window closes on its own: without this a target change long
    // after the press (the target dying, a new one walking in) was still
    // announced as if it answered the key.
    if (g_force_sel > 0) --g_force_sel;
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
