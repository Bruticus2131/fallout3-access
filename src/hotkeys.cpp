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
    // MapVirtualKey with MAPVK_VSC_TO_VK_EX gives us a usable VK for the
    // common scancodes. Extended keys (arrow cluster, numpad) need the high
    // bit, but we only expose alpha/F-keys/symbol keys in the default INI.
    UINT vk = MapVirtualKeyW(dik, MAPVK_VSC_TO_VK_EX);
    if (vk == 0) {
        // Fallback table for the keys we ship by default.
        switch (dik) {
        case 0x10: return 'Q';
        case 0x14: return 'T';
        case 0x21: return 'F';
        case 0x23: return 'H';
        case 0x2D: return 'X';
        case 0x2E: return 'C';
        case 0x35: return VK_OEM_2;       // '/'
        case 0x39: return VK_SPACE;
        case 0x1C: return VK_RETURN;
        case 0x0E: return VK_BACK;        // Backspace
        case 0x58: return VK_F12;
        default:   return 0;
        }
    }
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
            if (b.action) b.action();
        }
        b.was_down = down;
    }
}

} // namespace f3a::hotkeys
