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

// Speech through Prism (the namespace keeps its old name, from Tolk). Starts
// the speech thread and chooses the screen reader, else Windows' own voice.
// False when nothing could be started: the mod is then silent.
bool Init();
void Shutdown();

// Speak `text` (UTF-8). If `interrupt` is true, cancels currently spoken text
// of equal-or-lower priority.
void Speak(std::string_view text, Priority prio, bool interrupt = false);

// Cancel anything currently being spoken.
void Silence();
// Is the screen reader (probably) still saying something we sent? Asked of
// the reader when it can tell, else estimated from the length of the text.
bool IsSpeaking();

// Repeat the most recent utterance (bound to the RepeatLast hotkey). Speaks
// nothing if we haven't said anything yet.
void RepeatLast();

// True if a screen reader (NVDA/JAWS/etc.) is currently connected. False means
// Windows' own voice (OneCore, SAPI) is speaking, or nothing is.
bool HasActiveScreenReader();

// Choose the screen reader again. Prism also does it by itself when one
// starts or stops while the game runs.
void Refresh();

// Name of the active screen reader for status messages ("NVDA", "JAWS", ...).
const char* ActiveReaderName();

} // namespace f3a::tolk
