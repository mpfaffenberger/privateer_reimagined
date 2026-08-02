# Privateer Reimagined

**Privateer Reimagined** is a from-scratch C++20 reimagining of **Wing
Commander: Privateer**. It rebuilds the Gemini Sector as a modern standalone
space sim while preserving the original game's open-ended mix of trading,
combat, contracts, ship upgrades, exploration, and story-driven adventure.

Explore all 69 systems, make your fortune, follow the complete campaign, and
generally discover why space insurance premiums are obscene.

![The New Detroit bar — someone here wants a word](docs/screenshots/new_detroit_bar.png)
*The New Detroit bar. Ernesto Sandoval wants a word. It's probably fine.*

This is an unofficial standalone implementation, with no game engine, ECS
framework, or runtime dependency manager. Rendering, window/input, audio, and
debug UI are built on small vendored libraries under `third_party/`.

## Current state

The playable sandbox includes:

- 69 systems, 170 jump links, and 59 landable bases
- 18 ship classes, 9 gun types, missiles, torpedoes, turrets, shields, armor,
  ECM, repair systems, tractor beams, and ship upgrades
- six-axis flight, afterburner, autopilot, autodocking, jumping, nav maps,
  targeting, and combat HUDs
- data-driven combat AI, faction standings, dynamic encounters, hailing, and
  voiced comms
- commodity trading, cargo, salvage, ship sales, equipment dealers, guilds,
  and generated missions
- versioned saves and accumulating autosaves
- the complete 23-mission Privateer campaign, including fixers, escorts,
  scripted encounters, Steltek systems, and the drone finale
- cinematics, an in-game cinematic studio, music, SFX, speech, and extensive
  development/debug tooling
- a persistent Confed calendar beginning at stardate `2669.135`

For a detailed feature audit, see [`docs/GAP_ANALYSIS.md`](docs/GAP_ANALYSIS.md).
The current living-world work is documented in
[`docs/persistent_world_plan.md`](docs/persistent_world_plan.md) and its
[frozen interface contracts](docs/persistent_world_contracts.md).

## Build

### macOS

Requirements: CMake 3.20+, a C++20 compiler, and a Metal-capable Mac.

```bash
git lfs pull
cmake -S . -B build
cmake --build build -j
./build/new_privateer
```

The macOS build uses Metal and links the required Apple frameworks directly.

### Windows

Windows uses D3D11. The GitHub Actions workflow builds a portable static-CRT
executable and publishes a rolling `nightly` zip from `main`.

