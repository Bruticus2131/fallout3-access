#pragma once

// Real-time positional "beacon" — a panned sonar ping that tells a blind
// player where a target is, in the spirit of SKU's pre-baked directional
// beacons but synthesized live (no bundled audio, no licensing entanglement).
//
//   azimuth  : relative bearing to target in degrees, -180..180,
//              0 = dead ahead, negative = left, positive = right.
//   distance01: 0 = right on top of it, 1 = at/over the far range.
//
// The ping is panned left/right by azimuth, pitched higher when the target is
// ahead and lower when behind (front/back disambiguation), and quieter with
// distance. Call Ping() on whatever cadence you like — the caller decides how
// often (closer targets usually ping faster).

#include <vector>

namespace f3a::audio {

bool Init();        // open the waveOut device; safe to call once
void Shutdown();    // stop and release
bool Available();

void Ping(float azimuth_deg, float distance01);

// A clean centred "on-target" blip at `freqHz` for `ms` ms — the line-of-sight
// aim cue (played when the crosshair is on an enemy). Distinct from Ping.
void Cue(int freqHz, int ms);

// One sonar sweep: a sound that travels once round the listener, clockwise
// from straight ahead, over `total_ms`. Each blip sounds as the sweep passes
// its bearing — panned to its side, quieter and lower when behind, louder when
// close — in a voice that says what it is. Quiet ticks mark 12, 3, 6 and 9
// o'clock so the sweep itself can be followed.
enum SweepKind { kSweepHostile = 0, kSweepPerson, kSweepContainer, kSweepDoor,
                 kSweepItem };
struct SweepBlip {
    float azimuth_deg;   // -180..180, 0 = ahead, + = right
    float distance01;    // 0 = right here .. 1 = edge of range
    int   kind;          // SweepKind
};
void Sweep(const std::vector<SweepBlip>& blips, int total_ms);

} // namespace f3a::audio
