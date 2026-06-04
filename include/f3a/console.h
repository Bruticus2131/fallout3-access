#pragma once

// Thin wrapper over FOSE's console interface so we can run console commands
// programmatically (e.g. a debug hotkey that `coc`s into the world from the
// main menu, skipping the intro — invaluable when the tester can't see).

namespace f3a::console {

// Acquire the FOSE console interface. `fose_iface` is the FOSEInterface* the
// plugin received in FOSEPlugin_Load (passed as void* to avoid leaking FOSE
// headers into this lightweight header).
bool Init(const void* fose_iface);

// True if the console interface was acquired.
bool Available();

// Run a single console command line (e.g. "coc MegatonWorld").
void Run(const char* command);

} // namespace f3a::console
