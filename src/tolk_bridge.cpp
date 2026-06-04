#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"

#include <windows.h>
#include <sapi.h>
// Note: we avoid <atlbase.h> (not in MSVC Build Tools without ATL workload)
// and <wrl/client.h> (requires NTDDI_VISTA, while FOSE forces NTDDI_WIN2K).
// Raw ISpVoice* with manual Release is fine for our two call sites.

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

// Tolk has a stable C ABI. We dynamically load Tolk.dll so the mod still works
// (degraded, via SAPI) if Tolk.dll is absent.

namespace f3a::tolk {
namespace {

// ---- Tolk function pointers -------------------------------------------------

using PFN_Tolk_Load             = void  (*)();
using PFN_Tolk_Unload           = void  (*)();
using PFN_Tolk_IsLoaded         = bool  (*)();
using PFN_Tolk_TrySAPI          = void  (*)(bool);
using PFN_Tolk_PreferSAPI       = void  (*)(bool);
using PFN_Tolk_DetectScreenReader = const wchar_t* (*)();
using PFN_Tolk_HasSpeech        = bool  (*)();
using PFN_Tolk_Output           = bool  (*)(const wchar_t*, bool);
using PFN_Tolk_Speak            = bool  (*)(const wchar_t*, bool);
using PFN_Tolk_Silence          = bool  (*)();
using PFN_Tolk_IsSpeaking       = bool  (*)();

struct TolkAPI {
    HMODULE                       dll = nullptr;
    PFN_Tolk_Load                 Load = nullptr;
    PFN_Tolk_Unload               Unload = nullptr;
    PFN_Tolk_IsLoaded             IsLoaded = nullptr;
    PFN_Tolk_TrySAPI              TrySAPI = nullptr;
    PFN_Tolk_PreferSAPI           PreferSAPI = nullptr;
    PFN_Tolk_DetectScreenReader   Detect = nullptr;
    PFN_Tolk_HasSpeech            HasSpeech = nullptr;
    PFN_Tolk_Output               Output = nullptr;
    PFN_Tolk_Speak                Speak = nullptr;
    PFN_Tolk_Silence              Silence = nullptr;
    PFN_Tolk_IsSpeaking           IsSpeaking = nullptr;
};

TolkAPI            g_tolk;
ISpVoice*  g_sapi;          // SAPI fallback
std::mutex         g_mutex;
std::atomic<int>   g_current_prio{ -1 };
std::wstring       g_reader_name;   // cached detected reader
std::string        g_reader_name_a;
std::string        g_last_text;     // most recent utterance (for RepeatLast)

bool ResolveTolk(HMODULE dll, TolkAPI& api)
{
    api.dll      = dll;
    #define RESOLVE(field, name) \
        api.field = reinterpret_cast<decltype(api.field)>(GetProcAddress(dll, name)); \
        if (!api.field) { F3A_WARN("Tolk: missing export '%s'", name); return false; }

    RESOLVE(Load,        "Tolk_Load");
    RESOLVE(Unload,      "Tolk_Unload");
    RESOLVE(IsLoaded,    "Tolk_IsLoaded");
    RESOLVE(TrySAPI,     "Tolk_TrySAPI");
    RESOLVE(PreferSAPI,  "Tolk_PreferSAPI");
    RESOLVE(Detect,      "Tolk_DetectScreenReader");
    RESOLVE(HasSpeech,   "Tolk_HasSpeech");
    RESOLVE(Output,      "Tolk_Output");
    RESOLVE(Speak,       "Tolk_Speak");
    RESOLVE(Silence,     "Tolk_Silence");
    RESOLVE(IsSpeaking,  "Tolk_IsSpeaking");
    #undef RESOLVE
    return true;
}

bool InitSapiFallback()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        F3A_ERROR("SAPI fallback: CoInitializeEx failed (hr=0x%08x)", hr);
        return false;
    }
    hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ISpVoice, (void**)&g_sapi);
    if (FAILED(hr) || !g_sapi) {
        F3A_ERROR("SAPI fallback: failed to create SpVoice (hr=0x%08x)", hr);
        return false;
    }
    F3A_INFO("SAPI fallback ready.");
    return true;
}

