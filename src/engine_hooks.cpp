// Engine patches for Fallout3.exe v1.7.0.3 GOTY.
//
// Each patch rewrites one 5-byte CALL in the game's code to go through us
// first. Every site is checked byte for byte before it is touched, so another
// build (the no-gore exe, a different patch level) is simply left alone.

#include "f3a/engine_hooks.h"
#include "f3a/logger.h"

#include <windows.h>
#include <intrin.h>
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
volatile DWORD  g_last_shot_at  = 0;

// Where the shot leaves from, as the function itself takes it (0x007735AE):
// its fifth argument, a point, when there is one; else the node in its
// second - the weapon's "ProjectileNode" at both player call sites - at its
// world position (+0x8C).
bool ShotOrigin(const uint32_t* args, float out[3])
{
    if (IsBadReadPtr(args, 0x18)) return false;
    const float* at = reinterpret_cast<const float*>(args[5]);
    if (!at && args[2]) at = reinterpret_cast<const float*>(args[2] + 0x8C);
    if (!at || IsBadReadPtr(at, 12)) return false;
    out[0] = at[0]; out[1] = at[1]; out[2] = at[2];
    return true;
}

// What the game measures a point's direction FROM (0x00773A8C): the centre of
// the player's 3D bound - a per-thread cache when it holds the player, else
// the reference's 3D (+0x5C, then +0x14) - its bound at +0x20.
bool ShotMeasuredFrom(const unsigned char* player, float out[3])
{
    const unsigned char* root = nullptr;
    auto* tls = reinterpret_cast<unsigned char* const*>(__readfsdword(0x2C));
    const DWORD index = *reinterpret_cast<const DWORD*>(0x01179AF4);
    const unsigned char* slot = tls && !IsBadReadPtr(tls + index, 4) ? tls[index] : nullptr;
    if (slot && !IsBadReadPtr(slot + 0x248, 8) &&
        *reinterpret_cast<const unsigned char* const*>(slot + 0x24C) == player) {
        root = *reinterpret_cast<const unsigned char* const*>(slot + 0x248);
    } else if (!IsBadReadPtr(player + 0x5C, 4)) {
        auto* rs = *reinterpret_cast<const unsigned char* const*>(player + 0x5C);
        if (rs && !IsBadReadPtr(rs + 0x14, 4))
            root = *reinterpret_cast<const unsigned char* const*>(rs + 0x14);
    }
    if (!root || IsBadReadPtr(root + 0x20, 4)) return false;
    auto* c = *reinterpret_cast<const float* const*>(root + 0x20);
    if (!c || IsBadReadPtr(c, 12)) return false;
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    return true;
}

// `args` = the stack as the call left it: the return address, then the
// function's seven arguments.
//
// The game aims the shot from the body's centre to the point, but fires it
// from the muzzle - in a line parallel to that one, forty-odd units above it
// in the third person, whatever the range. So the point handed over is the
// one that makes the body-centre line run the way muzzle -> target does.
void __cdecl PrepareShot(unsigned char* player, const uint32_t* args)
{
    g_last_shot_at = GetTickCount();
    float p[3];
    ShotAimProvider prov = g_shot_provider;
    if (!player || !prov || !prov(p)) return;
    if (IsBadWritePtr(player + 0x8E4, 0x10)) return;
    float from[3] = {}, centre[3] = {};
    bool moved = false;
    const bool haveFrom = ShotOrigin(args, from);
    const bool haveCentre = ShotMeasuredFrom(player, centre);
    // The player's position: TESObjectREFR posX/Y/Z at +0x2C (FOSE). It was
    // read at +0x30 - Y, Z and the scale - so the muzzle never counted as
    // near the player, and the correction below never ran: every shot of a
    // friend's fight logged "muzzle unknown" with "player (1600,12675,1)" for
    // a player at (-9612,1600,12675), and went ~50 units over the point aimed
    // at - over a mole rat's back (2026-11-06).
    const float* pos = reinterpret_cast<const float*>(player + 0x2C);
    if (haveFrom && haveCentre && !IsBadReadPtr(pos, 12)) {
        auto nearby = [&](const float* v) {
            const float dx = v[0] - pos[0], dy = v[1] - pos[1], dz = v[2] - pos[2];
            return dx * dx + dy * dy + dz * dz <= 300.0f * 300.0f;   // NaN fails too
        };
        if (nearby(from) && nearby(centre)) {
            for (int i = 0; i < 3; ++i) p[i] = centre[i] + (p[i] - from[i]);
            moved = true;
        }
    }
    *reinterpret_cast<void**>(player + 0x8E4) = nullptr;
    float* at = reinterpret_cast<float*>(player + 0x8E8);
    at[0] = p[0]; at[1] = p[1]; at[2] = p[2];
    static DWORD s_logged = 0;
    if (GetTickCount() - s_logged > 1000) {
        s_logged = GetTickCount();
        if (moved)
            F3A_INFO("ShotAim: from the muzzle (%.0f,%.0f,%.0f), the game measures from "
                     "(%.0f,%.0f,%.0f) - point set to (%.0f,%.0f,%.0f)",
                     from[0], from[1], from[2], centre[0], centre[1], centre[2], p[0], p[1], p[2]);
        else
            F3A_INFO("ShotAim: the shot is carried to (%.0f,%.0f,%.0f) (muzzle unknown: origin "
                     "%d (%.0f,%.0f,%.0f) [arg2 %08X arg5 %08X], body centre %d (%.0f,%.0f,%.0f), "
                     "player (%.0f,%.0f,%.0f))", p[0], p[1], p[2], (int)haveFrom, from[0], from[1],
                     from[2], args && !IsBadReadPtr(args, 0x18) ? args[2] : 0u,
                     args && !IsBadReadPtr(args, 0x18) ? args[5] : 0u, (int)haveCentre, centre[0],
                     centre[1], centre[2], pos[0], pos[1], pos[2]);
    }
}

