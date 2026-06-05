# Fallout 3 Access

An accessibility mod that makes **Fallout 3** playable for blind and visually impaired players.

It is a [FOSE](https://fose.silverlock.org) plugin that reads the game's interface aloud through your screen reader — NVDA, JAWS, Dolphin, System Access (via [Tolk](https://github.com/dkager/tolk)) — with Windows SAPI as a fallback when no screen reader is running.

## Features

**Working today**

- Menu reading with full keyboard navigation:
  - main menu, pause menu, save & load menus,
  - settings menus — volume sliders read their value ("Music, 25"; left/right reads just the new value), difficulty stepper and On/Off toggles read their current choice,
  - confirmation dialogs — the question is read first, then the focused Yes/No button,
- Pip-Boy reading (experimental),
- **Backspace** goes back one menu level (presses the game's own Back button),
- mouse hover reads the item under the cursor, keyboard focus is tracked independently,
- localized game text is handled code-page-aware (Polish cp1250 supported out of the box),
- repeat-last-phrase and stop-speech hotkeys,
- works with both retail editions of patch 1.7.0.3 (standard and no-gore) — detected automatically.

**Planned / in progress**

- World navigation: object scanner, turn-to-object, auto-walk,
- dialogue reading, barter, lockpicking feedback, VATS combat,
- player status readout (HP / AP / rads / caps),
- character creation flow.

## Requirements

1. **Fallout 3** or **Fallout 3 GOTY** — [Steam](https://store.steampowered.com/app/22300/Fallout_3/) / GOG, at game version **1.7.0.3**.
2. **FOSE v1.3 beta 2** — the Fallout Script Extender.
3. A screen reader (NVDA recommended) or any SAPI voice installed.

## Installation

1. **Install Fallout 3.**
2. **Downgrade the game to 1.7.0.3 (Steam only).** The current Steam build (1.7.0.4, "Anniversary" update) is not supported by FOSE. Download the **[Fallout Anniversary Patcher](https://www.nexusmods.com/fallout3/mods/24913)** from Nexus Mods (free account required), extract its contents into your Fallout 3 folder (where `Fallout3.exe` is) and run `Patcher.exe`. Wait until it reports success, then press Enter.
3. **Install FOSE.** Download *FOSE v1.3 beta 2* from <https://fose.silverlock.org>, open the archive and copy `fose_loader.exe` and all `fose_*.dll` files into the Fallout 3 folder, next to `Fallout3.exe`.
4. **Install this mod.** Download `fallout3-access.zip` from the [Releases page](../../releases) and extract it **into the Fallout 3 folder**. The archive already has the right layout:
   - `Tolk.dll`, `nvdaControllerClient32.dll`, `dolapi32.dll`, `SAAPI32.dll` → land next to `Fallout3.exe`,
   - `Data\FOSE\Plugins\fallout3_access.dll` and `Fallout3Access.ini` → the plugin itself.
5. **Start the game with `fose_loader.exe`** (not `Fallout3.exe`). With NVDA running you should immediately hear the mod's load announcement, and "Main menu opened" once the menu is up.

## Keyboard reference

The game is navigated with its own keys (arrows, Enter); the mod speaks what happens. All mod hotkeys below can be remapped in `Data\FOSE\Plugins\Fallout3Access.ini` (DirectInput scancodes).

### Menus

| Key | Action |
| --- | --- |
| Arrow Up / Down | move between items (the mod reads the focused item) |
| Arrow Left / Right | change a slider / stepper value (the mod reads the new value) |
| Enter | activate the focused item |
| **Backspace** | go back one menu level *(added by this mod)* |
| Mouse hover | reads the item under the cursor |

### Global (mod)

| Key | Action |
| --- | --- |
| `/` (slash) | repeat the last spoken phrase |
| Space | stop speech immediately |
| F12 | toggle the mod on / off |
| F11 | diagnostic: dump the current menu structure to a file (attach it to bug reports) |
| F10 | debug: run the console command from `[Debug] StartGameCommand` (e.g. start a game skipping the intro) |

### Pip-Boy (vanilla game keys, listed here because they matter)

| Key | Action |
| --- | --- |
| Tab | open / close the Pip-Boy |
| **F1 / F2 / F3** | switch main page: Stats / Items / Data |
| Arrow Left / Right | switch sub-tab on the current page |
| Arrow Up / Down | move through the list (or switch the CND/RAD/EFF view on the Status page) |

### In the game world

| Key | Action | Status |
| --- | --- | --- |
| L | where am I — location and facing direction | works |
| F | compass direction | works |
| X | read nearby objects | works |
| C | read nearby hostiles | works |
| H | player status (HP, AP, rads, caps) | work in progress |
| K | current quest target direction | work in progress |

(Defaults deliberately avoid keys Fallout 3 uses itself.)

### World navigation

Explore and travel without sight:

| Key | Action |
| --- | --- |
| `[` / `]` | cycle nearby objects (reads "name, distance, clock direction, i of N") |
| Shift + `[` / `]` | change scanner category: all / NPCs / items / doors / containers |
| `'` (apostrophe) | turn to face the selected object |
| `;` (semicolon) | **beacon guidance**: you walk (W), the mod plays a positional sonar ping at the target — panned left/right by direction, higher-pitched when it's ahead and lower when behind, faster as you close in. Turn until the ping is centred and high, then walk toward it. Distance is called out and you're warned if the target is on another floor. Press again to stop. |
| `\` (backslash) | **auto-walk**: the mod walks you to the target on open ground (gentle steering, stops on obstacles or a different floor). Press again to stop. |

Beacon guidance (`;`) is the recommended way to get around indoors; auto-walk (`\`) is handy on open terrain. All keys are remappable in the INI.

## Configuration

Edit `Data\FOSE\Plugins\Fallout3Access.ini`:

- `[General] Language` — `pl` or `en` (language of the mod's own messages),
- `[General] GameTextCodepage` — code page of the *game's* text; `0` = auto (`pl` → 1250, otherwise system ANSI), set `1250` / `1252` explicitly if names sound garbled,
- `[Modules]` — enable/disable feature modules,
- `[Voice]` — verbosity options (item weight/value, warnings),
- `[Hotkeys]` — every key listed above, as DirectInput scancodes,
- `[Debug] StartGameCommand` — console command bound to F10.

## Troubleshooting

- **No speech at all:** make sure NVDA is running *before* the game starts, and that `Tolk.dll` sits next to `Fallout3.exe`. Without a screen reader the mod falls back to SAPI.
- **Logs:** `Documents\My Games\Fallout3\FOSE\fallout3_access.log` (rolling log) and `fallout3_access_dump.txt` (written when you press F11).
- **Reporting a menu that doesn't read:** open that menu, press F11, and attach `fallout3_access_dump.txt` to your issue.

## Building from source

```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 ^
      -DFOSE_SDK_PATH=path/to/fose-source ^
      -DGAME_PATH="C:/path/to/Fallout 3 goty"
cmake --build build --config Release
```

- Requires VS 2022 Build Tools + Windows SDK. FOSE plugins are 32-bit (`-A Win32` is mandatory).
- The FOSE source tree is **not** bundled (licensing); download it from <https://fose.silverlock.org> and point `FOSE_SDK_PATH` at it (the folder containing `common/` and `fose/`).
- Tolk headers/binaries are bundled under `third_party/tolk/`.

## Credits & licenses

- Mod code — MIT (see `LICENSE`).
- [Tolk](https://github.com/dkager/tolk) by Davy Kager — LGPL; bundled binaries unmodified.
- `nvdaControllerClient32.dll` — NV Access, LGPL.
- [FOSE](https://fose.silverlock.org) by Ian Patterson, Stephen Abel and Paul Connelly — not bundled.
- Fallout 3 © Bethesda Softworks.
