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
  through load doors into other cells. Long walks follow an offline map of the
  whole navigation mesh (roads preferred), and a target in another place is
  reached through the right chain of doors, however far away.
- **DLCs** — Broken Steel, Point Lookout, The Pitt, Operation: Anchorage and
  Mothership Zeta: walk graphs and doors for their places, and auto-walk leads
  you to the Pitt handcart, the Duchess Gambit and the presidential metro.
- **Quest objects** — a scanner category with what a running quest waits for:
  the button, the terminal, the item, the spot to walk into, the free seat for
  the G.O.A.T. — worked out from the quests' own scripts.
- **Audio description** — the opening film in real time, and the scenes and
  people of the Vault 101 prologue, spoken in the gaps between voices.
- **English and Polish** — the mod speaks the game's language by default
  (Polish for the Polish release, English for any other), or the one you set.
- **World map** — browse every known location with distance and bearing, and
  fast-travel there through the game's own travel path.
- **Combat** — VATS is fully keyboard-driven (switch target, cycle body part,
  hear hit chance); outside VATS an audio cue tells you when an enemy is under
  the crosshair.
- **Terminals and the hacking minigame** — screen text is read as it types out,
  candidate passwords are listed and spelled.
- **Lockpicking** — an audio cue guides the pick to the sweet spot.
- **Player status** on **H**, location on **L**, compass on **J**.
- Localized game text handled code-page-aware (Polish cp1250 detected from the game).
- Works with both retail editions of patch 1.7.0.3 (standard and no-gore),
  detected automatically.

**Rough edges / in progress**

- Character creation (the RaceSex menu) is not yet guided.
- DLC support is built from the game data and tested offline; in-game reports
  are welcome.
- Auto-walk can still get wedged on difficult terrain; it tells you when it
  gives up rather than walking into a wall silently.

## Requirements

