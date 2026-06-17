# Privateer AI patch-and-relaunch playground — **Plan B** (no debugger)

A reliable loop for experimenting on the Privateer combat AI **without** the
crash-prone DOSBox-X heavy debugger. Instead of poking RAM live (which kept
hard-crashing with *Corrupt MCB chain* / segment-limit violations the moment
you halted the dynamic core), we **binary-patch** the pirate AI `CNST` fields
in the game data, mount a **writable copy of the CD**, and **relaunch** to watch
the behaviour.

> **The loop:** `patch_cnst.py --f0 N` → `launch.sh` → fly out, get jumped,
> watch the pirate + read the cockpit **`Range:`** → `patch_cnst.py --reset`.
> Fast core, no debugger, no crashes.

> **Scope / legality.** Everything here runs on your **own** legal GOG copy.
> The patch **script** (`re/dosbox/patch_cnst.py`) and this doc are committable —
> pure code + derived facts. The patched data (`re/dosbox/cd/`, the
> `PRIV.TRE.master`, the DOSBox conf/launcher) is **gitignored under `re/`** and
> never committed. The CNST values quoted here are already documented in
> `docs/ai_model.md` §2.2.

---

## 0. What the patcher targets

Every NPC pilot's combat brain is a 16-byte `CNST` vector (8 × LE u16) baked
into `PRIV.TRE` (`docs/ai_model.md` §2.2):

```
f0@+0  f1@+2  f2@+4  f3@+6  f4@+8  f5@+10  f6@+12  f7@+14
```

`PRIV.TRE` lives **inside `GAME.GOG`** (the `C:\game` folder has no `PRIV.TRE`).
We extracted `GAME.GOG` (ISO 9660, **volume label `GAME`**) into the writable
folder `re/dosbox/cd/`, kept a pristine `re/dosbox/PRIV.TRE.master`, and mount
`re/dosbox/cd/` as the **D: CD-ROM** (labelled `GAME` so the CD check passes).

The faction `CNST` signatures the patcher knows (16 LE bytes each):

| Faction | `CNST` (f0..f7) | signature | blocks in PRIV.TRE |
|---------|-----------------|-----------|--------------------|
| `pirate`  | `600,1500,45,75,2,2,76,0` | `58 02 DC 05 2D 00 4B 00 02 00 02 00 4C 00 00 00` | **7** |
| `militia` | `600,1500,45,40,2,2,76,0` | `58 02 DC 05 2D 00 28 00 02 00 02 00 4C 00 00 00` | **4** |
| `default` | `600,1500,45,60,2,2,76,0` | `58 02 DC 05 2D 00 3C 00 02 00 02 00 4C 00 00 00` | **1** |

The patcher always reads the **pristine master**, overwrites **only the fields
you name** (everything else keeps its canonical value → zero drift), and writes
the result to `re/dosbox/cd/PRIV.TRE`. Re-running with new values is always
clean — no accumulated drift.

---

## 1. One-time setup (already done for you)

If the writable CD or master ever go missing, rebuild them from your GOG copy:

```sh
# locate GAME.GOG (the ®/™ in the .app path break naive shell quoting)
G=$(find /Applications -name GAME.GOG)

# GAME.GOG is a raw ISO 9660 image; mount a copy and extract it
cp "$G" /tmp/game.iso
hdiutil attach /tmp/game.iso -readonly -nobrowse        # -> /Volumes/GAME
mkdir -p re/dosbox/cd
cp -a /Volumes/GAME/. re/dosbox/cd/
cp re/dosbox/cd/PRIV.TRE re/dosbox/PRIV.TRE.master      # pristine master
hdiutil detach /Volumes/GAME
rm -f /tmp/game.iso
```

(`hdiutil` only recognises the image when it has a `.iso` extension — hence the
`cp` to `/tmp/game.iso`.) Confirm both stay gitignored:

```sh
git check-ignore re/dosbox/cd/PRIV.TRE re/dosbox/PRIV.TRE.master   # both print = ignored
```

---

