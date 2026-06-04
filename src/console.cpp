#include "f3a/console.h"
#include "f3a/logger.h"

#include "fose/PluginAPI.h"

namespace f3a::console {
namespace {
FOSEConsoleInterface* g_console = nullptr;
}

bool Init(const void* fose_iface)
{
    if (!fose_iface) return false;
    auto* fose = static_cast<const FOSEInterface*>(fose_iface);
    g_console = (FOSEConsoleInterface*)fose->QueryInterface(kInterface_Console);
    if (!g_console || !g_console->RunScriptLine) {
        F3A_WARN("Console interface unavailable; debug commands disabled.");
        g_console = nullptr;
        return false;
    }
    F3A_INFO("Console interface acquired.");
    return true;
}

bool Available() { return g_console != nullptr; }

void Run(const char* command)
{
    if (!command || !*command) return;
    if (!g_console) {
        F3A_WARN("Console::Run('%s') ignored — interface not available.",
                 command);
        return;
    }
    F3A_INFO("Console::Run('%s')", command);
    g_console->RunScriptLine(command);
}

} // namespace f3a::console