std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                     nullptr, 0);
    std::wstring w(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), needed);
    return w;
}

std::string WideToUtf8(const wchar_t* s)
{
    if (!s) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string a(needed > 0 ? needed - 1 : 0, '\0');
    if (needed > 1) {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, a.data(), needed, nullptr, nullptr);
    }
    return a;
}

} // namespace

bool Init()
{
    std::lock_guard<std::mutex> lk(g_mutex);

    HMODULE dll = LoadLibraryW(L"Tolk.dll");
    if (dll && ResolveTolk(dll, g_tolk)) {
        g_tolk.Load();
        g_tolk.TrySAPI(true);     // SAPI as last resort inside Tolk
        const wchar_t* name = g_tolk.Detect();
        if (name) {
            g_reader_name = name;
            g_reader_name_a = WideToUtf8(name);
            F3A_INFO("Tolk loaded. Active reader: %s", g_reader_name_a.c_str());
        } else {
            F3A_INFO("Tolk loaded. No screen reader detected; will use SAPI via Tolk.");
        }
        return true;
    }

    if (dll) FreeLibrary(dll);
    g_tolk = TolkAPI{};
    F3A_WARN("Tolk.dll not available — falling back to direct SAPI.");
    return InitSapiFallback();
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_tolk.dll) {
        if (g_tolk.Unload) g_tolk.Unload();
        FreeLibrary(g_tolk.dll);
        g_tolk = TolkAPI{};
    }
    if (g_sapi) { g_sapi->Release(); g_sapi = nullptr; }
}

bool HasActiveScreenReader()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_tolk.HasSpeech) return g_tolk.HasSpeech();
    return (bool)g_sapi;
}

void Refresh()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_tolk.Detect) return;
    const wchar_t* name = g_tolk.Detect();
    if (name) {
        g_reader_name = name;
        g_reader_name_a = WideToUtf8(name);
    } else {
        g_reader_name.clear();
        g_reader_name_a.clear();
    }
}

const char* ActiveReaderName()
{
    return g_reader_name_a.empty() ? "SAPI" : g_reader_name_a.c_str();
}

void Speak(std::string_view text, Priority prio, bool interrupt)
{
    if (text.empty()) return;

    std::lock_guard<std::mutex> lk(g_mutex);

    // Remember the last thing we actually spoke so RepeatLast can replay it.
    g_last_text.assign(text);

    const int current = g_current_prio.load();
    if (!interrupt && current >= static_cast<int>(prio)) {
        // Lower-or-equal priority utterance is in flight; drop us silently.
        // (Higher priority always interrupts.)
        if (current > static_cast<int>(prio)) return;
    }

    g_current_prio.store(static_cast<int>(prio));

    std::wstring w = Utf8ToWide(text);

    if (g_tolk.Output) {
        // Output() queues; Speak() interrupts. Map by `interrupt`.
        if (interrupt) g_tolk.Speak(w.c_str(), true);
        else           g_tolk.Output(w.c_str(), false);
        return;
    }

    if (g_sapi) {
        DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
        if (interrupt) flags |= SPF_PURGEBEFORESPEAK;
        g_sapi->Speak(w.c_str(), flags, nullptr);
    }
}

void Silence()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_current_prio.store(-1);
    if (g_tolk.Silence) { g_tolk.Silence(); return; }
    if (g_sapi)         { g_sapi->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr); }
}

void RepeatLast()
{
    std::string copy;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        copy = g_last_text;
    }
    if (copy.empty()) return;
    // System priority + interrupt so the repeat always comes through.
    Speak(copy, Priority::System, true);
}

} // namespace f3a::tolk
