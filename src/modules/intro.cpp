#include "f3a/modules.h"
#include "f3a/hotkeys.h"
#include "f3a/config.h"
#include "f3a/game_access.h"
#include "f3a/tolk_bridge.h"
#include "f3a/logger.h"

#include <windows.h>

namespace f3a::modules::intro {
namespace {

// Real-time audio description of Fallout 3's opening cinematic ("War. War never
// changes."). The intro is a FIXED sequence, so we play a pre-authored, time-
// stamped track of visual descriptions via Tolk. Per the user's choice it runs
// IN PARALLEL with the game's narration (a blind player follows the TTS over the
// game audio, like a screen reader talking over everything else).
//
// AUTO-START: polling_loop calls Start() when the Loading screen closes while no
// player exists yet — that's the new-game cinematic (a loaded save spawns the
// player at once, so the auto-stop below cancels it). Timeline t=0 is that
// moment, so the early cues cover the pre-monologue fade-in. F8 toggles it off
// (or starts it manually for testing).
struct Cue { uint32_t at_ms; const char* text; };

const Cue kCues[] = {
    {   1500, "Audiodeskrypcja intra." },
    {   4000, "Z czerni powoli wyłania się obraz, jak strojona stara taśma filmowa." },
    {   8000, "Zniszczone, opuszczone miasto w odcieniach szarości i rdzy. Za chwilę rozlegnie się narracja." },
    {  14000, "Kamera sunie przez ruiny: zawalone budynki, hałdy gruzu, powyginane wraki samochodów." },
    {  24000, "Wszystko pokrywa popiół i kurz. Niebo jest ciężkie, zachmurzone, bez śladu słońca." },
    {  35000, "W oddali sylwetka zrujnowanej stolicy: uszkodzony obelisk Pomnika Waszyngtona i pęknięta kopuła Kapitolu." },
    {  47000, "Pordzewiały szkolny autobus, połamane latarnie, czarne okna wypalonych domów." },
    {  59000, "Stare radio trzeszczy na półce — to z niego płynie opowieść o świecie sprzed atomowej zagłady." },
    {  72000, "Kamera unosi się nad pustkowiem, mijając zawaloną autostradę i porzucone pojazdy." },
    {  86000, "Na tle ruin staje samotna postać: żołnierz Bractwa Stali w ciężkim pancerzu wspomaganym, z karabinem w dłoniach." },
    {  99000, "Kamera okrąża go i zbliża do hełmu. W przyłbicy odbija się martwe pustkowie." },
    { 110000, "Obraz wsuwa się w szczelinę hełmu, aż wszystko zalewa biel." },
    { 118000, "Przejście do Krypty 101 — miejsca twoich narodzin. Koniec opisu intra." },
};
constexpr int kCueCount = (int)(sizeof(kCues) / sizeof(kCues[0]));

bool  g_active = false;
bool  g_auto   = false;   // started automatically (vs F8 manual test)
DWORD g_start  = 0;
int   g_idx    = 0;

void Begin(bool automatic)
{
    g_active = true;
    g_auto   = automatic;
    g_start  = GetTickCount();
    g_idx    = 0;
    F3A_INFO("Intro audio-description started (auto=%d).", (int)automatic);
}

void Toggle()
{
    if (g_active) {
        g_active = false;
        tolk::Speak("Audiodeskrypcja wyłączona.", tolk::Priority::System, true);
        return;
    }
    Begin(/*automatic=*/false);   // manual test — won't auto-stop in gameplay
}

} // namespace

// Called by polling_loop the moment the Loading screen closes with no player —
// the new-game cinematic. No-ops if disabled in the INI or already running.
void Start()
{
    if (g_active || !config::Get().intro_audio_desc) return;
    Begin(/*automatic=*/true);
}

void Tick(float)
{
    if (!g_active) return;
    // Auto-started run ends as soon as the player exists: that's either the birth
    // scene (cinematic over) or proof this was really a save-load, not the intro.
    if (g_auto && game::IsPlayerValid()) { g_active = false; return; }

    DWORD elapsed = GetTickCount() - g_start;
    // Fire every cue whose time has come (catch up if a frame was long). Queue
    // (no interrupt) so each description finishes instead of cutting the prior.
    while (g_idx < kCueCount && elapsed >= kCues[g_idx].at_ms) {
        tolk::Speak(kCues[g_idx].text, tolk::Priority::Ui, /*interrupt=*/false);
        ++g_idx;
    }
    if (g_idx >= kCueCount) g_active = false;
}

void Init()
{
    hotkeys::Bind(config::Get().hotkeys.intro_describe, &Toggle);
    F3A_INFO("Intro audio-description module ready.");
}
void Shutdown() {}

} // namespace f3a::modules::intro
