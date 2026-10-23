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

// --- The player's shot --------------------------------------------------------
//
// The shot direction 0x00773570 (thiscall on the player, three callers, all
// player shots) bends a ranged, non-VATS shot onto a target by at most
// fAutoAimMaxDegrees: onto the actor at PlayerCharacter+0x8E4 when there is
// one with 3D, else onto the point at +0x8E8 (x, y, z) when that is not zero
// (0x0077399E / 0x00773A76: direction = point - the view's origin). Which
// actor is there the game decides each frame (0x007794B0): only one the
// compass marks hostile, in a narrow window round the crosshair, with a clear
// ray - and in a whole fight with raiders it never chose one (log 2026-10-23).
// So while the aim key holds an enemy the point is set right before the shot
// to the spot the mod aims at - on the body, found by rays, led for a moving
// target - and the game's own correction carries the shot there.
constexpr uintptr_t kShotDirection = 0x00773570;
constexpr uintptr_t kShotSites[] = { 0x00778C3A, 0x007B794A, 0x007CA29B };

ShotAimProvider g_shot_provider = nullptr;
uintptr_t       g_shot_original = kShotDirection;
int             g_shot_patched  = 0;

void __cdecl PrepareShot(unsigned char* player)
{
    float p[3];
    ShotAimProvider prov = g_shot_provider;
    if (!player || !prov || !prov(p)) return;
    if (IsBadWritePtr(player + 0x8E4, 0x10)) return;
    *reinterpret_cast<void**>(player + 0x8E4) = nullptr;
    float* at = reinterpret_cast<float*>(player + 0x8E8);
    at[0] = p[0]; at[1] = p[1]; at[2] = p[2];
    static DWORD s_logged = 0;
    if (GetTickCount() - s_logged > 1000) {
        s_logged = GetTickCount();
        F3A_INFO("ShotAim: the shot is carried to (%.0f,%.0f,%.0f)", p[0], p[1], p[2]);
    }
}

void (__cdecl* g_prepare_shot)(unsigned char*) = &PrepareShot;

// Runs in place of the game's call: everything kept as it came (ecx = the
// player, the seven arguments on the stack), our point set, then on into the
// game's function, which returns to its caller.
__declspec(naked) void ShotDirection_Thunk()
{
    __asm {
        pushad
        pushfd
        push ecx
        call dword ptr [g_prepare_shot]
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_shot_original]
    }
}

void PatchShotDirection()
{
    // The function itself, before any call is pointed away from it:
    // push ebp / mov ebp,esp / and esp,-8 / sub esp,0xC4.
    const uint8_t want[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0xC4, 0x00 };
    auto* fn = reinterpret_cast<const uint8_t*>(kShotDirection);
    if (IsBadReadPtr(fn, sizeof(want)) || std::memcmp(fn, want, sizeof(want)) != 0) {
        F3A_WARN("engine_hooks: the shot direction code is not the expected one - shot aim off");
        return;
    }
    for (uintptr_t site : kShotSites)
        if (RedirectCall(site, kShotDirection, reinterpret_cast<const void*>(&ShotDirection_Thunk)))
            ++g_shot_patched;
    F3A_INFO("engine_hooks: shot aim installed at %d of 3 call sites.", g_shot_patched);
}

// --- Enter in a menu ----------------------------------------------------------
//
// With keyboard and mouse the game takes Enter in a menu as a click on the
// tile under the mouse (0x00628D01 makes it key code -2; 0x00626070 then
// clicks InterfaceManager+0xBC, else the active tile +0xCC). On the hacking
// screen that tile is whatever row the resting mouse is over, and the click
// enters the symbol or word highlighted there: the player pressed Enter to
// guess the word the mod had chosen, and the game had already spent an
// attempt on a stray "(" (log 2026-10-23 20:14: 3 -> 2 -> 1 attempts with no
// guess of theirs). While the mod answers Enter itself, the game's is held
// back - at its one call for the typed keys (0x00628BF5).
constexpr uintptr_t kMenuKey     = 0x00626070;
constexpr uintptr_t kMenuKeySite = 0x00628BF5;

EnterFilter g_enter_filter = nullptr;
uintptr_t   g_menu_key_original = kMenuKey;
bool        g_menu_key_patched = false;

