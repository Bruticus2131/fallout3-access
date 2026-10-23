#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"
#include "f3a/config.h"

#include <windows.h>
#include <objbase.h>

#include <prism.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Speech goes through Prism (github.com/ethindp/prism, MPL-2.0): one C API in
// front of NVDA, JAWS, ZoomText, System Access, ZDSR and the rest, with
// Windows' own voices (OneCore, SAPI) when no screen reader runs. It is built
// into this DLL - nothing beside the game, where Tolk needed four DLLs.
//
// Prism's backends are not thread-safe, several of them talk COM, and the mod
// speaks from the poll thread, the game's thread and the hotkey thread. So
// every Prism call is made on one thread of our own, which owns COM; Speak()
// only queues. That also keeps the game's thread from ever waiting on a
// screen reader.

namespace f3a::tolk {
namespace {

using Clock = std::chrono::steady_clock;

struct Cmd {
    enum Kind { Say, Stop } kind = Say;
    std::string text;
    bool interrupt = false;
};

// Front end: what Speak() decides, and the queue it feeds. g_mutex guards all
// of it; the speech thread takes it only to pick commands up.
std::mutex              g_mutex;
std::condition_variable g_cv;
std::deque<Cmd>         g_queue;
std::atomic<int>        g_current_prio{ -1 };
// When the last accepted utterance was issued. g_current_prio is only meaningful
// while that utterance is still playing; we use this (with IsSpeaking) to clear
// a stale priority once the reader goes idle.
Clock::time_point       g_last_speak_at{};
// When the last utterance is likely to have finished, from its length - for a
// reader that cannot say whether it is still talking (NVDA's cannot).
Clock::time_point       g_speech_ends_at{};
std::string             g_last_text;      // most recent utterance (for RepeatLast)

// The speech thread and what it found.
std::thread             g_worker;
std::atomic<bool>       g_quit{ false };
std::atomic<bool>       g_done{ false };           // the speech thread has finished
std::atomic<bool>       g_ready{ false };          // a backend is up
std::atomic<bool>       g_reported_speaking{ false };
std::atomic<bool>       g_can_report{ false };     // the backend says when it talks
std::atomic<bool>       g_screen_reader{ false };  // a screen reader, not a bare voice
std::atomic<bool>       g_recheck{ false };        // availability changed: choose again
std::atomic<long long>  g_said_at_ms{ 0 };         // last Say handed to the backend
char                    g_name[64] = "none";       // the backend's name, for the log

// Owned by the speech thread alone.
PrismContext*           g_ctx = nullptr;
PrismBackend*           g_backend = nullptr;
uint64_t                g_features = 0;

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

bool IsBareVoice(PrismBackendId id)
{
    return id == PRISM_BACKEND_SAPI || id == PRISM_BACKEND_ONE_CORE;
}

// Prism's own warnings in our log: which reader it tried and why one failed.
void PRISM_CALL OnPrismLog(void*, PrismLogLevel level, const char* source, const char* message)
{
    if (level >= PRISM_LOG_LEVEL_WARN)
        F3A_WARN("Prism %s: %s", source ? source : "", message ? message : "");
    else
        F3A_INFO("Prism %s: %s", source ? source : "", message ? message : "");
}

// A screen reader started or stopped (NVDA launched after the game): choose
// the backend again on the speech thread.
void PRISM_CALL OnAvailability(void*, PrismBackendId, const char* name, bool available)
{
    F3A_INFO("Speech: %s %s", name ? name : "?", available ? "is available" : "went away");
    g_recheck.store(true);
    g_cv.notify_all();
}

// The best backend now: [General] PreferSAPI = 1 asks for SAPI even with a
// screen reader running; else the screen reader first, Windows' voices last.
PrismBackend* ChooseBackend()
{
    if (config::Get().prefer_sapi) {
        if (PrismBackend* b = prism_registry_acquire(g_ctx, PRISM_BACKEND_SAPI)) {
            PrismError e = prism_backend_initialize(b);
            if (e == PRISM_OK || e == PRISM_ERROR_ALREADY_INITIALIZED) return b;
            prism_backend_free(b);
            F3A_WARN("Speech: SAPI asked for but did not start (%s)", prism_error_string(e));
        }
    }
    return prism_registry_acquire_best(g_ctx);
}

void UseBackend(PrismBackend* b)
{
    g_backend  = b;
    g_features = b ? prism_backend_get_features(b) : 0;
    const char* nm = b ? prism_backend_name(b) : nullptr;
    char was[sizeof(g_name)];
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::snprintf(was, sizeof(was), "%s", g_name);
        std::snprintf(g_name, sizeof(g_name), "%s", nm && *nm ? nm : "none");
    }
    PrismBackendId id = PRISM_BACKEND_INVALID;
    if (nm) id = prism_registry_id(g_ctx, nm);
    g_screen_reader.store(b && !IsBareVoice(id));
    g_can_report.store(b && (g_features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING));
    g_reported_speaking.store(false);
    g_ready.store(b != nullptr);
    if (std::strcmp(was, g_name) == 0) return;     // chosen again, the same one
    if (b)
        F3A_INFO("Speech: speaking through %s (%s)", g_name,
                 g_screen_reader.load() ? "screen reader" : "Windows voice");
    else
        F3A_ERROR("Speech: no screen reader and no Windows voice could be started");
}

// Choose again from scratch. The old handle goes first: Prism hands back a
// cached instance, and NVDA's, kept alive by us, would be chosen again after
// NVDA itself had quit.
void Rechoose()
{
    if (g_backend) { prism_backend_free(g_backend); g_backend = nullptr; }
    UseBackend(ChooseBackend());
}

void Run(const Cmd& c)
{
    if (!g_backend) return;
    PrismError e = PRISM_OK;
    if (c.kind == Cmd::Stop) {
        if (g_features & PRISM_BACKEND_SUPPORTS_STOP) e = prism_backend_stop(g_backend);
    } else {
        // Output = speech and braille, where the reader has braille.
        if (g_features & PRISM_BACKEND_SUPPORTS_OUTPUT)
            e = prism_backend_output(g_backend, c.text.c_str(), c.interrupt);
        else
            e = prism_backend_speak(g_backend, c.text.c_str(), c.interrupt);
        g_said_at_ms.store(NowMs());
    }
    if (e != PRISM_OK && e != PRISM_ERROR_NOT_SPEAKING) {
        F3A_WARN("Speech: %s failed (%s) - choosing the backend again",
                 c.kind == Cmd::Stop ? "stop" : "speak", prism_error_string(e));
        g_recheck.store(true);           // the reader went away under us
    }
}

void Worker(std::promise<bool> started)
{
    // COM for the backends that use it (JAWS, UIA, ZoomText); free-threaded,
    // as this thread pumps no messages.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com = SUCCEEDED(hr);

    prism_set_log_handler(PrismLogHandler{ &OnPrismLog, nullptr });
    prism_set_log_level(PRISM_LOG_LEVEL_WARN);
    PrismConfig cfg = prism_config_init();
    cfg.availability_callback = &OnAvailability;
    g_ctx = prism_init(&cfg);
    if (!g_ctx) {
        F3A_ERROR("Speech: Prism did not start");
        started.set_value(false);
        if (com) CoUninitialize();
        g_done.store(true);
        return;
    }
    UseBackend(ChooseBackend());
    started.set_value(g_backend != nullptr);

    std::unique_lock<std::mutex> lk(g_mutex);
    while (!g_quit.load()) {
        g_cv.wait_for(lk, std::chrono::milliseconds(50),
                      [] { return g_quit.load() || !g_queue.empty() || g_recheck.load(); });
        while (!g_queue.empty() && !g_quit.load()) {
            Cmd c = std::move(g_queue.front());
            g_queue.pop_front();
            lk.unlock();
            Run(c);
            lk.lock();
        }
        lk.unlock();
        if (g_recheck.exchange(false)) Rechoose();
        if (g_backend && g_can_report.load()) {
            bool speaking = false;
            if (prism_backend_is_speaking(g_backend, &speaking) == PRISM_OK)
                g_reported_speaking.store(speaking);
        }
        lk.lock();
    }
    lk.unlock();
    if (g_backend) { prism_backend_free(g_backend); g_backend = nullptr; }
    prism_shutdown(g_ctx);
    g_ctx = nullptr;
    if (com) CoUninitialize();
    g_done.store(true);
}

// Under g_mutex. Still talking, as far as anyone can tell: what the backend
// reports (with a moment's grace, as it is asked every 50 ms), else the
// estimate from the length of what was sent.
bool SpeakingLocked()
{
    const auto now = Clock::now();
    if (!g_queue.empty()) return true;
    if (g_can_report.load())
        return g_reported_speaking.load() || NowMs() - g_said_at_ms.load() < 250;
    return now < g_speech_ends_at;
}

} // namespace

