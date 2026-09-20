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

// Queue a VATS shot on the current target/body part. In VATS the "attack"
// control queues a shot; the default attack is the LEFT MOUSE button, and mouse
// injection reaches FO3 (DirectInput mouse) — unlike key injection. So inject a
// left click. The queued shots fire when the player presses Enter (native).
void QueueVatsShot()
{
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

// True on the rising edge of vk; `was` tracks the previous state.
bool KeyEdge(int vk, bool& was)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !was;
    was = down;
    return edge;
}

// W/S cycle the body part, A/D switch the target (mirrors the FNV mod). Runs
// only while VATS is open (this module's OnTick), and VATS pauses movement, so
// re-using WASD here doesn't fight walking. The selection read announces the
// result on the next tick.
void PollVatsKeys()
{
    bool w = KeyEdge('W', g_kW);   // evaluate all (no short-circuit) so every
    bool s = KeyEdge('S', g_kS);   // key's edge-state stays current
    bool a = KeyEdge('A', g_kA);
    bool d = KeyEdge('D', g_kD);
    bool sp = KeyEdge(VK_SPACE, g_kSpace);
    if (w || s) poll::RequestVatsClick(1);   // cycle body part
    if (a)      poll::RequestVatsClick(2);   // previous target
    if (d)      poll::RequestVatsClick(3);   // next target
    if (sp) {                                // queue a shot on the current pick
        QueueVatsShot();
        tolk::Speak("Strzał dodany.", tolk::Priority::Combat, false);
    }
}

// Spoken once, the first time VATS opens this session (toggle: [Voice]
// VatsTutorial). Explains what the mod reads and the keys to queue/fire/exit,
// since VATS is unusable blind without knowing the flow.
void SpeakTutorial()
{
    tolk::Speak(
        "Tryb V A T S. Klawiszami A i D zmieniasz cel, klawiszami W i S część "
        "ciała — czytam wybór: część ciała i szansę trafienia. Spacją dodajesz "
        "strzał do kolejki, Enter wykonuje strzały, Tab wychodzi.",
        tolk::Priority::Combat, true);
}

// Announce the current VATS selection (target, body part, hit %) read from the
// menu tiles, but only when it changes — so cycling targets/body parts with the
// game's controls reads out the new pick.
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
    // Seed key edge-state to the live state so a key held as VATS opens doesn't
    // fire a spurious cycle on the first tick.
    g_kW = (GetAsyncKeyState('W') & 0x8000) != 0;
    g_kS = (GetAsyncKeyState('S') & 0x8000) != 0;
    g_kA = (GetAsyncKeyState('A') & 0x8000) != 0;
    g_kD = (GetAsyncKeyState('D') & 0x8000) != 0;
    g_kSpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

    // First open this session: play the tutorial. The selection is read on the
    // following ticks (tutorial isn't interrupted because it's higher-priority
    // and AnnounceSelection only speaks on change).
    if (config::Get().vats_tutorial && !g_tutorial_done) {
        g_tutorial_done = true;
        SpeakTutorial();
        return;
    }
    AnnounceSelection();
}

void OnClose() { g_last_sel.clear(); }
void OnTick(float) { PollVatsKeys(); AnnounceSelection(); }

} // namespace

void Init()
{
    menu::RegisterMenu(menu::Id::VATS, &OnOpen, &OnClose, &OnTick);
    F3A_INFO("VATS module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::vats
