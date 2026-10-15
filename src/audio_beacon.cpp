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
// The sonar sweep's own device and buffer (see Sweep).
HWAVEOUT g_sweep_wave = nullptr;
Buf      g_sweep_buf;

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
bool OpenDeviceLocked(HWAVEOUT& h)
{
    if (h) return true;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = kChannels;
    fmt.nSamplesPerSec  = kSampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    MMRESULT mr = waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        F3A_WARN("audio: waveOutOpen failed (%u); beacon disabled.", mr);
        h = nullptr;
        return false;
    }
    F3A_INFO("audio: beacon device ready.");
    return true;
}

bool OpenLocked() { return OpenDeviceLocked(g_wave); }

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
    if (g_sweep_wave) {
        waveOutReset(g_sweep_wave);
        if (g_sweep_buf.prepared) {
            waveOutUnprepareHeader(g_sweep_wave, &g_sweep_buf.hdr, sizeof(WAVEHDR));
            g_sweep_buf.prepared = false;
        }
        waveOutClose(g_sweep_wave);
        g_sweep_wave = nullptr;
    }
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

// A clean centred "on-target" blip (equal L/R) at `freq` Hz for `ms`, used by
// the line-of-sight aim cue. Short fades top and tail so it doesn't click.
void SynthCue(std::vector<short>& out, float freq, int ms)
{
    const int n = kSampleRate * ms / 1000;
    out.resize((size_t)n * kChannels);
    const float w = 6.2831853f * freq / kSampleRate;
    int fade = kSampleRate * 6 / 1000;            // 6 ms in/out fade
    if (fade > n / 2) fade = n / 2;
    for (int i = 0; i < n; ++i) {
        float env = 1.0f;
        if (i < fade)          env = (float)i / (fade ? fade : 1);
        else if (i > n - fade) env = (float)(n - i) / (fade ? fade : 1);
        if (env < 0.0f) env = 0.0f;
        short s = (short)(0.5f * env * std::sin(w * i) * 30000.0f);
        out[(size_t)i * 2 + 0] = s;
        out[(size_t)i * 2 + 1] = s;
    }
}

// Queue `pcm` on a free waveOut buffer (caller holds g_mutex, device open).
void EmitLocked(std::vector<short>& pcm)
{
    Buf* buf = nullptr;
    for (int i = 0; i < kNumBuffers; ++i) {
        Buf& b = g_bufs[g_next];
        g_next = (g_next + 1) % kNumBuffers;
        if (!b.prepared || (b.hdr.dwFlags & WHDR_DONE)) { buf = &b; break; }
    }
    if (!buf) return;  // all buffers still playing — skip

    if (buf->prepared) {
        waveOutUnprepareHeader(g_wave, &buf->hdr, sizeof(WAVEHDR));
        buf->prepared = false;
    }
    buf->pcm.swap(pcm);

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

void Ping(float azimuth_deg, float distance01)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_wave && !OpenLocked()) return;   // lazy open on first ping
    std::vector<short> pcm;
    Synthesize(pcm, azimuth_deg, distance01);
    EmitLocked(pcm);
}

void Cue(int freqHz, int ms)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_wave && !OpenLocked()) return;
    std::vector<short> pcm;
    SynthCue(pcm, (float)freqHz, ms);
    EmitLocked(pcm);
}

