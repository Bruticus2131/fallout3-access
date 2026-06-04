#pragma once
#include <cstdarg>

namespace f3a::log {

void Init();
void Shutdown();
void Write(const char* level, const char* fmt, ...);

// Diagnostic dump sink — a small separate file (fallout3_access_dump.txt)
// rewritten on each BeginDump(). Use for the F11 menu-tree dump so it stays
// short and pasteable instead of buried in the rolling log.
void BeginDump();
void DumpWrite(const char* fmt, ...);
void EndDump();

}

#define F3A_INFO(...)  ::f3a::log::Write("INFO",  __VA_ARGS__)
#define F3A_WARN(...)  ::f3a::log::Write("WARN",  __VA_ARGS__)
#define F3A_ERROR(...) ::f3a::log::Write("ERROR", __VA_ARGS__)
#define F3A_DEBUG(...) ::f3a::log::Write("DEBUG", __VA_ARGS__)