For a local Visual Studio developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target new_privateer
.\build\new_privateer.exe
```

`third_party/bin/sokol-shdc.exe` must be present; CI downloads it automatically.

### Linux

CMake has an OpenGL/X11 backend. It is less exercised than macOS and Windows,
so consider it a porting target rather than a polished supported release.

## Command-line options

Useful development overrides include:

```bash
./build/new_privateer --system troy
./build/new_privateer --ship centurion
./build/new_privateer --skip-title
./build/new_privateer --play-cinematic demo_flyby
```

Additional `--dev-*`, capture, load, and system-soak switches live near
`sokol_main()` in `src/main.cpp`; they are developer interfaces rather than a
stable player-facing CLI.

## Core controls

### Flight & combat

| Input | Action |
|---|---|
| Mouse | Fly-by-wire aiming (when engaged) |
| `Space` | Toggle fly-by-wire ⇄ free cursor |
| `+` / `-` | Increase/decrease throttle |
| Hold `Tab` | Afterburner |
| `,` / `.` | Roll left/right |
| Hold left mouse or `Ctrl` | Fire guns |
| `G` | Cycle gun arm-mode (unarmed → each gun type → all) |
| `Enter` | Fire missile |
| `F` | Compatibility alias: cycle loaded ordnance |
| `T` | Cycle targets (nearest → farthest, ≤15 km) |
| `Z` | Tractor loose loot into the hold |
| `I` | In-flight inventory |
| `P` | Pause |

### Navigation

| Input | Action |
|---|---|
| `N` | Open local nav map / cycle nav targets |
| `M` | Sector nav map (whole galaxy: systems + jump links) |
| `A` | Autopilot to the selected nav point |
| `D` | Dock at the selected base |
| `J` | Jump through the selected jump gate |

### Status MFD & comms

| Input | Action |
|---|---|
| `C` | Comms screen (then `1`–`9` pick replies) |
| `R` | Damage report screen |
| `W` | Cycle loaded launcher ordnance and open ARMAMENTS |
| `Esc` | Back/close; double-tap in flight to quit |

Additional context-sensitive bindings are shown by the in-game HUD. Debug and
content-authoring panels intentionally have their own development bindings —
`Ctrl+M` toggles the ImGui debug panel, and `]` / `[` scale/reset sim time for
watching AI brawls in fast-forward.

## Tests

The CMake harnesses are opt-in so a normal build remains a game-only build.
Build and run a focused test like this:

```bash
cmake --build build --target test_world_clock
./build/test_world_clock

cmake --build build --target test_docking
./build/test_docking

cmake --build build --target test_savegame
./build/test_savegame
```

Other wired targets include:

- `test_app_cli`
- `test_ai_brain`
- `test_missions`
- `test_mission_tracker`
- `test_fixers`
- `test_campaign`
- `test_cinematic`
- `test_full_loop` — broad sandbox integration harness; currently undergoing
  modernization as older assumptions are replaced by accumulating autosaves
  and newer content behavior

More focused standalone harnesses live under `tools/test_*.cpp`.

## Architecture

```text
src/
  main.cpp                 application host and orchestration
  camera/autopilot/...     flight, navigation, docking, jumping
  ship/ai_brain/...        ships, combat, weapons, and AI
  economy/outfitting/...   trading, inventory, repairs, and equipment
  missions/campaign/...    generated missions and scripted story
  cinematic*               runtime, parser, triggers, and studio
  savegame/player/plot     persistent player and campaign state
  mesh/sprite/postprocess  rendering and visual effects
shaders/                   GLSL cross-compiled by sokol-shdc
assets/
  systems/ ships/ bases/   world and entity definitions
  data/                    economy, encounters, dialogue, and equipment
  cinematics/ speech/ ...  authored media
third_party/               vendored Sokol, ImGui, stb, and HandmadeMath
tools/                     extraction, generation, authoring, and test tools
docs/                      architecture notes, reverse engineering, and plans
re/                        recovered data and reverse-engineering material
```

### Runtime stack

| Layer | Technology |
|---|---|
| Language | C++20 |
| Build | CMake 3.20+ |
| Window/input | `sokol_app` |
| Graphics | `sokol_gfx` — Metal, D3D11, or OpenGL |
| Audio | Sokol audio plus project mixers/content |
| Debug/content UI | Dear ImGui |
| Images | `stb_image` |
| Math | HandmadeMath |
| Shaders | GLSL → `sokol-shdc` generated platform code |

The code favors plain modules and explicit state over framework machinery.
Game/content boundaries are mostly JSON-driven, while headless compile guards
keep logic testable without a GPU or audio device.

## Development priorities

Near-term work is tracked in the docs rather than a pretend-static checkbox
list. The major active areas are:

1. **Gemini Lives:** world calendar, scheduled named NPCs, recurring encounters,
   richer character/voice content, and persistent living-world events.
2. **Combat fidelity:** component damage, useful Damage/Weapons MFDs,
   Friend-or-Foe missiles, scanner tiers, and remaining turret UX.
3. **Engineering:** split cohesive responsibilities out of `src/main.cpp`, keep
   headless harnesses current, and continue improving cross-platform builds.
4. **Later:** Righteous Fire content and campaign.

## Asset and tooling notes

Many tools under `tools/` recover or transform data from an original Privateer
installation. Generated/intermediate material is intentionally separate from
runtime data where practical. Do not assume every script is needed to play the
game; most are authoring or reverse-engineering utilities.

The project uses Git LFS for large media. If assets appear to be tiny pointer
files, run `git lfs pull` before blaming the renderer. The renderer has enough
to answer for already.

## License

MIT. See [`LICENSE`](LICENSE).
