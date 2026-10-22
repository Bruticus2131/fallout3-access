# Fallout 3 Access

An accessibility mod that makes **Fallout 3** playable for blind and visually impaired players.

It is a [FOSE](https://fose.silverlock.org) plugin that reads the game's interface aloud through your screen reader — NVDA, JAWS, Dolphin, System Access (via [Tolk](https://github.com/dkager/tolk)) — with Windows SAPI as a fallback when no screen reader is running.

## Features

**Working today**

- **Menu reading with full keyboard navigation** — main menu, pause, save & load, character creation,
  settings (sliders read their value, toggles and steppers read their choice),
  confirmation dialogs (question first, then the focused button).
- **Pip-Boy** — every tab named; after an item's name its card is read as the
  screen shows it (damage or resistance, weight, value, condition in percent,
  ammo, effects); on the Stats page the description of each S.P.E.C.I.A.L.,
  skill and perk, the limbs' condition, radiation and active effects; the
  hotkey wheel; quests marked tracked / not tracked / completed, Enter tracks
  a quest and opens a note.
- **Dialogue** — who you talk to, topics with their place in the list and
  "already said", and **D** repeats the other side's last line.
- **Barter** — both columns (Left / Right), prices, items put up for the deal,
  how many caps the deal moves and which way, both sides' caps on **H**.
- **Containers, repair and sleep / wait** — container columns with category
  filters, the repair screens' before / after comparison and cost, the hours
  and the time when you wait.
- **Inventory** — worn items announced as equipped, **Delete** drops the
  selected item, and the "how many?" stack prompt is read out with the game's
  own confirm/cancel keys (the game's crash on that prompt is fixed).
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
  hear hit chance); holding **]** aims at the enemy and a tone says when a shot
  would hit; every hit and every kill is heard, for any enemy.
- **Terminals and the hacking minigame** — screen text is read as it types out,
  candidate passwords and bracket pairs are listed and spelled, Enter enters
  the chosen one, and after a wrong guess you hear which words still fit.
- **Lockpicking** — Left / Right move the bobby pin, an audio cue guides it to
  the sweet spot.
- **Player status** on **H** (health and AP with their maximum, radiation,
  crippled limbs, weapon and ammo, caps, carry weight, time), location on
  **L**, compass on **J**.
