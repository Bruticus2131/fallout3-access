#pragma once

#include <string>
#include <string_view>

namespace f3a::tolk {

// Logical priority of an utterance. Higher priority interrupts lower.
enum class Priority {
    Background = 0,   // ambient world scan, periodic
    Ui         = 10,  // menu selection changes
    Dialog     = 20,  // NPC dialog lines
    Combat     = 30,  // VATS, hostile spotted, low HP
    System     = 40,  // mod errors, configuration feedback
    Urgent     = 50   // critical: dying, fall damage
};

// Initialize Tolk (load Tolk.dll, attempt to auto-detect a screen reader).
// Returns true on success. If false, the bridge falls back to SAPI directly.
bool Init();
void Shutdown();

// Speak `text` (UTF-8). If `interrupt` is true, cancels currently spoken text
// of equal-or-lower priority.
void Speak(std::string_view text, Priority prio, bool interrupt = false);

// Cancel anything currently being spoken.
void Silence();

// Repeat the most recent utterance (bound to the RepeatLast hotkey). Speaks
// nothing if we haven't said anything yet.
void RepeatLast();

// True if a screen reader (NVDA/JAWS/etc.) is currently connected. False means
// we're either on SAPI fallback or have no audio output at all.
bool HasActiveScreenReader();

// Re-detect the active screen reader (useful if the user starts/stops NVDA
// while the game is running).
void Refresh();

// Name of the active screen reader for status messages ("NVDA", "JAWS", ...).
const char* ActiveReaderName();

} // namespace f3a::tolk