bool __cdecl HoldEnter(int code)
{
    EnterFilter f = g_enter_filter;
    if (code != -2 || !f || !f()) return false;
    static DWORD s_logged = 0;
    if (GetTickCount() - s_logged > 2000) {
        s_logged = GetTickCount();
        F3A_INFO("engine_hooks: the game's Enter held back (the mod answers it)");
    }
    return true;
}

bool (__cdecl* g_hold_enter)(int) = &HoldEnter;

// In place of the call: ecx = the interface manager, the key code on the stack.
__declspec(naked) void MenuKey_Thunk()
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call dword ptr [g_hold_enter]
        add esp, 4
        pop ecx
        test al, al
        jnz held
        jmp dword ptr [g_menu_key_original]
    held:
        ret 4
    }
}

void PatchMenuEnter()
{
    // push eax / mov ecx,esi / call 0x00626070, and the function's own start.
    const uint8_t site[] = { 0x50, 0x8B, 0xCE, 0xE8 };
    const uint8_t fn[]   = { 0x6A, 0xFF, 0x68, 0x48, 0x3F, 0xC6, 0x00 };
    const auto* s = reinterpret_cast<const uint8_t*>(kMenuKeySite - 3);
    const auto* f = reinterpret_cast<const uint8_t*>(kMenuKey);
    g_menu_key_patched = !IsBadReadPtr(s, sizeof(site)) && std::memcmp(s, site, sizeof(site)) == 0 &&
                         !IsBadReadPtr(f, sizeof(fn)) && std::memcmp(f, fn, sizeof(fn)) == 0 &&
                         RedirectCall(kMenuKeySite, kMenuKey, reinterpret_cast<const void*>(&MenuKey_Thunk));
    F3A_INFO("engine_hooks: menu Enter hold %s.",
             g_menu_key_patched ? "installed" : "NOT installed (code differs)");
}

// --- Up and Down in a text field ---------------------------------------------
//
// The text-edit control (0x00625640) turns keys into special codes, 0x80000000
// plus: 1 Left, 2 Right, 3 Up, 4 Down (0x00620AA4..). Left and Right move the
// cursor; Up and Down fall through its jump table (0x006258CC) to the case
// that INSERTS a character - the code's low byte, an invisible 0x03 / 0x04 -
// and the name check then greys out Ok. Blind, the arrows are pressed by habit
// to look for the button, and the name could no longer be confirmed. The two
// entries are pointed at the handler's plain exit (0x006258B1, where its other
// do-nothing cases end), so the arrows do nothing there.
constexpr uintptr_t kEditKeyTable = 0x006258CC;
constexpr uintptr_t kEditInsertChar = 0x0062581F;
constexpr uintptr_t kEditKeyDone = 0x006258B1;
bool g_edit_arrows_patched = false;

void PatchTextEditArrows()
{
    auto* table = reinterpret_cast<uint32_t*>(kEditKeyTable);
    // backspace, left, right, up, down - as this build has them
    const uint32_t expect[5] = { 0x0062570C, 0x006257AE, 0x006257C4,
                                 kEditInsertChar, kEditInsertChar };
    bool ok = !IsBadReadPtr(table, sizeof(expect)) &&
              std::memcmp(table, expect, sizeof(expect)) == 0;
    DWORD old = 0;
    if (ok && VirtualProtect(table + 3, 8, PAGE_EXECUTE_READWRITE, &old)) {
        table[3] = kEditKeyDone;
        table[4] = kEditKeyDone;
        VirtualProtect(table + 3, 8, old, &old);
        g_edit_arrows_patched = true;
    }
    F3A_INFO("engine_hooks: text field Up/Down fix %s.",
             g_edit_arrows_patched ? "installed" : "NOT installed (code differs)");
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
    PatchTextEditArrows();
    PatchShotDirection();
    PatchMenuEnter();
    return g_drop_patched && g_edit_arrows_patched && g_shot_patched == 3 && g_menu_key_patched;
}

void SetShotAimProvider(ShotAimProvider provider) { g_shot_provider = provider; }
void SetMenuEnterFilter(EnterFilter filter) { g_enter_filter = filter; }

void RemoveHooks() {}

} // namespace f3a::engine