- **The console (~)** — every character you type or delete is spoken, the caret's
  moves too, a command brought back with Up / Down is read whole, what a command
  prints after Enter is read out, and PageUp / PageDown read the page of output
  that scrolls into view. The mod's own keys are off while the console is open
  (F11 still dumps it, with the console's state and last lines).
- Localized game text handled code-page-aware (Polish cp1250 and Russian cp1251
  detected from the game, a translation plugin included).
- The mod speaks Polish, English or Russian; any other language can be added
  with a text file (`Fallout3Access\lang_<code>.txt`, English → that language).
- Works with both retail editions of patch 1.7.0.3 (standard and no-gore),
  detected automatically.

**Rough edges / in progress**

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
5. **Start the game with `Fallout3.exe`** (not `fose_loader.exe`). With NVDA running you should immediately hear the mod's load announcement, and "Main menu opened" once the menu is up.

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
| H | player status — health and action points out of their maximum, radiation, crippled limbs, weapon with its condition and ammo, caps, carry weight, place and time (in barter: both sides' caps and the deal; while waiting: the hours) |
| Shift + H | your level, your experience out of what the next level takes, and how much is left |
| K | current quest target: name, direction and distance |
| F | the game's view-toggle key; the mod announces first or third person |
| F8 | opening-cinematic audio description (plays by itself; F8 stops it) |

(Defaults deliberately avoid keys Fallout 3 uses itself.)

### World navigation

Explore and travel without sight:

| Key | Action |
| --- | --- |
| Page Up / Page Down | cycle nearby objects (reads "name, distance, clock direction, i of N") |
| Shift + Page Up / Page Down | change scanner category: all / people / bodies / items / doors / containers / places / quest objects / quests (only those with something in them) |
| `'` (apostrophe) | turn to face the selected object |
| Home | centre the view on the selected object (or the nearest actor) and say how far it is; a target in another place: its marker on the door that leads there, with the real distance. During auto-walk Home only says where the walk is going and how far, without turning you |
| Shift + Home | keep the camera on a moving target until you press it again |
| `;` (semicolon) | **beacon guidance**: you walk, the mod plays a positional sonar ping at the target — panned by direction, higher-pitched ahead and lower behind, faster as you close in. Press again to stop. |
| `.` (period) | beacon guidance to the current quest marker |
| `\` (backslash) | **auto-walk**: the mod walks you there, following a navmesh path around walls and through load doors. Press again to stop; W / A / S / D stop it too. |
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
| D | read the selected item's stat card again (weight, value, damage, condition); on the Stats page the description; on a repair screen the comparison |
| Delete | drop the selected item |
| Arrows, then **A** / **E** | in the "how many?" prompt: set the amount, then confirm / cancel (these are the game's own keys; the mod reads the number aloud) |

### Containers and barter (open a container with End)

| Key | Action |
| --- | --- |
| Left / Right | switch between **your items** and the **container's** |
| Up / Down | move through the items of that column (in the container's column the mod moves the selection itself — the game's keys only ever reach yours) |
| Shift + Left / Right | previous / next item category in that column |
| Enter | the game's own take / put — stealing, karma and the "how many?" prompt behave as in the game |
| `[` | toggle between the two columns |

Barter uses the same keys: each row says its price, and the deal is announced
as it changes ("you pay N caps" / "you get N caps"). **H** says both sides'
caps.

### Dialogue

| Key | Action |
| --- | --- |
| Up / Down | topics, with "N of M" and "already said" for topics you have used |
| D | repeat the other side's last line |

### Terminals, hacking and lockpicking

| Key | Action |
| --- | --- |
| Up / Down | hacking: the candidate words (spelled), then the bracket pairs; a word ruled out by your earlier guesses is marked |
| Enter | hacking: enter the chosen word or bracket pair |
| Left / Right | hacking: read the screen; lockpicking: move the bobby pin |

### Console (~, the game's own key)

| Key | Action |
| --- | --- |
| typing, Backspace, Delete, Left / Right, Home / End | each character typed or deleted is spoken, and the character the caret moves onto |
| Up / Down | an earlier command, read whole |
| Enter | run it; the mod reads what it printed (or "Done." when it printed nothing) |
| PageUp / PageDown | scroll the output; the page that comes into view is read, "Top." / "Bottom." at the ends |

### Combat

| Key | Action |
| --- | --- |
| V.A.T.S. key | enter VATS; the mod reads target, body part and hit chance |
| A / D | in VATS: previous / next target |
| W / S | in VATS: next / previous body part, read at once with its hit chance |
| `]` | hold to aim (free aim): locks onto the nearest enemy fighting you and says who and how far; while held, a tone says the shot would hit |
| `,` (comma) | aim the view at the selected or nearest target (free aim, no VATS) |
| `=` | skip an objective that cannot be completed blind (advances the quest stage — use deliberately) |

## Configuration

Edit `Data\FOSE\Plugins\Fallout3Access.ini`:

- `[General] Language` — `auto` (default: the game's language, read from
  `Fallout3.esm` and checked again once the game has loaded its plugins —
  Polish for the Polish release, Russian for a Russian one, English for any
  other), or `pl` / `en` / `ru` to choose the language of the mod's own
  messages. Another code (`de`...) speaks from `Fallout3Access\lang_<code>.txt`
  — pairs of lines, `en: "English text"` then `de: "translation"`; whatever is
  not in the file is spoken in English and logged as "no translation for",
- `[General] GameTextCodepage` — code page of the *game's* text; `0` = auto
  (Polish game → 1250, Russian → 1251, otherwise system ANSI). Set `1250` /
  `1251` / `1252` explicitly if item names sound garbled,
- `[Modules]` — enable/disable feature modules,
- `[Hotkeys]` — every key listed above, as DirectInput scancodes,
- `[Debug] StartGameCommand` — console command bound to F10.

Useful `[Voice]` settings:

| Key | Meaning |
| --- | --- |
| `CrosshairNames` | say the NAME of what you are looking at, not just the verb |
| `CrosshairDistance` | add the distance to that announcement (off by default) |
| `HideEmptyContainers` | leave empty containers out of the scanner (`0` lists them, marked "pusty") |
| `TargetCue`, `TargetCueHz` | while aiming with `]`, a tone when the shot would hit the target, and its pitch |
| `ItemCard` | after an item's name, read its card (and a stat's description) as the screen shows it |
| `NativeWalk` | auto-walk drives the player directly (`0` falls back to holding the movement keys) |
| `AutoWalkSpeed` | auto-walk speed in game units per second |
| `WalkAnimation` | play the game's walk/run animation while auto-walking (this is what gives you real footstep sounds) |
| `FootstepCue` | synthesized step clicks, used only when the animation is off |
| `NearbyScanRadius`, `NearbyScanMaxItems` | how far and how much X (nearby objects) reads |
| `ScannerRadius` | how far the PgUp/PgDn scanner reaches outdoors (default 8000 units, about 125 m); indoors it covers the whole interior |
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
- Parts of the auto-walk design (engine pathfinding via the player's mover,
  turn-speed scaling and look-ahead braking, waypoint advance, the final-leg
  height guard, stopping the walk animation), the aiming recipes (native aim
  rotation, skeleton / collision aim points, raycast hit checks), several
  container and loot-menu techniques, the approach to the hacking minigame and
  to experience announcements, and the road-graph loader, generator and format
  are based on [FalloutNVAccess](https://github.com/FalloutAccess/FalloutNVAccess)
  by Alexander Epaneshnikov (MIT).
- [FOSE](https://fose.silverlock.org) by Ian Patterson, Stephen Abel and Paul Connelly — not bundled.
- Fallout 3 © Bethesda Softworks.
