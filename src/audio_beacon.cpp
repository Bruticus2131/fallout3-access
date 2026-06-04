#include "f3a/audio_beacon.h"
#include "f3a/logger.h"

#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <mutex>
#include <vector>

#pragma comment(lib, "winmm.lib")

// We synthesize each ping into a small 16-bit stereo PCM buffer and hand it to
// waveOut. A ring of headers lets pings overlap slightly without stalling the
// poll thread (waveOutWrite only queues). Headers are recycled once the device
// flags them WHDR_DONE.

namespace f3a::audio {
namespace {

constexpr int   kSampleRate = 22050;
constexpr int   kChannels   = 2;
constexpr int   kPingMs     = 90;
constexpr int   kNumBuffers = 8;

HWAVEOUT   g_wave = nullptr;
std::mutex g_mutex;

struct Buf {
    WAVEHDR            hdr{};
    std::vector<short> pcm;   // interleaved L,R
    bool               prepared = false;
};
Buf g_bufs[kNumBuffers];
int g_next = 0;

// constant-power pan + front/back pitch + distance attenuation
void Synthesize(std::vector<short>& out, float azimuth_deg, float dist01)
{
    if (dist01 < 0.0f) dist01 = 0.0f;
    if (dist01 > 1.0f) dist01 = 1.0f;

    const float az = azimuth_deg * 0.01745329252f;  // radians
    // Left/right: sin(az) is -1 (hard left) .. +1 (hard right).
    float lr = std::sin(az);
    if (lr < -1.0f) lr = -1.0f; else if (lr > 1.0f) lr = 1.0f;
    const float t = (lr + 1.0f) * 0.5f * 1.57079632679f;  // 0..pi/2
    const float gainL = std::cos(t);
    const float gainR = std::sin(t);

    // Front/back: cos(az) is +1 ahead .. -1 behind → pitch 1000 down to 500 Hz.
    const float frontness = std::cos(az);             // -1..1
    const float freq = 500.0f + (frontness + 1.0f) * 0.5f * 500.0f;

    // Distance: closer = louder.
    const float amp = 0.35f + (1.0f - dist01) * 0.65f;

    const int n = kSampleRate * kPingMs / 1000;
    out.resize((size_t)n * kChannels);
    const float twoPiFOverSr = 6.2831853f * freq / kSampleRate;
    const int attack = kSampleRate * 5 / 1000;        // 5 ms attack

    for (int i = 0; i < n; ++i) {
        // Envelope: short linear attack, then linear decay to zero.
        float env;
        if (i < attack) env = (float)i / attack;
        else            env = 1.0f - (float)(i - attack) / (n - attack);
        if (env < 0.0f) env = 0.0f;

        const float s = amp * env * std::sin(twoPiFOverSr * i);
        const short l = (short)(s * gainL * 30000.0f);
        const short r = (short)(s * gainR * 30000.0f);
        out[(size_t)i * 2 + 0] = l;
        out[(size_t)i * 2 + 1] = r;
    }
}

// Open the device. MUST be called with g_mutex held. We do NOT open at plugin
// load: waveOutOpen during the game's early init (FOSEPlugin_Load runs on the
// main thread before the engine's audio is up) hangs/crashes the game. Instead
// we open lazily on the first Ping, which only happens once the player is in
// the world.
bool OpenLocked()
{
    if (g_wave) return true;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = kChannels;
    fmt.nSamplesPerSec  = kSampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        F3A_WARN("audio: waveOutOpen failed (%u); beacon disabled.", mr);
        g_wave = nullptr;
        return false;
    }
    F3A_INFO("audio: beacon device ready.");
    return true;
}

} // namespace

bool Init()
{
    // Intentionally a no-op at load time — opening the device here crashes
    // the game. The device opens lazily on the first Ping().
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (auto& b : g_bufs) {
        if (b.prepared) {
            waveOutUnprepareHeader(g_wave, &b.hdr, sizeof(WAVEHDR));
            b.prepared = false;
        }
    }
    waveOutClose(g_wave);
    g_wave = nullptr;
}

bool Available()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_wave != nullptr;
}

void Ping(float azimuth_deg, float distance01)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_wave && !OpenLocked()) return;   // lazy open on first ping

    // Find a free buffer (one that's never been used or is flagged done).
    Buf* buf = nullptr;
    for (int i = 0; i < kNumBuffers; ++i) {
        Buf& b = g_bufs[g_next];
        g_next = (g_next + 1) % kNumBuffers;
        if (!b.prepared || (b.hdr.dwFlags & WHDR_DONE)) { buf = &b; break; }
    }
    if (!buf) return;  // all buffers still playing — skip this ping

    if (buf->prepared) {
        waveOutUnprepareHeader(g_wave, &buf->hdr, sizeof(WAVEHDR));
        buf->prepared = false;
    }

    Synthesize(buf->pcm, azimuth_deg, distance01);

    buf->hdr = WAVEHDR{};
    buf->hdr.lpData         = reinterpret_cast<LPSTR>(buf->pcm.data());
    buf->hdr.dwBufferLength = (DWORD)(buf->pcm.size() * sizeof(short));

    if (waveOutPrepareHeader(g_wave, &buf->hdr, sizeof(WAVEHDR))
            != MMSYSERR_NOERROR) {
        return;
    }
    buf->prepared = true;
    waveOutWrite(g_wave, &buf->hdr, sizeof(WAVEHDR));
}

} // namespace f3a::audio
