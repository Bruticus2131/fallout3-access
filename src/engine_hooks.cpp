// Engine hooks for Fallout3.exe v1.7.0.3 GOTY (Steam).
//
// Strategy: replace 5-byte CALL sites in cold paths with relative jumps to our
// own trampolines. We don't pull in a heavyweight detours library — the few
// call sites we need are simple enough to patch by hand. If we later want
// Detours/MinHook, replace the helpers in PatchCall() below.
//
// ALL OFFSETS BELOW ARE PLACEHOLDERS. They must be filled in from the FOSE
// runtime relocator (FOSE caches the right pointer for the matching runtime
// version). The convention used here matches existing FOSE patches.

#include "f3a/engine_hooks.h"
#include "f3a/menu_dispatch.h"
#include "f3a/plugin.h"
#include "f3a/logger.h"

#include <windows.h>
#include <cstdint>
#include <string>  // GameTiles.h uses std::string without including it

#if __has_include("fose_common/SafeWrite.h")
  #include "fose_common/SafeWrite.h"
  #include "fose/GameMenus.h"
  #define F3A_HAVE_FOSE_SDK 1
#else
  #define F3A_HAVE_FOSE_SDK 0
#endif

namespace f3a::engine {
namespace {

// --- 1.7.0.3 offsets (from FOSE source; verify before flipping the switch) ---
//
//   MenuManager::ShowMenu       0x00 (TODO)
//   Menu::Open / Menu::Close    via vftbl indices in fose/GameMenus.h
//   Main::Update                0x00 (TODO) — preferred tick site
//
// We don't hardcode here yet; the file gates real patching behind
// F3A_HAVE_FOSE_SDK so it compiles cleanly without the SDK present.

#if F3A_HAVE_FOSE_SDK

// Example skeleton — patch MenuManager::OpenMenu so we hear about every open:
//
//   typedef void (__thiscall *MM_OpenMenu_t)(MenuManager*, UInt32 menuId);
//   MM_OpenMenu_t g_orig_OpenMenu = nullptr;
//
//   void __fastcall HookedOpenMenu(MenuManager* self, void*, UInt32 menuId)
//   {
//       g_orig_OpenMenu(self, menuId);
//       f3a::menu::OnMenuOpen(static_cast<f3a::menu::Id>(menuId));
//   }
//
//   // In InstallHooks(): WriteRelCall(<call site addr>, &HookedOpenMenu);

#endif

} // namespace

bool InstallHooks()
{
#if F3A_HAVE_FOSE_SDK
    F3A_INFO("engine_hooks: SDK present; real patches not yet enabled (offsets TBD).");
    return false;
#else
    F3A_WARN("engine_hooks: FOSE SDK not available at compile time.");
    return false;
#endif
}

void RemoveHooks() {}

} // namespace f3a::engine
