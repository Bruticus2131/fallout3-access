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

namespace f3a::audio {

bool Init();        // open the waveOut device; safe to call once
void Shutdown();    // stop and release
bool Available();

void Ping(float azimuth_deg, float distance01);

} // namespace f3a::audio
