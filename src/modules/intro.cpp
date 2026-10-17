#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/logger.h"

namespace f3a::modules::intro {

// The opening cinematic ("Wojna nigdy się nie zmienia") and the game's other
// films are described by the audio-description module now, timed to the
// film's own frame counter (game::CurrentMovie) from the [movie ...] entries
// in Fallout3Access\audiodesc_pl.txt — written frame by frame from the video
// file itself. The old track here guessed where the film started (when the
// loading screen closed with no player) and ran on a stopwatch; its
// descriptions were written without seeing the film and ended halfway.
// What stays is the F8 key: films' descriptions on/off.

void Start() {}
void Tick(float) {}

void Init()
{
    hotkeys::Bind(config::Get().hotkeys.intro_describe, &audiodesc::ToggleMovies);
    F3A_INFO("Intro: F8 switches the films' audio description.");
}

void Shutdown() {}

} // namespace f3a::modules::intro
