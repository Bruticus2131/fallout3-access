#include "f3a/logger.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace f3a::log {

namespace {
FILE*       g_file = nullptr;
std::mutex  g_mutex;

bool GetFosePath(char* out, size_t out_size, const char* filename)
{
    // %USERPROFILE%\Documents\My Games\Fallout3\FOSE\<filename>
    // — same convention FOSE plugins follow. SHGetFolderPathA is available
    // on every supported Windows (CSIDL_PERSONAL == Documents).
    char docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                SHGFP_TYPE_CURRENT, docs))) {
        return false;
    }
    snprintf(out, out_size, "%s\\My Games\\Fallout3\\FOSE", docs);
    CreateDirectoryA(out, nullptr);
    snprintf(out, out_size, "%s\\My Games\\Fallout3\\FOSE\\%s", docs, filename);
    return true;
}

// Separate, small file rewritten fresh on each BeginDump() — used by the
// F11 diagnostic so the user can paste a short tree dump instead of the
// huge rolling log.
FILE* g_dump = nullptr;
} // namespace

void Init()
{
    char path[MAX_PATH];
    if (!GetFosePath(path, sizeof(path), "fallout3_access.log")) return;
    g_file = std::fopen(path, "w");
    if (g_file) {
        std::fprintf(g_file,
            "Fallout3Access log started (v%d.%d.%d)\n",
            F3A_VERSION_MAJOR, F3A_VERSION_MINOR, F3A_VERSION_PATCH);
        std::fflush(g_file);
    }
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    if (g_dump) { std::fclose(g_dump); g_dump = nullptr; }
}

void BeginDump()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_dump) { std::fclose(g_dump); g_dump = nullptr; }
    char path[MAX_PATH];
    if (!GetFosePath(path, sizeof(path), "fallout3_access_dump.txt")) return;
    g_dump = std::fopen(path, "w");  // truncate: only the latest dump
}

void DumpWrite(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_dump) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_dump, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_dump);
    std::fflush(g_dump);
}

void EndDump()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_dump) { std::fclose(g_dump); g_dump = nullptr; }
}

void Write(const char* level, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] %-5s ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_file, fmt, ap);
    va_end(ap);

    std::fputc('\n', g_file);
    std::fflush(g_file);
}

} // namespace f3a::log