void (__cdecl* g_prepare_shot)(unsigned char*, const uint32_t*) = &PrepareShot;

// Runs in place of the game's call: everything kept as it came (ecx = the
// player, the seven arguments on the stack), our point set, then on into the
// game's function, which returns to its caller.
__declspec(naked) void ShotDirection_Thunk()
{
    __asm {
        pushad
        pushfd
        lea eax, [esp + 0x24]          // the stack as the call left it
        push eax
        push ecx
        call dword ptr [g_prepare_shot]
        add esp, 8
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

// --- Left and Right in the container screen ------------------------------------
//
// The container screen's key handler (ContainerMenu slot 14, 0x0063ADE0)
// answers the bare Left and Right arrows (codes 3 / 4) by clicking the focused
// column's category arrow - in every mode but 4. The mod gives those keys to
// the column switch (Shift+arrows step the category, through the arrows' own
// buttons), so both happened at once: Right in your column stepped YOUR
// category (38 items -> 8 weapons) and then moved to the other column; Left
// came back and the game stepped the category again under the row the mod had
// just picked - the list rebuilt, nothing selected, Up and Down dead (log
// 2026-11-02, trading with Clover). Its dispatch is a byte table (0x0063AF64,
// codes 3..14) into five cases (0x0063AF50); case 0 is codes 3 and 4 alone.
// It is pointed at the handler's own "handled" exit (0x0063AF45: mov bl,1 /
// pop edi / pop esi / mov al,bl / pop ebx / ret 8), so the keys are taken and
// nothing is done with them. Barter has its own handler and keeps its arrows.
constexpr uintptr_t kContKeyCases = 0x0063AF50;
constexpr uintptr_t kContKeyBytes = 0x0063AF64;
constexpr uintptr_t kContKeyFilter = 0x0063AED3;
constexpr uintptr_t kContKeyHandled = 0x0063AF45;
bool g_cont_arrows_patched = false;

void PatchContainerArrows()
{
    auto* cases = reinterpret_cast<uint32_t*>(kContKeyCases);
    const uint32_t expectCases[5] = { kContKeyFilter, 0x0063AEC4, 0x0063AE1A, 0x0063AE6F, 0x0063AF47 };
    const uint8_t expectBytes[12] = { 0, 0, 4, 4, 4, 4, 4, 1, 4, 4, 2, 3 };
    const uint8_t exitCode[10] = { 0xB3, 0x01, 0x5F, 0x5E, 0x8A, 0xC3, 0x5B, 0xC2, 0x08, 0x00 };
    // lea eax,[edi-3] / cmp eax,0Bh / ja ... / movzx eax,byte [eax+0063AF64h] /
    // jmp [eax*4+0063AF50h]
    const uint8_t dispatch[] = { 0x8D, 0x47, 0xFD, 0x83, 0xF8, 0x0B, 0x0F, 0x87, 0x3B, 0x01, 0x00, 0x00,
                                 0x0F, 0xB6, 0x80, 0x64, 0xAF, 0x63, 0x00,
                                 0xFF, 0x24, 0x85, 0x50, 0xAF, 0x63, 0x00 };
    auto same = [](uintptr_t at, const void* want, size_t n) {
        const void* p = reinterpret_cast<const void*>(at);
        return !IsBadReadPtr(p, n) && std::memcmp(p, want, n) == 0;
    };
    DWORD old = 0;
    if (same(kContKeyCases, expectCases, sizeof(expectCases)) &&
        same(kContKeyBytes, expectBytes, sizeof(expectBytes)) &&
        same(kContKeyHandled, exitCode, sizeof(exitCode)) &&
        same(0x0063AE00, dispatch, sizeof(dispatch)) &&
        VirtualProtect(cases, 4, PAGE_EXECUTE_READWRITE, &old)) {
        cases[0] = kContKeyHandled;
        VirtualProtect(cases, 4, old, &old);
        g_cont_arrows_patched = true;
    }
    F3A_INFO("engine_hooks: container Left/Right (no category step) %s.",
             g_cont_arrows_patched ? "installed" : "NOT installed (code differs)");
}

// --- Up and Down in the container screen ---------------------------------------
//
// The interface hands a key to the top menu's slot 14 first (0x006240C0) and,
// when the menu does not take it, to the first list of that menu - in its own
// registry, 0x01075BD4 - that has a row in its "selected" pointer (ListBox
// +0x10, vtable[1] 0x00661970) and answers the key (vtable[2] 0x00632EB0:
// code 1 = up, 2 = down, from that row's list index). Which list that is
// depends on what was selected last and how: after the mod's column switches
// it was the other column, or none - Up and Down did nothing in the player's
// column after a trip to the companion's and back (log 2026-11-02, twice).
// The container screen's slot 14 (0x0063ADE0) does not take codes 1 and 2, so
// its vtable entry is pointed at a filter first: while the mod drives Up and
// Down in this screen itself (both columns, through the list's own select
// routine - as it already did in the other column), it takes them from the
// game. Barter keeps its own handler.
constexpr uintptr_t kContVtable     = 0x00E04E54;
constexpr uintptr_t kContSpecialKey = 0x0063ADE0;

ContainerKeyFilter g_cont_key_filter = nullptr;
bool g_cont_updown_patched = false;

bool __fastcall ContainerSpecialKey_Hook(void* menu, void* /*edx*/, uint32_t code, float state)
{
    ContainerKeyFilter f = g_cont_key_filter;
    if (f && f(code)) return true;
    using Fn = bool(__thiscall*)(void*, uint32_t, float);
    return reinterpret_cast<Fn>(kContSpecialKey)(menu, code, state);
}

void PatchContainerUpDown()
{
    auto* slot = reinterpret_cast<uint32_t*>(kContVtable + 14 * 4);
    DWORD old = 0;
    if (!IsBadReadPtr(slot, 4) && *slot == kContSpecialKey &&
        VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) {
        *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ContainerSpecialKey_Hook));
        VirtualProtect(slot, 4, old, &old);
        g_cont_updown_patched = true;
    }
    F3A_INFO("engine_hooks: container Up/Down by the mod %s.",
             g_cont_updown_patched ? "installed" : "NOT installed (code differs)");
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
    PatchContainerArrows();
    PatchContainerUpDown();
    return g_drop_patched && g_edit_arrows_patched && g_shot_patched == 3 && g_menu_key_patched &&
           g_cont_arrows_patched && g_cont_updown_patched;
}

void SetContainerKeyFilter(ContainerKeyFilter filter) { g_cont_key_filter = filter; }

uintptr_t ContainerKeySlotHook()
{
    return g_cont_updown_patched ? reinterpret_cast<uintptr_t>(&ContainerSpecialKey_Hook) : 0;
}

void SetShotAimProvider(ShotAimProvider provider) { g_shot_provider = provider; }

unsigned long LastPlayerShotAt() { return g_last_shot_at; }
void SetMenuEnterFilter(EnterFilter filter) { g_enter_filter = filter; }

void RemoveHooks() {}

} // namespace f3a::engine