## 2. The workflow

**(a) Patch** the pirate's break-off radius way up:

```sh
python3 re/dosbox/patch_cnst.py --faction pirate --f0 5000
```

The patcher reports: faction, signature, **blocks patched** (7 for pirate),
the **byte offsets**, and each field's *canonical → patched* value. Example:

```
blocks patched: 7
offsets       : 0x6fb687, 0x6fbc3d, 0x6fc1d7, 0x6fc76f, 0x6fcd09, 0x6fd285, 0x6fd813
  f0 break-off?         600 ->   5000  <== changed
  ...
```

**(b) Launch** (cd's into the app Resources so C: resolves, then runs dosbox-x
with the patched D: CD; fast `core=auto/cputype=auto/cycles=max`, no debugger):

```sh
bash re/dosbox/launch.sh
```

At the `C:\>` prompt (banner: *"Privateer Plan B shell ready"*), start the game:

```
PRIV.EXE
```

**(c) Observe.** Play to the cockpit, launch into space, open the Nav map, pick
a nav a hop away and engage **autopilot** — it drops you out of cruise at
encounters. In pirate-heavy space (e.g. around Troy at game start) you'll be
jumped by `PIR_*` pilots. **Target the attacker** so the cockpit shows its
**`Range:`** readout — that number is the *same* `magnitude>>8` world-unit
distance the AI uses (`docs/ai_model.md` §7.10), so you can read break-off /
commit / flee distances straight off the HUD. Note what the patched value did.

**(d) Reset** to clean data when done (or between experiments):

```sh
python3 re/dosbox/patch_cnst.py --reset
```

---

## 3. Hypothesis-testing experiments

Each experiment: patch → `launch.sh` → fly out → get jumped → read the
**`Range:`** at which the behaviour changes → `--reset`. Run the two extremes
back-to-back and compare. (Hypotheses from `docs/ai_model.md` §7.9–7.11 and
`docs/ai_debug_playground.md` §0.)

### Exp 1 — `f0` = break-off / disengage trigger?

```sh
python3 re/dosbox/patch_cnst.py --faction pirate --f0 100      # hugs you?
# ...launch, observe break-off Range, reset...
python3 re/dosbox/patch_cnst.py --faction pirate --f0 5000     # peels off early?
```
*Predict:* small `f0` → bores in almost to contact; large `f0` → extends /
disengages from much farther out.

### Exp 2 — `f1` = attack / combat-commit range (not detection)?

```sh
python3 re/dosbox/patch_cnst.py --faction pirate --f1 200      # commits only point-blank?
# ...launch, observe, reset...
python3 re/dosbox/patch_cnst.py --faction pirate --f1 8000     # starts its run from way out?
```
*Predict:* `f1` is the combat-commit range. Detection/contact is the separate
15000 sensor sphere — if `f1` were detection, changing it wouldn't move the
*commit* distance once you're already a known contact.

### Exp 3 — `f3` = maneuver-jitter / aggression scaling (static-trace confirmed)

```sh
python3 re/dosbox/patch_cnst.py --faction pirate --f3 10       # tame, predictable jinks?
# ...launch, observe how wild the pirouettes are, reset...
python3 re/dosbox/patch_cnst.py --faction pirate --f3 200      # erratic / highly evasive?
```
*Predict:* `f3` scales the RNG perturbation mixed into the per-tick maneuver
update (`docs/ai_model.md` §7.13). Low `f3` → tame, predictable steering;
high `f3` → noisy/evasive jinking. This is **not** a pursuit-tenacity or
break-off dial — live `Range:` observations for "when does he disengage" are
expected to be *inconclusive*.

### Exp 4 — `f6` = flee caution (pending static trace)