bool Init()
{
    if (g_worker.joinable()) return g_ready.load();
    g_quit.store(false);
    std::promise<bool> started;
    std::future<bool> result = started.get_future();
    g_worker = std::thread(&Worker, std::move(started));
    // Wait for the first choice, so the log line after this names the reader.
    // A slow screen reader keeps starting in the background.
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        F3A_WARN("Speech: still looking for a screen reader after 5 s");
        return true;
    }
    return result.get();
}

void Shutdown()
{
    if (!g_worker.joinable()) return;
    g_quit.store(true);
    g_cv.notify_all();
    // A reader stuck in a call must not hold the game up on its way out.
    for (int i = 0; i < 200 && !g_done.load(); ++i) Sleep(10);
    if (g_done.load()) {
        g_worker.join();
    } else {
        F3A_WARN("Speech: the speech thread did not stop within 2 s - leaving it");
        g_worker.detach();
    }
}

bool HasActiveScreenReader()
{
    return g_ready.load() && g_screen_reader.load();
}

void Refresh()
{
    g_recheck.store(true);
    g_cv.notify_all();
}

const char* ActiveReaderName()
{
    return g_name;
}

// Recently spoken, for suppressing ambient repetition.
//
// The game re-announces the same tutorial hint over and over — its tiles blink,
// they are rebuilt, several readers can see the same text — and the player is
// left listening to "hold aim to..." on a loop while trying to hear something
// that matters. Deduplicating inside each reader only fixes the reader you
// happen to think of; this catches the lot.
//
// The rule is deliberately narrow: only speech that does NOT interrupt is
// suppressed. Anything the player asked for by pressing a key interrupts, and
// pressing a key twice to hear the same thing twice must always work. (The FNV
// accessibility mod does the same at its own notification hook, with a
// two-second window.)
struct RecentLine { std::string text; Clock::time_point at; };
std::vector<RecentLine> g_recent;
constexpr int kRecentKeep = 32;
// Two seconds, as in the FNV mod: only the SAME line arriving twice in a
// row is swallowed. It used to be five minutes for everything, which also
// silenced a second "+25 PD", "Weź Stimpak" for the next stimpak, the next
// "Otwieram drzwi"... The game's recurring hints have their own once-per-
// session rule now (polling_loop LooksLikeHint), so this needn't do that job.
const auto kRepeatAfter = std::chrono::seconds(2);

