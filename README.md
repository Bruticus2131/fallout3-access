# Fallout 3 Access

An accessibility mod that makes **Fallout 3** playable for blind and visually impaired players.

It is a [FOSE](https://fose.silverlock.org) plugin that reads the game's interface aloud through your screen reader — NVDA, JAWS, Dolphin, System Access (via [Tolk](https://github.com/dkager/tolk)) — with Windows SAPI as a fallback when no screen reader is running.

## Features

**Working today**

- **Menu reading with full keyboard navigation** — main menu, pause, save & load,
  settings (sliders read their value, toggles and steppers read their choice),
  confirmation dialogs (question first, then the focused button).
- **Pip-Boy** — stats, items, data; quest list with objectives.
- **Dialogue, barter, containers and the message boxes** the game throws at you.
- **Inventory** — item stat cards on **D**, worn items announced as equipped,
  **Delete** drops the selected item, and the "how many?" stack prompt is read
  out with the game's own confirm/cancel keys.
- **World navigation** — object scanner, turn-to-object, audio beacon guidance
  and auto-walk that routes around walls using the game's navmesh, including
  through load doors into other cells.
- **World map** — browse every known location with distance and bearing, and
  fast-travel there through the game's own travel path.
- **Combat** — VATS is fully keyboard-driven (switch target, cycle body part,
  hear hit chance); outside VATS an audio cue tells you when an enemy is under
  the crosshair.
- **Terminals and the hacking minigame** — screen text is read as it types out,
  candidate passwords are listed and spelled.
- **Lockpicking** — an audio cue guides the pick to the sweet spot.
- **Player status** on **H**, location on **L**, compass on **J**.
- Localized game text handled code-page-aware (Polish cp1250 out of the box).
- Works with both retail editions of patch 1.7.0.3 (standard and no-gore),
  detected automatically.

**Rough edges / in progress**

- Character creation (the RaceSex menu) is not yet guided.
- Long-distance travel on foot is best combined with map fast travel; walking
  across the wasteland in one go is not reliable yet.
- Auto-walk can still get wedged on difficult terrain; it tells you when it
  gives up rather than walking into a wall silently.

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

| Key | Action |
| --- | --- |
| L | where am I — location and facing direction |
| J | compass direction |
| X | read nearby objects |
| C | read nearby hostiles |
| H | player status — health, action points, radiation |
| K | current quest target: name, direction and distance |
| F | the game's view-toggle key; the mod announces first or third person |
| F8 | opening-cinematic audio description (plays by itself; F8 stops it) |

(Defaults deliberately avoid keys Fallout 3 uses itself.)

### World navigation

Explore and travel without sight:

| Key | Action |
| --- | --- |
| Page Up / Page Down | cycle nearby objects (reads "name, distance, clock direction, i of N") |
| Ctrl + Page Up / Page Down | change scanner category: all / NPCs / items / doors / containers / quests |
| `'` (apostrophe) | turn to face the selected object |
| Home | centre the view on the selected object (or the nearest actor) |
| Shift + Home | keep the camera on a moving target until you press it again |
| `;` (semicolon) | **beacon guidance**: you walk, the mod plays a positional sonar ping at the target — panned by direction, higher-pitched ahead and lower behind, faster as you close in. Press again to stop. |
| `.` (period) | beacon guidance to the current quest marker |
| `\` (backslash) | **auto-walk**: the mod walks you there, following a navmesh path around walls and through load doors. Press again to stop. |
| End | **activate** the selected object without aiming — opens the door, reads the book, loots the container |
| Alt + Home | last resort: teleport to the selected object when it cannot be reached on foot |
| G | say what is under the crosshair |

Auto-walk steers the player directly rather than holding the movement keys, so
it controls speed precisely and slows into turns instead of skidding off ledges.
It plays the game's walk and run animations, so you hear real footsteps.
A quest marker is followed **live**: if the objective changes while you are on
your way, the walk retargets instead of finishing at a stale marker.

### World map (Pip-Boy → Data → World Map)

| Key | Action |
| --- | --- |
| Page Up / Page Down | browse locations (name, distance, bearing, whether discovered) |
| Enter | fast-travel there (the game's own travel, with its own rules) |
| Home | set it as the auto-walk destination and leave the map |
| End | filter: all / discovered / undiscovered |

### Inventory and stacks

| Key | Action |
| --- | --- |
| D | read the selected item's stat card (weight, value, damage, condition) |
| Delete | drop the selected item |
| Arrows, then **A** / **E** | in the "how many?" prompt: set the amount, then confirm / cancel (these are the game's own keys; the mod reads the number aloud) |

### Combat

| Key | Action |
| --- | --- |
| V.A.T.S. key | enter VATS; the mod reads target, body part and hit chance |
| A / D | in VATS: previous / next target |
| W / S / B | in VATS: cycle the targeted body part |
| `,` (comma) | aim the view at the selected or nearest target (free aim, no VATS) |
| `=` | skip an objective that cannot be completed blind (advances the quest stage — use deliberately) |

## Configuration

Edit `Data\FOSE\Plugins\Fallout3Access.ini`:

- `[General] Language` — `pl` or `en` (language of the mod's own messages),
- `[General] GameTextCodepage` — code page of the *game's* text; `0` = auto
  (`pl` → 1250, otherwise system ANSI). Set `1250` / `1252` explicitly if item
  names sound garbled,
- `[Modules]` — enable/disable feature modules,
- `[Hotkeys]` — every key listed above, as DirectInput scancodes,
- `[Debug] StartGameCommand` — console command bound to F10.

Useful `[Voice]` settings:

| Key | Meaning |
| --- | --- |
| `CrosshairNames` | say the NAME of what you are looking at, not just the verb |
| `CrosshairDistance` | add the distance to that announcement |
| `TargetCue`, `TargetCueHz` | audio cue when an enemy is under the crosshair, and its pitch |
| `NativeWalk` | auto-walk drives the player directly (`0` falls back to holding the movement keys) |
| `AutoWalkSpeed` | auto-walk speed in game units per second |
| `WalkAnimation` | play the game's walk/run animation while auto-walking (this is what gives you real footstep sounds) |
| `FootstepCue` | synthesized step clicks, used only when the animation is off |
| `NearbyScanRadius`, `NearbyScanMaxItems` | how far and how much the scanner reports |
| `CompassUnits` | `0` = clock face ("three o'clock"), `1` = degrees |

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
