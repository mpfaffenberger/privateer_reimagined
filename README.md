# Privateer Reimagined

**Privateer Reimagined** is a from-scratch C++20 reimagining of **Wing
Commander: Privateer**. It rebuilds the Gemini Sector as a modern standalone
space sim and keeps the original's open-ended mix of trading, combat,
contracts, ship upgrades, exploration, and story.

Explore all 69 systems, make your fortune, play through the full campaign, and
find out why space insurance premiums are so high.

![The New Detroit bar — someone here wants a word](docs/screenshots/new_detroit_bar.png)
*The New Detroit bar. Ernesto Sandoval wants a word. It's probably fine.*

This is an unofficial standalone implementation. It uses no game engine, ECS
framework, or runtime dependency manager. Rendering, window/input, audio, and
debug UI are built on small vendored libraries under `third_party/`.

## Current state

The playable sandbox and campaign include:

- **The Gemini Sector:** 69 systems across four quadrants, 170 jump links, 59
  landable bases plus the Steltek derelict, and a classic-style four-quadrant
  sector star chart that hides unrevealed systems.
- **Ships and gear:** 18 ship definitions (4 player hulls, plus NPC,
  Kilrathi, and Steltek craft), 9 gun types, DF/HS/IR missiles and
  torpedoes, turrets that auto-fire, shields, armor, ECM, repair droids, a
  tractor beam, and a jump drive. Ships render from multi-view sprite atlases
  baked from 3D meshes.
- **Flight and combat:** fly-by-wire flight with a critically damped turn
  response, afterburner, autopilot, autodock, jumps, nav and sector maps,
  targeting with ITTS lead, and an interactive ARMAMENTS schematic.
- **Cockpits:** painted cockpit overlays with live instruments in the MFD
  holes for the Tarsus, Galaxy, and Centurion. The Orion still uses the
  HUD-only view.
- **World:** data-driven combat AI decoded from the original's skill
  vectors, 9 factions with player reputation, dynamic and scripted
  encounters, hails, contraband scans, and voiced comms.
- **Economy:** a commodity exchange, cargo and salvage, contraband (sold
  only at pirate bases), a ship dealer with trade-in, and a visual
  hardpoint equipment bay.
