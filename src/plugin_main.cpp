// Fallout3Access — FOSE plugin entry point.
//
// FOSE looks for two exported symbols and a global FOSEPlugin_Version on load:
//   - FOSEPlugin_Query(const FOSEInterface*, PluginInfo*) -> bool
//   - FOSEPlugin_Load (const FOSEInterface*)               -> bool
//
// We respond to FOSE messages to learn about game state transitions and to
// schedule one-shot init that has to happen after the main menu appears.

#include <windows.h>
#include <shlwapi.h>

#include "fose_common/fose_version.h"
#include "fose/PluginAPI.h"
#include "fose/PluginManager.h"
#include "common/IDebugLog.h"

#include "f3a/plugin.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/hotkeys.h"
#include "f3a/menu_dispatch.h"
#include "f3a/engine_hooks.h"
#include "f3a/polling_loop.h"
#include "f3a/modules.h"
#include "f3a/console.h"
#include "f3a/fose_runtime.h"

namespace tolk = f3a::tolk;
namespace cfg  = f3a::config;
namespace str  = f3a::strings;

// ---- Static plugin info ----------------------------------------------------

static HMODULE       g_module = nullptr;
static FOSEInterface g_fose   = {};

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
FOSEMessagingInterface* g_messaging = nullptr;

// ---- Forward decls ---------------------------------------------------------

static void OnFoseMessage(FOSEMessagingInterface::Message* msg);
static std::wstring ResolvePluginIniPath();

// ---- FOSE entry points -----------------------------------------------------

extern "C" {

__declspec(dllexport) bool FOSEPlugin_Query(const FOSEInterface* fose,
                                            PluginInfo*          info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name        = "Fallout3Access";
    info->version     = (F3A_VERSION_MAJOR << 16) | (F3A_VERSION_MINOR << 8)
                        | F3A_VERSION_PATCH;

    if (fose->isEditor) {
        // We don't run in the GECK.
        return false;
    }

    if (fose->foseVersion < FOSE_VERSION_INTEGER) {
        // Loader is older than what we were built against.
        return false;
    }

    // Runtime version check: Fallout 3 patched to 1.7.0.3 (Steam GOTY).
    // In fose_version.h this build is labelled FALLOUT_VERSION_1_7
    // (engine reports build 1.7.3).
    if (fose->runtimeVersion != FALLOUT_VERSION_1_7 &&
        fose->runtimeVersion != FALLOUT_VERSION_1_7ng) {
        return false;
    }

    return true;
}

__declspec(dllexport) bool FOSEPlugin_Load(const FOSEInterface* fose)
{
    g_fose         = *fose;
    g_pluginHandle = fose->GetPluginHandle();

    f3a::log::Init();
    F3A_INFO("FOSEPlugin_Load: runtime=0x%08X, fose=0x%08X",
             fose->runtimeVersion, fose->foseVersion);

    // Pick the correct hardcoded-address table for this exact runtime build
    // (standard 1.7.0.3 vs no-gore) before any memory read happens.
    f3a::fose_rt::SelectRuntime(fose->runtimeVersion);
    F3A_INFO("Address table selected for runtime 0x%08X.", fose->runtimeVersion);

    g_messaging = (FOSEMessagingInterface*)fose->QueryInterface(kInterface_Messaging);
    if (!g_messaging) {
        F3A_ERROR("Failed to acquire messaging interface.");
        return false;
    }
    g_messaging->RegisterListener(g_pluginHandle, "FOSE", OnFoseMessage);

    // Console interface — used by the debug "start game" hotkey.
    f3a::console::Init(&g_fose);

    // Load INI from <Game>/Data/FOSE/Plugins/Fallout3Access.ini
    std::wstring ini = ResolvePluginIniPath();
    cfg::Load(ini.c_str());

    // Bring up TTS bridge.
    if (!tolk::Init()) {
        F3A_ERROR("Tolk init failed; the mod will be silent.");
    }

    f3a::menu::Init();
    f3a::hotkeys::Init();
    f3a::engine::InstallHooks();   // (placeholder — disabled until needed)

    f3a::OnPluginLoad();

    // Kick off the worker thread that polls InterfaceManager visibility.
    f3a::poll::Start();

    // First spoken line — confirms TTS is working before the main menu loads.
    tolk::Speak(str::Render(str::Key::ModLoaded), tolk::Priority::System, true);
    return true;
}

} // extern "C"

// ---- Internal --------------------------------------------------------------

static std::wstring ResolvePluginIniPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_module, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    PathAppendW(buf, L"Fallout3Access.ini");
    return buf;
}

static void OnFoseMessage(FOSEMessagingInterface::Message* msg)
{
    switch (msg->type) {
    case FOSEMessagingInterface::kMessage_PostLoad:
        F3A_INFO("FOSE PostLoad");
        break;

    case FOSEMessagingInterface::kMessage_PostPostLoad:
        F3A_INFO("FOSE PostPostLoad — all plugins loaded.");
        break;

    case FOSEMessagingInterface::kMessage_LoadGame:
    case FOSEMessagingInterface::kMessage_PostLoadGame:
        F3A_INFO("Game loaded.");
        f3a::OnGameLoaded();
        break;

    case FOSEMessagingInterface::kMessage_NewGame:
        F3A_INFO("New game.");
        f3a::OnNewGame();
        break;

    case FOSEMessagingInterface::kMessage_ExitGame:
    case FOSEMessagingInterface::kMessage_ExitGame_Console:
        f3a::OnGameExit();
        f3a::poll::Stop();
        f3a::modules::ShutdownAll();
        tolk::Shutdown();
        f3a::menu::Shutdown();
        f3a::hotkeys::Shutdown();
        f3a::log::Shutdown();
        break;

    case FOSEMessagingInterface::kMessage_ExitToMainMenu:
        F3A_INFO("Returned to main menu.");
        break;

    default:
        break;
    }
}

// ---- Dll entry -------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_module = hinst;
        DisableThreadLibraryCalls(hinst);
        break;
    }
    return TRUE;
}

// ---- High-level lifecycle hooks (called from FOSEPlugin_Load) --------------

namespace f3a {

void OnPluginLoad()
{
    f3a::modules::InitAll();
    F3A_INFO("Mod initialized. Active reader: %s",
             tolk::ActiveReaderName());
}

void OnGameLoaded()
{
    f3a::hotkeys::Rebind();
    tolk::Speak(str::Render(str::Key::LoadingDone),
                tolk::Priority::System, true);
}

void OnNewGame()
{
    OnGameLoaded();
}

void OnGameExit() {}

void OnFrame(float dt)
{
    if (!cfg::IsEnabled()) return;
    f3a::hotkeys::Poll();
    f3a::menu::OnTick(dt);
}

} // namespace f3a
