#include "f3a/console.h"
#include "f3a/logger.h"

#include "fose/PluginAPI.h"

namespace f3a::console {
namespace {
const FOSEInterface*  g_fose    = nullptr;   // stable copy owned by plugin_main
FOSEConsoleInterface* g_console = nullptr;

// FOSE returns null for the console interface when queried at plugin-load time
// (the console subsystem isn't up yet — messaging IS, which is why that one
// works early). So we don't give up at Init; we (re)try to acquire it on demand
// until it succeeds, which it does once a game is actually running.
void TryAcquire()
{
    if (g_console || !g_fose) return;
    g_console = (FOSEConsoleInterface*)g_fose->QueryInterface(kInterface_Console);
    if (g_console && !g_console->RunScriptLine) g_console = nullptr;
    if (g_console) F3A_INFO("Console interface acquired.");
}
} // namespace

bool Init(const void* fose_iface)
{
    g_fose = static_cast<const FOSEInterface*>(fose_iface);
    TryAcquire();   // fine if still null — Available()/Run retry later
    return g_fose != nullptr;
}

bool Available()
{
    TryAcquire();
    return g_console != nullptr;
}

void Run(const char* command)
{
    if (!command || !*command) return;
    TryAcquire();
    if (!g_console) {
        F3A_WARN("Console::Run('%s') ignored — interface not available.",
                 command);
        return;
    }
    F3A_INFO("Console::Run('%s')", command);
    g_console->RunScriptLine(command);
}

} // namespace f3a::console