- **Missions:** shared contract boards (Mission Computer, Mercenaries'
  Guild, Merchants' Guild) offering 6 mission types.
- **Campaign:** the full 23-mission Privateer campaign (Sandoval through
  the Terrell/drone finale) with bar fixers, portrait conversations,
  escorts, a secret compartment, plot-gated Steltek systems, and in-flight
  cinematics.
- **Persistence:** versioned saves (format v8) with accumulating autosaves,
  and a Confed calendar that starts at stardate `2669.135` and advances one
  day per landing.
- **Audio and tooling:** dynamic music, per-scene bar music, SFX, speech,
  an in-game cinematic studio, and a lot of development tooling.

Not built yet, in short: per-system component damage, Friend-or-Foe
missiles, scanner tiers, buying turrets for hulls that don't have them,
rumors from the bartender, NPC schedules for the living world, and
Righteous Fire. The full, evidence-backed list is in
[`docs/GAP_ANALYSIS.md`](docs/GAP_ANALYSIS.md). Living-world work is covered
in [`docs/persistent_world_plan.md`](docs/persistent_world_plan.md) and its
[frozen interface contracts](docs/persistent_world_contracts.md).

## Build

All platforms need Git LFS: most runtime media and mesh sources are stored
in LFS.

Mission briefing text comes from the checked-in
`src/mission_templates.gen.h`. If you own the original game, you can recover
`re/mission_text.json` with `re/extract_mission_text.py` and re-run CMake.
The header is then regenerated from your data. Neither step is needed to
build or play.

### macOS (primary platform)

Requirements: CMake 3.20+, a C++20 compiler, and a Metal-capable Mac.

```bash
git lfs pull
cmake -S . -B build
cmake --build build -j
./build/new_privateer
```

The macOS build uses Metal and links the required Apple frameworks directly.
It is also the only platform with the `dev_remote` HTTP API. Other platforms
compile a stub.

### Windows

Windows uses D3D11 and MSVC. The simplest path is the helper script. It
enters the VS developer shell, downloads `sokol-shdc.exe` if needed, and
builds a Release binary with Ninja:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_and_run.ps1
.\build\new_privateer.exe
```

To build by hand from a VS developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target new_privateer
```

`third_party/bin/sokol-shdc.exe` must be present. There is **no CI**: the
GitHub Actions Windows workflow was removed in July 2026. Windows builds are
published by hand as preview pre-releases on the GitHub Releases page.
Configure with `-DNP_WINDOWS_CONSOLE=ON` to get a debug console.

### Linux

CMake has an OpenGL (`SOKOL_GLCORE`) + X11 backend. It gets much less
testing than macOS and Windows, so treat it as a porting target rather than
a supported release.

## Command-line options

The CLI is parsed in `src/app_cli.cpp`. These are developer interfaces, not
a stable player-facing CLI.

| Flag | Effect |
|---|---|
| `--system <id>` | Start in a specific system (for example `troy`) |
| `--ship <class>` | Start in a specific hull (for example `centurion`) |
| `--skip-title` | Skip the title screen |
| `--windowed` | Force windowed mode |
| `--continue` | Resume the newest save (autosaves accumulate as timestamped files) |
| `--load <slot>` | Load a legacy numbered save slot |
| `--play-cinematic <id>` (+ `--cine-at <s>`) | Play a cinematic, optionally seeking to a time |
| `--goto <system>` (+ `--goto-at`) | Dev teleport to a system after a delay |
| `--dev-invuln`, `--dev-missions`, `--dev-land`, `--dev-kill-at`, `--dev-jump-drive` | Dev conveniences |
| `--dev-jump-soak`, `--goto-soak`, `--*-interval` | Long-running jump/system soak tests |
| `--capture-clean` | Hide HUD/cockpit overlay for clean atlas screenshots |

## Core controls

### Flight & combat

| Input | Action |
|---|---|
| Mouse | Fly-by-wire aiming (when engaged) |
| `Space` | Toggle fly-by-wire ⇄ free cursor |
| `+` / `-` | Increase/decrease throttle |
| Hold `Tab` | Afterburner (also takes back control from autopilot) |
| `,` / `.` | Roll left/right |
| Hold left mouse or `Ctrl` | Fire guns |
| `G` | Cycle gun arm-mode (unarmed → each gun type → all) |
| `W` | Cycle the loaded launcher and open ARMAMENTS |
| `F` | Compatibility alias for `W` |
| `Enter` | Fire missile |
| `T` | Cycle targets, nearest to farthest (≤15 km) |
| `Z` | Tractor loose loot into the hold (2.5 km) |
| `I` | In-flight inventory |
| `P` | Pause/unpause (shows centered instructions) |

### Navigation

| Input | Action |
|---|---|
| `N` | Open the local nav map / cycle nav targets |
| `M` | Sector star chart (inside the `N` map it opens as a side pane) |
| `A` | Autopilot to the selected nav point |
| `D` | Dock at the selected base |
| `J` | Jump through the selected jump gate |

### Status MFD & comms

| Input | Action |
|---|---|
| `C` | Comms screen (then `1`–`9` pick replies) |
| `R` | Damage page (hull/armor only; per-system damage isn't modeled yet) |
| `Esc` | Back/close; double-tap in flight to quit |

The in-game HUD shows more context-sensitive bindings.

### Developer hotkeys

The debug and authoring panels have their own bindings:

| Input | Tool |
|---|---|
| `Ctrl+M` | ImGui debug panel |
| `Ctrl+K` | Cinematic Studio |
| `Ctrl+B` | Bar-music DJ |
| `]` / `[` | Scale / reset sim time (watch AI brawls in fast-forward) |
| `F1` | Base Art Studio (landed rooms) |
| `F2` | Sprite light editor |
| `F3` | Ship-sprite frame HUD |
| `F4` | Atlas grid viewer |
| `F5` | Mesh orientation editor |
| `F6` | Sprite generation tool |
| `F7` / `F8` / `F9` | Sound / music / speech labelers |
| `F10` | Nav map auditor |

On macOS, a loopback-only `dev_remote` HTTP server (`127.0.0.1:47001`) lets
scripts and agents drive the game. Endpoints are documented in
`src/dev_remote.h`.

## Tests

Test harnesses are opt-in CMake targets (`EXCLUDE_FROM_ALL`), so a normal
build only builds the game. Build and run one from the repo root, because
harnesses load assets relative to the working directory:

```bash
cmake --build build --target test_campaign
./build/test_campaign
```

To list every wired harness:

```bash
cmake --build build --target help | grep test_
```

For current pass/fail status and known-broken harnesses, see the test
scoreboard in [`docs/GAP_ANALYSIS.md`](docs/GAP_ANALYSIS.md#p0--build-and-test-health).

> **Warning ([#383]):** `test_savegame`, `test_missions`, and
> `test_full_loop` currently write into your **real** save directory (slots
> 7/9 plus an autosave). Back up your saves before running them.

## Architecture

```text
src/                       ~60k lines of C++, flat module layout
  main.cpp                 application host, input, sim loop (~7.7k lines; being split)
  app_cli / game_state     CLI parsing and top-level state
  camera/autopilot/jump/docking/turn_response    flight and navigation
  ship*/ai_brain/firing/gun/missile/perception   ships, combat, weapons, AI
  cockpit_hud/cockpit_mfd/cockpit_overlay/...    HUD, MFDs, painted cockpits
  economy/commodity*/outfitting/equipment_*      trading and ship fitting
  missions*/campaign/fixers/plot/escort/drone    contracts and story
  scripted_encounters/encounters/hailing/comm*   the living sector
  cinematic*                                     runtime, triggers, studio
  savegame/player/world_clock                    persistence and calendar
  mesh*/sprite*/skybox*/sun*/postprocess*        rendering
  *_labeler/*_editor/*_studio/dev_remote/...     dev and authoring tools
shaders/                   GLSL cross-compiled by sokol-shdc
assets/
  galaxy.json systems/ bases/ ships/   world and entity definitions
  data/                    economy, encounters, fixers, characters, dialogue
  cockpits/ concourse/ meshes/ sprites/  art
  cinematics/ speech/ music/ sfx/      authored media
third_party/               vendored Sokol, ImGui, stb, and HandmadeMath
tools/                     ~150 extraction, generation, authoring, and test tools
docs/                      architecture notes, reverse engineering, and plans
re/                        recovered data and reverse-engineering material
scripts/                   platform build helpers
```

### Runtime stack

| Layer | Technology |
|---|---|
| Language | C++20 |
| Build | CMake 3.20+ |
| Window/input | `sokol_app` |
| Graphics | `sokol_gfx`: Metal (macOS), D3D11 (Windows), OpenGL (Linux) |
| Audio | Sokol audio plus project mixers/content |
| Debug/content UI | Dear ImGui |
| Images | `stb_image` |
| Math | HandmadeMath |
| Shaders | GLSL → `sokol-shdc` generated platform code |

The code uses plain modules and explicit state instead of framework
machinery. Game content is mostly driven by JSON. Headless compile guards
(`*_HEADLESS`) let logic be tested without a GPU or audio device.

## Development priorities

Work is tracked in [GitHub issues][issues]. The ranked gap list, test
scoreboard, and suggested attack order live in one place:
[`docs/GAP_ANALYSIS.md`](docs/GAP_ANALYSIS.md). Broadly, the active areas are
build/test health, combat fidelity (component damage, FF missiles, scanner
tiers), presentation (HDR, cockpits), the Gemini Lives living world, and
later Righteous Fire.

When you change key bindings, CLI flags, the save format, or test targets,
update this README and the gap analysis in the same pull request.

## Asset and tooling notes

Many tools under `tools/` recover or transform data from an original
Privateer installation. Generated and intermediate material is kept apart
from runtime data where practical. You don't need most scripts to play the
game; they're authoring or reverse-engineering utilities.

The project uses Git LFS for large media and mesh sources. If assets show up
as tiny pointer files, run `git lfs pull` before blaming the renderer. The
renderer has enough to answer for already.

## License

MIT. See [`LICENSE`](LICENSE).

[issues]: https://github.com/mpfaffenberger/privateer_reimagined/issues

[#383]: https://github.com/mpfaffenberger/privateer_reimagined/issues/383