bool SaidRecently(std::string_view text)
{
    auto now = Clock::now();
    for (const auto& r : g_recent)
        if (r.text == text && now - r.at < kRepeatAfter) return true;
    return false;
}

void RememberSaid(std::string_view text)
{
    auto now = Clock::now();
    for (auto& r : g_recent)
        if (r.text == text) { r.at = now; return; }
    if ((int)g_recent.size() >= kRecentKeep) g_recent.erase(g_recent.begin());
    g_recent.push_back({ std::string(text), now });
}

void Speak(std::string_view text, Priority prio, bool interrupt)
{
    if (text.empty()) return;

    std::lock_guard<std::mutex> lk(g_mutex);

    if (!interrupt && SaidRecently(text)) return;

    // g_current_prio holds the priority of the last utterance, but it's only
    // meaningful WHILE that utterance is still playing. It is never cleared on
    // completion, so without this a single System/Ui line would permanently
    // suppress every later Background line (e.g. the world-scan list read on X/C
    // went silent after the first quest/menu announce). Clear it once the reader
    // is idle — as the backend reports it, plus a wall-clock fallback.
    auto now = Clock::now();
    bool idle = !SpeakingLocked();
    if (now - g_last_speak_at > std::chrono::milliseconds(2500)) idle = true;
    if (idle) g_current_prio.store(-1);

    const int current = g_current_prio.load();
    // Drop only if a STRICTLY higher-priority utterance is still in flight.
    if (!interrupt && current > static_cast<int>(prio)) return;

    // Remembered only once it is really going to be spoken: a line dropped for
    // priority must neither block its own next try nor be what RepeatLast
    // replays.
    RememberSaid(text);
    g_last_text.assign(text);
    F3A_DEBUG("say: %.*s", static_cast<int>(text.size()), text.data());

    g_current_prio.store(static_cast<int>(prio));
    g_last_speak_at = now;
    {
        // ~45 ms a character at a screen reader's usual brisk rate; queued
        // lines follow the one before them.
        auto start = (!interrupt && g_speech_ends_at > now) ? g_speech_ends_at : now;
        g_speech_ends_at = start + std::chrono::milliseconds(300 + 45 * (long long)text.size());
    }

    // An interrupting line cuts off whatever was still waiting to be said.
    if (interrupt) g_queue.clear();
    g_queue.push_back({ Cmd::Say, std::string(text), interrupt });
    g_cv.notify_all();
}

bool IsSpeaking()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return SpeakingLocked();
}

void Silence()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_current_prio.store(-1);
    g_speech_ends_at = {};
    g_queue.clear();
    g_queue.push_back({ Cmd::Stop, {}, true });
    g_cv.notify_all();
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
