#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/polling_loop.h"
#include "f3a/console.h"
#include "f3a/game_access.h"
#include "f3a/menu_dispatch.h"

#include <windows.h>
#include <map>
#include <array>
#include <unordered_map>
#include <vector>

// We use GetAsyncKeyState to avoid grabbing the DInput device away from the
// game. The DIK -> VK mapping below covers the keys we expose in INI.

namespace f3a::hotkeys {
namespace {

struct Binding { uint32_t dik; Action action; bool was_down = false; };

std::vector<Binding> g_bindings;

int DikToVk(uint32_t dik)
{
    // Known table FIRST. Extended DIK codes (0xC7 Home, 0xC9 PgUp, 0xCF End,
    // 0xD1 PgDn, ...) are E0-prefixed scancodes that MapVirtualKey does NOT
    // translate correctly — it returns a bogus non-zero VK (numpad aliases),
    // which used to shadow the fallback and silently bind the wrong key.
    switch (dik) {
    case 0x0D: return VK_OEM_PLUS;    // = / +  (skip objective)
    case 0x10: return 'Q';
    case 0x30: return 'B';            // B — cycle VATS body part
    case 0x13: return 'R';
    case 0x14: return 'T';
    case 0x21: return 'F';
    case 0x22: return 'G';
    case 0x23: return 'H';
    case 0x24: return 'J';
    case 0x25: return 'K';
    case 0x26: return 'L';
    case 0x2D: return 'X';
    case 0x2E: return 'C';
    case 0x1A: return VK_OEM_4;       // [
    case 0x1B: return VK_OEM_6;       // ]
    case 0x20: return 'D';            // D — item info in menus
    case 0x27: return VK_OEM_1;       // ;
    case 0x28: return VK_OEM_7;       // '
    case 0x2B: return VK_OEM_5;       // backslash
    case 0x33: return VK_OEM_COMMA;   // ,
    case 0x34: return VK_OEM_PERIOD;  // .
    case 0x35: return VK_OEM_2;       // '/'
    case 0x39: return VK_SPACE;
    case 0x1C: return VK_RETURN;
    case 0x0E: return VK_BACK;        // Backspace
    case 0xC7: return VK_HOME;
    case 0xC8: return VK_UP;          // arrows: E0-prefixed, so they MUST be
    case 0xD0: return VK_DOWN;        // listed here — MapVirtualKey hands back
    case 0xCB: return VK_LEFT;        // the numpad keys for these, which is how
    case 0xCD: return VK_RIGHT;       // the aim trim silently did nothing.
    case 0xC9: return VK_PRIOR;       // Page Up
    case 0xCF: return VK_END;
    case 0xD1: return VK_NEXT;        // Page Down
    case 0xD2: return VK_INSERT;
    case 0xD3: return VK_DELETE;
    case 0x3B: return VK_F1;
    case 0x3C: return VK_F2;
    case 0x3D: return VK_F3;
    case 0x3E: return VK_F4;
    case 0x3F: return VK_F5;
    case 0x40: return VK_F6;
    case 0x41: return VK_F7;
    case 0x42: return VK_F8;
    case 0x43: return VK_F9;
    case 0x44: return VK_F10;
    case 0x57: return VK_F11;
    case 0x58: return VK_F12;
    default:   break;
    }
    UINT vk = MapVirtualKeyW(dik, MAPVK_VSC_TO_VK_EX);
    return (int)vk;
}

bool IsForegroundFallout()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void ToggleMod()
{
    bool now = !config::IsEnabled();
    config::SetEnabled(now);
    tolk::Speak(strings::Render(now ? strings::Key::ModEnabled
                                    : strings::Key::ModDisabled),
                tolk::Priority::System, true);
}

void SilenceAction()
{
    // Space is bound to Silence globally, but inside VATS it is the key that
    // queues a shot — and the two fired together, so the mod announced "Strzał
    // dodany" and then instantly cut itself off. In VATS the shot wins; the
    // player can still stop speech with the repeat/other keys.
    if (menu::IsOpen(menu::Id::VATS)) return;
    tolk::Silence();
}

void MenuBackAction()
{
    // Click the visible Back/exit button via the game's own handler — but that
    // (Menu::HandleClick) is a game call that opens/closes menus, so it must run
    // on the main thread. Request it; the main-thread hook performs it.
    ::f3a::poll::RequestMenuBack();
}

void RestoreDefaultsAction()
{
    // R = click "Restore Defaults" on the controls page. The actual click is a
    // game call (main thread); ClickRestoreDefaults no-ops unless that page is
    // open, so pressing R during gameplay (where it's the reload key) is safe.
    ::f3a::poll::RequestRestoreDefaults();
}

void SkipObjectiveAction()
{
    // '=' = skip the current (inaccessible) objective by advancing the tracked
    // quest one stage. Only meaningful in gameplay; the SetStage call runs on
    // the main thread.
    if (!::f3a::poll::IsGameplayActive()) return;
    ::f3a::poll::RequestSkipObjective();
}

void VatsBodyPartAction()
{
    // B = cycle the targeted body part in VATS. ClickVatsBodyPart no-ops unless
    // VATS is open, so pressing B elsewhere is harmless.
    ::f3a::poll::RequestVatsBodyPart();
}

void DebugStartGame()
{
    const auto& cmd = config::Get().debug_start_command;
    if (cmd.empty()) return;
    if (!console::Available()) {
        tolk::Speak("Konsola niedostępna.", tolk::Priority::System, true);
        return;
    }
    tolk::Speak("Uruchamiam grę przez konsolę.", tolk::Priority::System, true);
    console::Run(cmd.c_str());
}

} // namespace

void Init()
{
    Rebind();
}

void Shutdown()
{
    g_bindings.clear();
}

void Bind(uint32_t dik, Action action)
{
    if (dik == 0) {
        F3A_WARN("Bind: skipping zero dik (would never fire).");
        return;
    }
    for (auto& b : g_bindings) {
        if (b.dik == dik) {
            F3A_INFO("Bind: replaced binding for DIK 0x%02X.", dik);
            b.action = std::move(action);
            return;
        }
    }
    g_bindings.push_back({dik, std::move(action), false});
    F3A_INFO("Bind: added DIK 0x%02X (count=%u).",
             dik, (unsigned)g_bindings.size());
}

void Rebind()
{
    g_bindings.clear();
    const auto& h = config::Get().hotkeys;

    // Two bindings are unconditional regardless of which module is enabled.
    Bind(h.toggle_mod, &ToggleMod);
    Bind(h.silence,    &SilenceAction);
    Bind(h.repeat_last, &tolk::RepeatLast);

    // Diagnostic: dump active menu tile tree to log (F11 default).
    Bind(h.dump_menu_tree, &::f3a::poll::DumpActiveMenuTree);

    // Debug: start a game via console (coc), skipping the intro (F10 default).
    Bind(h.debug_start_game, &DebugStartGame);

    // Menu navigation: Backspace = go back one menu level.
    Bind(h.menu_back, &MenuBackAction);

    // R = restore default controls (only acts on the controls settings page).
    Bind(h.restore_defaults, &RestoreDefaultsAction);

    // '=' = skip the current objective (advance the tracked quest's stage).
    Bind(h.skip_objective, &SkipObjectiveAction);

    // B = cycle the targeted body part in VATS.
    Bind(h.vats_body_part, &VatsBodyPartAction);

    // The rest are wired by their owning modules via additional Bind() calls
    // from world_scan / nav_assist / pipboy etc.
    F3A_INFO("Hotkeys: %u bindings active.", (unsigned)g_bindings.size());
}

int g_shift_grace = 0;   // ticks remaining where Shift "counts" as held

// Keys WE press ourselves. Autowalk drives the player by injecting W/A/S/D and
// Space, and those injections come back through GetAsyncKeyState exactly like a
// real press — so the mod kept firing its own hotkeys at itself: a recovery
// sidestep pressed D and triggered "read the item card", a recovery hop pressed
// Space and triggered whatever sits on it. Keys announced here are ignored for
// a few ticks so an injected press cannot be mistaken for the player's.
constexpr int kSuppressTicks = 4;
std::map<uint32_t, int> g_suppressed;

// Alt, tracked by TRANSITION rather than by current state.
//
// Alt+Tab leaves Alt reading as held: Windows never delivers the key-up to the
// application that lost focus, so GetAsyncKeyState keeps reporting it down long
// after the player let go. The mod took that at face value and turned an
// ordinary Home press into Alt+Home — teleporting the player instead of aiming,
// which is exactly how a session at the rifle range ended up standing on the
// targets. So Alt only counts once we have SEEN it go down while Fallout had
// focus, and regaining focus re-primes the state instead of trusting it.
bool g_alt_prev    = false;
bool g_alt_genuine = false;
bool g_had_focus   = false;

void Poll()
{
    if (!IsForegroundFallout()) { g_had_focus = false; return; }

    {
        bool altNow = (GetAsyncKeyState(VK_MENU)  & 0x8000) ||
                      (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
                      (GetAsyncKeyState(VK_RMENU) & 0x8000);
        if (!g_had_focus) {
            // First poll after the window came back: adopt whatever Alt reads
            // as, but do not treat it as a press the player just made.
            g_had_focus   = true;
            g_alt_prev    = altNow;
            g_alt_genuine = false;
        } else {
            if (altNow && !g_alt_prev) g_alt_genuine = true;
            if (!altNow)               g_alt_genuine = false;
            g_alt_prev = altNow;
        }
    }

    // Track Shift with a short grace window so modifier+key combos
    // register even if the key edge lands a tick or two before the modifier
    // is read as down.
    bool shiftNow = (GetAsyncKeyState(VK_SHIFT)  & 0x8000) ||
                    (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
                    (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    if (shiftNow)           g_shift_grace = 4;     // ~320 ms at the poll rate
    else if (g_shift_grace) --g_shift_grace;

    for (auto it = g_suppressed.begin(); it != g_suppressed.end(); ) {
        if (--it->second <= 0) it = g_suppressed.erase(it); else ++it;
    }

    for (auto& b : g_bindings) {
        int vk = DikToVk(b.dik);
        if (!vk) continue;
        if (g_suppressed.find(b.dik) != g_suppressed.end()) {
            // Keep the edge state current so releasing it doesn't fire later.
            b.was_down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            continue;
        }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !b.was_down) {
            F3A_DEBUG("hotkey fired: dik=0x%02X vk=0x%02X", b.dik, vk);
            if (b.action) b.action();
        }
        b.was_down = down;
    }
}

void SuppressKey(uint32_t dik) { g_suppressed[dik] = kSuppressTicks; }

bool ShiftActive() { return g_shift_grace > 0; }

// Shift read LIVE, bypassing the grace window above: the category modifier is
// HELD while tapping PgUp/PgDn, so the physical state is reliable — and
// crucially it clears the instant Shift is released, so the very next plain
// PgUp/PgDn reads an object/quest immediately instead of being eaten as another
// category step (which felt like the scanner "not refreshing right away").
bool ShiftHeldNow()
{
    return (GetAsyncKeyState(VK_SHIFT)  & 0x8000) ||
           (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
           (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
}

// Alt held — the modifier for Alt+Home = teleport (vs plain Home = aim).
// See g_alt_genuine: a raw read of Alt is not trustworthy after an Alt+Tab.
bool AltActive() { return g_alt_genuine; }

} // namespace f3a::hotkeys
