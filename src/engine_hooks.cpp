// Engine patches for Fallout3.exe v1.7.0.3 GOTY.
//
// Each patch rewrites one 5-byte CALL in the game's code to go through us
// first. Every site is checked byte for byte before it is touched, so another
// build (the no-gore exe, a different patch level) is simply left alone.

#include "f3a/engine_hooks.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace f3a::engine {
namespace {

// --- Dropping a stack from the inventory ------------------------------------
//
// InventoryMenu::HandleClick's drop case (id 7) checks the selected item -
// the global at 0x01076864, an inventory entry - and for a stack opens the
// "Ile?" prompt (0x0066A700) with the callback 0x006542A0, which drops the
// chosen number of THAT global's item. The callback reads it with no null
// check. Meanwhile the row the prompt covers gets its unmouseover
// (InventoryMenu slot 5, 0x00653CF0), which clears the selection - kept only
// when a gamepad is in use and the prompt is the top menu. With keyboard and
// mouse it is gone by the time Ok is pressed: A crashed the game at
// 0x006542C8 (read through the null entry), and so did our own Ok clicks
// earlier.
//
// The patch: the prompt's opener remembers the item it was opened for, and the
// callback puts it back as the selection before the game's drop runs - what the
// gamepad path does by keeping it.
constexpr uintptr_t kDropQuantityCall = 0x00654C88;   // call ShowQuantityMenu
constexpr uintptr_t kDropCallbackPush = 0x00654C7D;   // push 0x006542A0, just before
constexpr uintptr_t kShowQuantityMenu = 0x0066A700;
constexpr uintptr_t kDropCallback     = 0x006542A0;
constexpr uintptr_t kInventorySelection = 0x01076864;

using QuantityCallback = void(__cdecl*)(int count);
using ShowQuantityFn   = void*(__cdecl*)(int maxCount, QuantityCallback cb, int cap);

void*            g_drop_entry    = nullptr;   // the item the open prompt is for
QuantityCallback g_drop_callback = nullptr;   // the game's, called after the fix-up
bool             g_drop_patched  = false;

void __cdecl DropCallback_Hook(int count)
{
    auto** sel = reinterpret_cast<void**>(kInventorySelection);
    if (g_drop_entry && *sel != g_drop_entry) {
        F3A_INFO("Drop: selection was %p, put back %p for %d", *sel, g_drop_entry, count);
        *sel = g_drop_entry;
    }
    g_drop_entry = nullptr;
    if (!*sel || !g_drop_callback) {
        // Nothing to drop; the game's callback would read through null.
        F3A_WARN("Drop: no selected item at the prompt's Ok - skipped");
        return;
    }
    g_drop_callback(count);
}

void* __cdecl ShowDropQuantity_Hook(int maxCount, QuantityCallback cb, int cap)
{
    g_drop_entry    = *reinterpret_cast<void**>(kInventorySelection);
    g_drop_callback = cb;
    F3A_INFO("Drop: quantity prompt for %p, up to %d", g_drop_entry, maxCount);
    return reinterpret_cast<ShowQuantityFn>(kShowQuantityMenu)(maxCount, &DropCallback_Hook, cap);
}

// Point the CALL at `site` (which must currently call `expected`) at `hook`.
bool RedirectCall(uintptr_t site, uintptr_t expected, const void* hook)
{
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return false;
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, 4);
    if (site + 5 + rel != expected) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    std::memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void PatchDropQuantity()
{
    // The push of the game's callback right before the call confirms the site.
    const uint8_t push[5] = { 0x68, 0xA0, 0x42, 0x65, 0x00 };
    const void* at = reinterpret_cast<const void*>(kDropCallbackPush);
    g_drop_patched = !IsBadReadPtr(at, 5) && std::memcmp(at, push, 5) == 0 &&
                     RedirectCall(kDropQuantityCall, kShowQuantityMenu, &ShowDropQuantity_Hook);
    F3A_INFO("engine_hooks: inventory drop prompt fix %s.",
             g_drop_patched ? "installed" : "NOT installed (code differs)");
}

} // namespace

bool InstallHooks()
{
    PatchDropQuantity();
    return g_drop_patched;
}

void RemoveHooks() {}

} // namespace f3a::engine