```sh
python3 re/dosbox/patch_cnst.py --faction pirate --f6 0        # never flees, fights to death?
# ...launch, observe, reset...
python3 re/dosbox/patch_cnst.py --faction pirate --f6 128      # bolts early at low health?
```
Still the cleanest *hypothesis* (tiered caution values strongly suggest a
morale/flee role), but the static trace did **not** find a direct resident-image
consumer for `f6`, and a full carve + scan of the VROOMM overlay pool
(`re/dosbox/overlays/pool_7ff60_e4f20.bin`) also found no code consumer: 415 raw
u16 occurrences of the four known `f6` byte values, zero plausible CNST blocks,
zero immediate-mode references after filtering far-call addresses, and zero
indirect `LES BX,[SI+2]` -> `[BX+0x0C]` reads.

**Status:** `f6` remains **pending static confirmation**. Run this experiment to
look for a behavioural signal in the cockpit `Range:` readout, but do not treat a
null result as disproof.  See `docs/ai_model.md` §7.13 for the full scan results
and the dynamic-next-step rationale.

> **Advanced `f6` path — VROOMM overlay trace.** If the static scan is
> inconclusive, the next step is to trace which overlays are loaded at runtime
> during merchant flee/comms/docking scenarios.  See `docs/ai_model.md`
> §7.13.1–7.13.4 for the overlay manager location, the load/unload logger patch,
> and the planned pirate-vs-merchant comparison scenario.  This is a deeper RE
> exercise and requires manual in-cockpit observation.

Jot down, per run: field, value, and the observed break-off / commit / jitter
**`Range:`** / behaviour note. That's exactly the data needed to confirm or
relabel fields in `docs/ai_model.md`.

> **Targeting the right attacker.** Most early-game muggers are `PIR_*` (patch
> `--faction pirate`, 7 blocks). If a `MIL_*` militia or a generic pilot is the
> one shooting you, patch that faction instead (`--faction militia` = 4 blocks,
> `--faction default` = 1 block). The patcher errors loudly if a signature
> matches 0 blocks (wrong faction / non-pristine master).

---

## 4. How it was validated (honest status)

- **PRIV.TRE confirmed inside GAME.GOG** (89,486,108 bytes); ISO volume label
  `GAME`. Signature counts in `PRIV.TRE`: pirate **7**, militia **4**, default **1**.
- **Patch validated:** `--faction pirate --f0 5000` patched **7** blocks; a
  byte-diff vs the master showed exactly **14** changed bytes (2 bytes × 7
  blocks), `58 02` (600) → `88 13` (5000) at `0x6fb687` and the other six
  offsets. No-drift verified: a follow-up `--f1`-only patch restored `f0` to
  600. `--reset` restores the master byte-for-byte (checksum identical).
- **CD mount + boot:** the folder-as-CD-ROM mount works — D: reports volume
  label `GAME` with `PRIV.TRE` present. `PRIV.EXE` launched the `PRCD.EXE`
  engine off the patched CD and reached the **640×480 intro/menu with music
  streaming** (we ran it headless, so we confirmed the boot via logs — video
  mode set, 80386 paging on = VROOMM engine live, MIDI sysex streaming — not by
  seeing the cockpit). The **in-cockpit AI observation is yours to drive** on a
  real display.
  - *Fallback (not needed):* if Privateer's CD check ever rejects the folder
    mount, rebuild a patched ISO and `imgmount` it instead:
    ```sh
    hdiutil makehybrid -iso -joliet -default-volume-name GAME \
      -o re/dosbox/game_patched.iso re/dosbox/cd
    # then in the conf: imgmount D "<abs>/game_patched.iso" -t iso -fs iso
    ```
    The folder-cdrom mount booted fine here, so this stays a documented backup.

---

## 5. Reference

- Patch tool: `re/dosbox/patch_cnst.py` (committable; `--help` for usage).
- Conf: `re/dosbox/privateer-debug.conf` (gitignored) — `core=auto`, D: =
  `re/dosbox/cd` as `-t cdrom -label GAME`.
- Launcher: `re/dosbox/launch.sh` (gitignored).
- Live-debugger alternative (when you *do* want RAM pokes): see
  `docs/ai_debug_playground.md`.
- CNST field semantics + hypotheses: `docs/ai_model.md` §2.2, §7.9–7.13.
