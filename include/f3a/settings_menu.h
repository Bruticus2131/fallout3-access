#pragma once

// The mod's own category in the game's Settings menu ("Dostępność" /
// "Accessibility", beside Gameplay, Display, Audio and Controls), with its
// options as the game's own sliders and on/off rows. A change takes effect at
// once and is written to the mod's INI. Implementation and the reverse
// engineering behind it: settings_menu.cpp.

#include <string>

namespace f3a::settings_menu {

// Patch the game's Settings menu builder. Checked byte for byte first; on
// another build nothing is touched and false is returned.
bool Install();

// A slider of the category as it should be said: `caption` is the row's text
// as read (UTF-8), `position` the slider's step (its user0) -> "880 Hz",
// "15 stopni". False for any other row: the plain number is said then.
bool SliderText(const std::string& caption, int position, std::string* out);
// F2 on one of the mod's key rows (main thread): the next key pressed becomes
// its key. False when the selected row is not one of them.
bool StartKeyCapture();
// Backspace (main thread): from the keys screen back to the Accessibility
// options. False anywhere else - the game's own Back then.
bool BackFromKeys();
// Every poll tick: waits for that key.
void Tick();

} // namespace f3a::settings_menu