namespace {
// One voice of the sonar, mixed into L/R starting at sample `start`.
// The whole sweep is built as ONE buffer: waveOut plays queued buffers one
// after another, never together, so separate blips could not overlap and the
// timing would drift with every one of them.
void SweepVoice(std::vector<float>& L, std::vector<float>& R, int start,
                float azimuth_deg, float amp, int kind)
{
    const float az = azimuth_deg * 0.01745329252f;
    float lr = std::sin(az);
    if (lr < -1.0f) lr = -1.0f; else if (lr > 1.0f) lr = 1.0f;
    const float t  = (lr + 1.0f) * 0.5f * 1.57079632679f;
    const float gL = std::cos(t), gR = std::sin(t);
    // Behind: quieter and a touch lower, so front and back don't sound alike.
    const float front = std::cos(az);
    const float pitch = front < 0.0f ? 1.0f + 0.15f * front : 1.0f;
    if (front < 0.0f) amp *= 1.0f + 0.35f * front;

    const float sr = (float)kSampleRate;
    const float twoPi = 6.2831853f;
    int len = 0;
    switch (kind) {
        case kSweepHostile:   len = 230; break;
        case kSweepPerson:    len = 120; break;
        case kSweepContainer: len = 320; break;
        case kSweepDoor:      len = 150; break;
        default:              len = 40;  break;   // item
    }
    const int n = kSampleRate * len / 1000;
    const int attack = kSampleRate * 4 / 1000;
    for (int i = 0; i < n; ++i) {
        int k = start + i;
        if (k < 0 || k >= (int)L.size()) break;
        float ts = (float)i / sr;
        float env = (i < attack) ? (float)i / attack : 1.0f;
        float s = 0.0f;
        switch (kind) {
        case kSweepHostile: {           // a low growl: buzzy, with tremolo
            float f = 150.0f * pitch;
            float x = std::sin(twoPi * f * ts);
            s = std::tanh(3.0f * x) * (0.6f + 0.4f * std::sin(twoPi * 25.0f * ts));
            env *= std::exp(-ts / 0.12f);
            break; }
        case kSweepPerson: {            // a soft, round tone
            float f = 520.0f * pitch;
            s = std::sin(twoPi * f * ts);
            env *= std::exp(-ts / 0.05f);
            break; }
        case kSweepContainer: {         // a small bell: loot here
            float f1 = 1046.5f * pitch, f2 = 1568.0f * pitch;
            s = 0.6f * std::sin(twoPi * f1 * ts) + 0.4f * std::sin(twoPi * f2 * ts);
            env *= std::exp(-ts / 0.09f);
            break; }
        case kSweepDoor: {              // two knocks
            float local = ts < 0.075f ? ts : ts - 0.075f;
            float f = 220.0f * pitch;
            s = 0.7f * std::sin(twoPi * f * local) + 0.3f * std::sin(twoPi * 2.0f * f * local);
            env *= std::exp(-local / 0.012f);
            break; }
        default: {                      // a tick: a loose item
            float f = 2000.0f * pitch;
            s = std::sin(twoPi * f * ts);
            env *= std::exp(-ts / 0.01f);
            break; }
        }
        float v = amp * env * s;
        L[k] += v * gL;
        R[k] += v * gR;
    }
}

// A quiet orientation tick (12/3/6/9 o'clock): short, panned, distinct for
// "ahead".
void SweepTick(std::vector<float>& L, std::vector<float>& R, int start,
               float azimuth_deg)
{
    const float az = azimuth_deg * 0.01745329252f;
    float lr = std::sin(az);
    const float t  = (lr + 1.0f) * 0.5f * 1.57079632679f;
    const float gL = std::cos(t), gR = std::sin(t);
    const float f = azimuth_deg == 0.0f ? 1400.0f
                  : (std::cos(az) < -0.5f ? 500.0f : 900.0f);
    const int n = kSampleRate * 25 / 1000;
    for (int i = 0; i < n; ++i) {
        int k = start + i;
        if (k < 0 || k >= (int)L.size()) break;
        float ts = (float)i / kSampleRate;
        float v = 0.10f * std::exp(-ts / 0.006f) * std::sin(6.2831853f * f * ts);
        L[k] += v * gL;
        R[k] += v * gR;
    }
}
} // namespace

void Sweep(const std::vector<SweepBlip>& blips, int total_ms)
{
    if (total_ms < 500)  total_ms = 500;
    if (total_ms > 6000) total_ms = 6000;
    const int body = kSampleRate * total_ms / 1000;
    const int tail = kSampleRate * 350 / 1000;     // let the last voice ring out
    std::vector<float> L((size_t)(body + tail), 0.0f), R((size_t)(body + tail), 0.0f);

    auto at = [&](float azimuth_deg) {
        float a = azimuth_deg < 0.0f ? azimuth_deg + 360.0f : azimuth_deg;
        if (a >= 360.0f) a -= 360.0f;
        return (int)(a / 360.0f * body);
    };
    for (float a : { 0.0f, 90.0f, 180.0f, -90.0f }) SweepTick(L, R, at(a), a);
    SweepTick(L, R, body, 0.0f);                   // back at 12: the sweep is done
    for (const auto& b : blips) {
        float d = b.distance01 < 0.0f ? 0.0f : (b.distance01 > 1.0f ? 1.0f : b.distance01);
        float amp = 0.25f + 0.75f * (1.0f - d);
        if (!std::isfinite(b.azimuth_deg)) continue;
        SweepVoice(L, R, at(b.azimuth_deg), b.azimuth_deg, amp, b.kind);
    }

    float peak = 0.0f;
    for (size_t i = 0; i < L.size(); ++i)
        peak = std::fmax(peak, std::fmax(std::fabs(L[i]), std::fabs(R[i])));
    const float scale = peak > 0.9f ? 0.9f / peak : 1.0f;
    std::vector<short> pcm(L.size() * 2);
    for (size_t i = 0; i < L.size(); ++i) {
        pcm[i * 2 + 0] = (short)(L[i] * scale * 30000.0f);
        pcm[i * 2 + 1] = (short)(R[i] * scale * 30000.0f);
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    // The sweep plays on its OWN device. On the shared one it was queued
    // ahead of everything else for up to six seconds, so the aim cues and
    // pings that came during it played late — after the moment they were
    // about. Windows mixes the two devices.
    if (!g_sweep_wave && !OpenDeviceLocked(g_sweep_wave)) {
        if (!g_wave && !OpenLocked()) return;
        EmitLocked(pcm);                           // no second device: share
        return;
    }
    Buf& b = g_sweep_buf;
    if (b.prepared) {
        // A new sweep replaces one still playing.
        if (!(b.hdr.dwFlags & WHDR_DONE)) waveOutReset(g_sweep_wave);
        waveOutUnprepareHeader(g_sweep_wave, &b.hdr, sizeof(WAVEHDR));
        b.prepared = false;
    }
    b.pcm.swap(pcm);
    b.hdr = WAVEHDR{};
    b.hdr.lpData         = reinterpret_cast<LPSTR>(b.pcm.data());
    b.hdr.dwBufferLength = (DWORD)(b.pcm.size() * sizeof(short));
    if (waveOutPrepareHeader(g_sweep_wave, &b.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        return;
    b.prepared = true;
    waveOutWrite(g_sweep_wave, &b.hdr, sizeof(WAVEHDR));
}

} // namespace f3a::audio
