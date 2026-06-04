#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/polling_loop.h"
#include "f3a/console.h"
#include "f3a/game_access.h"

#include <windows.h>
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
    case 0x10: return 'Q';
    case 0x14: return 'T';
    case 0x21: return 'F';
    case 0x23: return 'H';
    case 0x25: return 'K';
    case 0x26: return 'L';
    case 0x2D: return 'X';
    case 0x2E: return 'C';
    case 0x1A: return VK_OEM_4;       // [
    case 0x1B: return VK_OEM_6;       // ]
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
    tolk::Silence();
}

void MenuBackAction()
{
    // Click the visible Back button via the game's own handler. If no menu
    // exposes one right now, do nothing quietly.
    game::ClickMenuBack();
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

    // The rest are wired by their owning modules via additional Bind() calls
    // from world_scan / nav_assist / pipboy etc.
    F3A_INFO("Hotkeys: %u bindings active.", (unsigned)g_bindings.size());
}

void Poll()
{
    if (!IsForegroundFallout()) return;

    for (auto& b : g_bindings) {
        int vk = DikToVk(b.dik);
        if (!vk) continue;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !b.was_down) {
            F3A_DEBUG("hotkey fired: dik=0x%02X vk=0x%02X", b.dik, vk);
            if (b.action) b.action();
        }
        b.was_down = down;
    }
}

} // namespace f3a::hotkeys