1. **Fallout 3** or **Fallout 3 GOTY** — [Steam](https://store.steampowered.com/app/22300/Fallout_3/) / GOG, at game version **1.7.0.3**.
2. **FOSE v1.3 beta 2** — the Fallout Script Extender.
3. **[Loot Menu Updated](https://www.nexusmods.com/fallout3/mods/27191)** — required.
   It replaces the container screen with a list drawn on the HUD, and the mod
   reads that list out: container name, the highlighted entry, its place in the
   list and whether it is equipped. Without it, looting is far harder to follow.
4. A screen reader (NVDA recommended) or any SAPI voice installed.

## Installation

1. **Install Fallout 3.**
2. **Downgrade the game to 1.7.0.3 (Steam only).** The current Steam build (1.7.0.4, "Anniversary" update) is not supported by FOSE. Download the **[Fallout Anniversary Patcher](https://www.nexusmods.com/fallout3/mods/24913)** from Nexus Mods (free account required), extract its contents into your Fallout 3 folder (where `Fallout3.exe` is) and run `Patcher.exe`. Wait until it reports success, then press Enter.
3. **Install FOSE.** Download *FOSE v1.3 beta 2* from <https://fose.silverlock.org>, open the archive and copy `fose_loader.exe` and all `fose_*.dll` files into the Fallout 3 folder, next to `Fallout3.exe`.
4. **Install this mod.** The easiest way is the installer, `fallout3-access-setup-<version>.exe` from the [Releases page](../../releases). To install by hand instead, download `fallout3-access-v<version>.zip` from the same page and extract it **into the Fallout 3 folder**. The archive already has the right layout:
   - `Tolk.dll`, `nvdaControllerClient32.dll`, `dolapi32.dll`, `SAAPI32.dll` → land next to `Fallout3.exe`,
   - `Data\FOSE\Plugins\fallout3_access.dll` and `Fallout3Access.ini` → the plugin itself,
   - `Data\FOSE\Plugins\Fallout3Access\` → the walk graphs and the door table (`*.dat`, ready made for Fallout 3 and all five DLCs) and the audio descriptions. **Copy all of it**: without this folder, long walks and walks to other places stay off. Nothing has to be generated and no Python is needed.
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
| Shift + Page Up / Page Down | change scanner category: all / NPCs / items / doors / containers / quests |
| `'` (apostrophe) | turn to face the selected object |
| Home | centre the view on the selected object (or the nearest actor) |
| Shift + Home | keep the camera on a moving target until you press it again |
| `;` (semicolon) | **beacon guidance**: you walk, the mod plays a positional sonar ping at the target — panned by direction, higher-pitched ahead and lower behind, faster as you close in. Press again to stop. |
| `.` (period) | beacon guidance to the current quest marker |
| `\` (backslash) | **auto-walk**: the mod walks you there, following a navmesh path around walls and through load doors. Press again to stop. |
| End | **activate** the selected object without aiming — opens the door, reads the book, loots the container |
| Alt + Home | last resort: teleport to the selected object when it cannot be reached on foot |
| G | say what is under the crosshair |
| N | **Pip-Boy sonar**: one sweep of sound travels round you, clockwise from straight ahead, and everything nearby answers from its own direction — an enemy growls, a container with loot rings, a door knocks, a person hums, an item ticks; louder when close, quieter and lower behind you. Then a short count of what was heard. |

The scanner leaves **empty containers** out (`HideEmptyContainers`), so every
container it lists has something in it. Auto-walk opens ordinary closed doors on
its route by itself, and stops at a locked one to tell you which lock it is.

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

### Containers (open one with End)

| Key | Action |
| --- | --- |
| Left / Right | switch between **your items** and the **container's** |
| Up / Down | move through the items of that column (in the container's column the mod moves the selection itself — the game's keys only ever reach yours) |
| Shift + Left / Right | previous / next item category in that column |
| Enter | the game's own take / put — stealing, karma and the "how many?" prompt behave as in the game |
| `[` | toggle between the two columns |

### Combat

| Key | Action |
| --- | --- |
| V.A.T.S. key | enter VATS; the mod reads target, body part and hit chance |
| A / D | in VATS: previous / next target |
| W / S | in VATS: next / previous body part, read at once with its hit chance |
| `]` | hold to aim (free aim): locks onto the nearest enemy fighting you and says who and how far |
| `,` (comma) | aim the view at the selected or nearest target (free aim, no VATS) |
| `=` | skip an objective that cannot be completed blind (advances the quest stage — use deliberately) |

## Configuration

Edit `Data\FOSE\Plugins\Fallout3Access.ini`:

- `[General] Language` — `auto` (default: the game's language, read from
  `Fallout3.esm` — Polish for the Polish release, English for any other), or
  `pl` / `en` to choose the language of the mod's own messages,
- `[General] GameTextCodepage` — code page of the *game's* text; `0` = auto
  (Polish game → 1250, otherwise system ANSI). Set `1250` / `1252` explicitly
  if item names sound garbled,
- `[Modules]` — enable/disable feature modules,
- `[Hotkeys]` — every key listed above, as DirectInput scancodes,
- `[Debug] StartGameCommand` — console command bound to F10.

Useful `[Voice]` settings:

| Key | Meaning |
| --- | --- |
| `CrosshairNames` | say the NAME of what you are looking at, not just the verb |
| `CrosshairDistance` | add the distance to that announcement (off by default) |
| `HideEmptyContainers` | leave empty containers out of the scanner (`0` lists them, marked "pusty") |
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
- The walk graphs and the door table (`data/roadnet/*.dat`) are generated from
  the game's own data files and are not kept in git (35 MB); they ship ready
  made in the release archive and the installer, so **players never need
  this**. Only to rebuild them from source (Python 3 with numpy and scipy):

  ```
  python tools/generate_walk_graph.py --esm "<FO3>/Data/Fallout3.esm" --all-worldspaces --output-dir data/roadnet
  python tools/generate_walk_graph.py --esm "<FO3>/Data/Fallout3.esm" --worldspace Wasteland --worldspace CitadelWorld ^
         --worldspace DCworld01 --worldspace DCworld08 --worldspace DCworld09 --worldspace DCworld12 ^
         --overlay "<FO3>/Data/Anchorage.esm" --overlay "<FO3>/Data/ThePitt.esm" --overlay "<FO3>/Data/BrokenSteel.esm" ^
         --overlay "<FO3>/Data/PointLookout.esm" --overlay "<FO3>/Data/Zeta.esm" --suffix +DLC --output-dir data/roadnet
  python tools/generate_walk_graph.py --esm "<FO3>/Data/ThePitt.esm" --master "<FO3>/Data/Fallout3.esm" --all-worldspaces --output-dir data/roadnet
  (likewise for Anchorage.esm, BrokenSteel.esm, PointLookout.esm)
  python tools/generate_world_doors.py --data "<FO3>/Data"
  python tools/generate_quest_objects.py --data "<FO3>/Data"
  python tools/generate_item_values.py --data "<FO3>/Data"
  ```
  (`TestQAWorld.dat`, a developer test worldspace, is not shipped.)

## Credits & licenses

- Mod code — MIT (see `LICENSE`).
- [Tolk](https://github.com/dkager/tolk) by Davy Kager — LGPL; bundled binaries unmodified.
- `nvdaControllerClient32.dll` — NV Access, LGPL.
- The road-graph loader, nearest-node grid and road rules are ported from
  [FalloutNVAccess](https://git.alex19ep.me/alex19EP/FalloutNVAccess) by Alexander Epaneshnikov — MIT.
- [FOSE](https://fose.silverlock.org) by Ian Patterson, Stephen Abel and Paul Connelly — not bundled.
- Fallout 3 © Bethesda Softworks.
