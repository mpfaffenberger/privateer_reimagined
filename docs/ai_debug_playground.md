# Privateer AI live-debug playground (DOSBox-X)

> **Prefer Plan B for routine AI tuning.** This live-debugger route is powerful
> but crash-prone on macOS (*Corrupt MCB chain* when you halt the dynamic core).
> For a reliable, debugger-free loop — binary-patch the CNST fields, mount a
> writable patched CD, and relaunch — see **`docs/ai_patch_playground.md`**
> (Plan B). Use this live-debugger doc only when you specifically need live RAM
> pokes / breakpoints.

A copy-pasteable recipe for **live-experimenting on the Privateer combat AI**:
launch vanilla Privateer in the DOSBox-X heavy debugger, get into a 1-v-1 with a
pirate, make yourself effectively invulnerable, then **poke an NPC's CNST skill
fields (`f0/f1/f3/f6`) in memory and watch the AI behaviour change**.

> **Scope / legality.** This file is *instructions + derived facts only* (the
> CNST field values like `600`/`1500` are already documented in
> `docs/ai_model.md`). The DOSBox-X config, symlink to your game, memory dumps
> and the binary itself are **gitignored under `re/`** — never committed. You
> run the game from your **own** legal GOG install.

> **What we verified for you vs. what you drive.** We confirmed: DOSBox-X heavy
> debugger installs and its console attaches from a Terminal; the config below
> mounts your game and boots the engine (`PRIV.EXE` -> `PRCD.EXE`) to the
> intro/menu (640x480 video + music streaming). We could **not** see the screen,
> so the in-flight steps and the live memory capture are *yours to drive* — the
> addresses, signatures and commands below are all derived from the `PRCD.EXE`
> disasm (`docs/ai_model.md` §7.x).

---

## 0. The working hypotheses you're testing

From `docs/ai_model.md` §7.9–7.11 (disasm-confirmed mechanics, **behavioural
labels still to be pinned empirically** — that's the point of this playground):

| Field | Block off | Default (pirate) | Old label | **Hypothesis to test** |
|-------|-----------|------------------|-----------|------------------------|
| `f0`  | `+0x00`   | `600`  (`0x0258`) | engage radius | **DISENGAGE / break-off trigger** — too close => extend/peel off |
| `f1`  | `+0x02`   | `1500` (`0x05DC`) | detection range | **ATTACK / combat-commit range** (NOT detection; radar cull is the separate 15000 sensor sphere) |
| `f3`  | `+0x06`   | `75`   (`0x004B`) | maneuver-jitter / aggression scaling | RNG perturbation scale in steering update — high=erratic/evasive, low=tame |
| `f6`  | `+0x0c`   | `76`   (`0x004C`) | morale / flee-caution (pending) | flee threshold hypothesis (merchant 128 = bolts early; Kilrathi 64 = fights to death) — no direct EXE consumer found yet |

The three same-scale range tiers (all `magnitude>>8` world units): **sensor
15000 ⊃ detect/`f1` 1500 ⊃ engage/`f0` 600**.

---

## 1. Install (one-time)

```sh
brew install --cask dosbox-x
# The cask app fails Apple's Gatekeeper (it's deprecated upstream); clear the
# quarantine flag so it will launch:
xattr -dr com.apple.quarantine /Applications/dosbox-x.app
```

The cask's binary `/Applications/dosbox-x.app/Contents/MacOS/dosbox-x` is the
**heavy-debugger build** (confirmed: it exports `BP`, `BPM`, `MEMFIND`, `SM`,
`FM`, `DOS MCBS`, …).

The config + a symlink to your game live in `re/dosbox/` (gitignored):

- `re/dosbox/privateer-debug.conf` — DOSBox-X config (core=normal so breakpoints
  fire; mounts your game read/write so saves work).
- `re/dosbox/gamedir` — symlink to
  `…/Wing Commander ® Privateer ™.app/Contents/Resources/game`.

If the symlink is missing, recreate it:

```sh
ln -sfn "/Applications/Wing Commander ® Privateer ™.app/Contents/Resources/game" \
        /Users/mpfaffenberger/code/new_privateer/re/dosbox/gamedir
```

---

## 2. Launch (always from a Terminal)

The macOS debugger console **only works when you start dosbox-x from a
terminal** (the binary says so itself). Use the helper script -- it `cd`s into
the game's `Resources` folder so the conf's RELATIVE mounts (`game` /
`game.gog`) resolve exactly like GOG's own launcher (an absolute path through
the `(R) (TM)` characters silently fails to mount):

```sh
bash /Users/mpfaffenberger/code/new_privateer/re/dosbox/launch.sh
```

A DOSBox-X window appears at the `C:\>` prompt with a "Privateer RE shell ready"
banner (if you DON'T see that banner / you're on `Z:\>`, the mount failed --
shout). CPU is set to GOG's known-good `core=auto / cputype=auto / cycles=max`
(full speed, no INT 6 storm). Start the game:

```
PRIV.EXE
```

`PRIV.EXE` is a tiny launcher that loads the real `PRCD.EXE` engine (the thing
we reverse-engineered). Play through to a new/loaded game in the cockpit.

---

## 3. Open the debugger console

Press the **debugger hotkey** — default mapper event `debugger` is bound to
**Alt+Pause**. Mac keyboards have no Pause key, so rebind once:

1. Open the Mapper Editor (DOSBox-X menu **Main ▸ Mapper Editor**, or host-key
   **Ctrl-F1**).
2. Find the **`debugger`** event, click it, press a free key (e.g. **Alt-D**),
   click **Save**, then **Exit**.

From then on, your rebind drops you into the debugger TUI **in the Terminal
window** you launched from. The emulation freezes while you're in the debugger.
The config sets `debuggerrun = debugger` so it halts (doesn't auto-run) on entry.

### Debugger command cheat-sheet (all values in hex)

| Cmd | Meaning |
|-----|---------|
| `BP seg:off` | code breakpoint |
| `BPM seg:off` | **memory-change** breakpoint (great on data) |
| `BPLM linear` | linear-address memory breakpoint |
| `BPINT n [ah] [al]` | interrupt breakpoint |
| `BPDEL n` / `BPDEL *` | delete one / all breakpoints |
| `D seg:off` | dump/display memory |
| `DV linear` / `DP linear` | data view at linear / physical addr |
| `SM seg:off v0 v1 …` | **set memory** (poke bytes) |
| `FM seg:off` | **freeze** the value at an address (invuln!) |
| `MEMFIND seg:off …` | start a memory-search instance |
| `MEMS op value` | narrow the search (`=`, `<`, `>`, …) — cheat-engine style |
| `MEMDUMP seg:off len` | dump region to `MEMDUMP.TXT` |
| `SR reg value` | set a CPU register |
| `IV seg:off name` | name an address (label `f0`, `f1`, …) |
| `DOS MCBS` | list DOS memory blocks (find PRCD.EXE's load segment) |
| run keys | **F5** run, **F10** step-over, **F11** step-into, **F12**/run-watch |

Type a command name with no/garbage args to make the debugger print its own
usage line (handy for `MEMFIND`'s exact arg order on this build).

---

## 4. Static <-> runtime address map (PRCD.EXE)

MZ header math (`PRCD.EXE`): header = `0xAE00` (44544 B); entry `CS:IP =
0000:0000` (so at entry **CS == the load segment**); resident image =
file `0xAE00 … 0x7FF20`; the overlay pool is everything past `0x7FF20`.

**File-offset -> program-relative address:**

```
program_linear = file_offset - 0xAE00
seg:off        = (program_linear >> 4) : (program_linear & 0xF)
```

**Program-relative -> runtime:** `actual_seg = LoadSeg + program_seg`, i.e.
`linear_runtime = LoadSeg*16 + program_linear`, where `LoadSeg` is where DOS
loaded `PRCD.EXE` (read it from `DOS MCBS`, or it's `CS` at engine entry).

Resident AI consumers (fixed runtime segment — these are the reliable BP
targets; the **CNST loader at file `0xb37f1` is in a VROOMM overlay that
relocates**, so do NOT breakpoint it by static address):

| What | file off | program seg:off | notes |
|------|----------|-----------------|-------|
| AI **detect** func entry (`f1`) | `0x1ce8c` | `0x1208:000C` | compares `dist>>8` vs `f1` at `0x1cf2d`; stores `dist>>8` to `DS:0x65F0` at `0x1cf09`; `mov bx,[bx+0x17]` (=> CNST) at `0x1cf1a` |
| AI **engage** func entry (`f0`) | `0x1d1f8` | `0x123F:0008` | compares vs `(f0+R1+R2)<<8` at `0x1d2f3` |
| sensor **cull** compare (15000) | `0x3d5b2` | `0x327B:0002` | `cmp dword [bp-8],0x3a98` |
| magnitude helper | `0x57646` | `0x4C5B:0x0296` | alpha-max-beta-gamma 3-D |

**DGROUP (`DS`) globals — fixed at runtime, your anchor for everything:**

| `DS` offset | Contents |
|-------------|----------|
| `0x0E50` | `f0` fallback default = `600` (`58 02`) |
| `0x0E52` | `f1` fallback default = `1500` (`DC 05`) |
| `0x65F0` | AI "distance to target" scratch (rewritten every detect tick) |
| `0x66B4` | near-pointer to the **player ship** object |

**Live pointer chain to an NPC's CNST block** (all DGROUP-near pointers):

```
ship + 0x02  -> pilotdef (AIDS record)        (gated by ship+0x0B bit0)
pilotdef + 0x17 -> CNST 16-byte block (near-ptr, lives in the DS near-heap)
CNST + 0x00 = f0   +0x02 = f1   +0x06 = f3   +0x0C = f6
```

---

## 5. Find your data segment (do this first, every session)

Get into the cockpit (in space is fine), then press your debugger hotkey. In the
register panel note **`DS`**. Confirm it's the real DGROUP by dumping the f0/f1
defaults:

```
D DS:0E50
```

You should see bytes `58 02 DC 05` at `DS:0E50` — that's `600` then `1500`,
the documented fallback vector. If you see those, `DS` is correct; **use that
`DS` value in every command below.** (Optionally label them:
`IV DS:0E50 f0def` / `IV DS:0E52 f1def`.)

To find `LoadSeg` (only needed for the code-BP route in §7B): run `DOS MCBS`,
find the block owned by `PRCD`/`PRIV`; its segment + `0x10` is `LoadSeg`.

---

## 6. Get into a pirate fight

1. From a base, launch into space.
2. Open the Nav map, pick a nav point a hop away, engage the **autopilot**
   (nav computer). Autopilot auto-drops you out of cruise at encounters.
3. In pirate-heavy systems (e.g. around Troy at game start) you'll be jumped by
   `PIR_*` pilots. Target the attacker (so the cockpit shows its Range readout).

Tip: the cockpit **`Range:`** number is the *same* `magnitude>>8` world-unit
distance the AI uses (§7.10) — so you can read break-off / attack distances
straight off the HUD while you poke fields.

---

## 7. Locate an NPC's live CNST block

### 7A. Fast route — memory search by signature (no addresses needed)

A spawned **pirate** (`PIR_*`) has CNST = `600,1500,45,75,2,2,76,0`, which is
this exact 16-byte little-endian signature:

```
58 02 DC 05 2D 00 4B 00 02 00 02 00 4C 00 00 00
```

Start a search over the data segment and let it match:

```
MEMFIND DS:0000 58 02 DC 05 2D 00 4B 00 02 00 02 00 4C 00 00 00
```

(Type `MEMFIND` first to see this build's exact arg order/width flag; then feed
the bytes.) The debugger reports the matching `seg:off` — that's the **live
CNST block**. If several pirates are present you'll get several matches; any one
works. Other handy signatures:

| Pilot class | CNST | signature (LE bytes) |
|-------------|------|----------------------|
| Pirate `PIR_*` | 600,1500,45,**75**,2,2,**76** | `58 02 DC 05 2D 00 4B 00 02 00 02 00 4C 00 00 00` |
| Militia `MIL_*` | 600,1500,45,**40**,2,2,76 | `58 02 DC 05 2D 00 28 00 02 00 02 00 4C 00 00 00` |
| Bounty/ace `BOU_*`/GARVICK | 600,1500,45,75,**3,3**,**102** | `58 02 DC 05 2D 00 4B 00 03 00 03 00 66 00 00 00` |
| Confed escort `CON_AA` | 600,**2100**,45,**60**,2,2,76 | `58 02 34 08 2D 00 3C 00 02 00 02 00 4C 00 00 00` |

### 7B. Confirm-the-chain route — breakpoint the AI (optional, elegant)

This both *proves* the pointer chain and lands you on the exact live block:

```
BPM DS:65F0          ; halt when the AI rewrites its distance scratch
```

Press **F5** to run. During an encounter the **detect** function rewrites
`DS:0x65F0` every tick, so it halts inside that function. You're now near file
`0x1cf09`. Single-step (**F11**) down to the instruction `mov bx,[bx+0x17]`
(visible in the disasm pane). Just **before** it, `BX` = the pilotdef pointer;
**step over it** and `BX` = the **CNST block near-pointer**. Then:

```
D DS:BX              ; should show 58 02 DC 05 …  (the same signature)
```

`BPDEL *` to clear breakpoints when done. (Reminder: the CNST *loader* at
`0xb37f1` is overlay-relocated — use this resident detect func, not the loader.)

---

## 8. Become invulnerable (freeze your health)

DOSBox-X can **freeze** a memory cell (`FM`). Find your shield/armor cell with a
cheat-engine-style comparative search, then freeze it:

1. Note your current shield/armor value on the HUD. Open the debugger.
2. Start a wide search for that value, e.g. searching words:
   `MEMFIND DS:0000 <value>` (use the printed usage for width).
3. **F5**, take a hit so the value drops, debugger again, narrow:
   `MEMS < <previousValue>` (or `MEMS = <newReadout>`).
4. Repeat steps 2–3 until one or two addresses remain — that's your health cell.
5. Top it to max and **freeze** it:
   ```
   SM <seg>:<off> FF 7F        ; set to a large value (adjust width)
   FM <seg>:<off>              ; freeze it so hits never reduce it
   ```

Now you can loiter at any range and study AI behaviour indefinitely. (If `FM`
behaves oddly with your build, just re-`SM` to max between runs — the AI
experiments are short.)

---

## 9. Poke the CNST fields and watch the AI

Let `B` = the CNST block `seg:off` from §7 (e.g. `DS:1A3C`). Fields are
little-endian words; addresses: `f0 = B+0`, `f1 = B+2`, `f3 = B+6`, `f6 = B+C`.

Read the current block any time:

```
D <B>          ; 16 bytes: f0 f1 f2 f3 f4 f5 f6 f7
```

Poke (remember LE byte order). Decimal -> hex word -> bytes:

| Value | hex | SM bytes |
|-------|-----|----------|
| 100   | 0064 | `64 00` |
| 600   | 0258 | `58 02` |
| 1500  | 05DC | `DC 05` |
| 2000  | 07D0 | `D0 07` |
| 5000  | 1388 | `88 13` |
| 8000  | 1F40 | `40 1F` |

Examples (replace `<B>` with your block address):

```
SM <B>+0 D0 07     ; f0 := 2000  (engage/break-off radius up)
SM <B>+0 64 00     ; f0 := 100   (engage/break-off radius down)
SM <B>+2 88 13     ; f1 := 5000  (attack/commit range way up)
SM <B>+6 00 00     ; f3 := 0     (kill aggression)
SM <B>+C 80 00     ; f6 := 128   (max caution -> should flee early)
SM <B>+C 00 00     ; f6 := 0     (zero caution -> should never flee)
```

Press **F5** to resume and watch the pirate. Re-enter the debugger to change
values again — the engine reads the block live every tick, so changes take
effect immediately.

---

## 10. Suggested experiments (tie results back to the hypotheses)

Read distances off the cockpit **`Range:`** HUD (= AI world units).

1. **`f0` = break-off vs engage?**
   - Set `f0 = 100` (`SM <B>+0 64 00`): does the pirate bore in much closer / stop
     peeling off until almost touching? Set `f0 = 2000` (`D0 07`): does it
     break off / extend from much farther out?
   - *Hypothesis (new):* small `f0` => hugs you; large `f0` => disengages early.
     *Old label* said `f0` is the engage radius — watch which story the
     break-off distance actually tells.

2. **`f1` = attack/commit range vs detection?**
   - Set `f1 = 5000` (`SM <B>+2 88 13`): does it start its attack run / commit
     from much farther than the usual ~1500? Set `f1 = 200` (`C8 00`): does it
     only commit when right on top of you?
   - *Hypothesis (new):* `f1` is the combat-commit range (detection/contact is
     the separate 15000 sensor sphere). If detection were `f1`, dropping it
     wouldn't change *commit* distance once you're already a known contact.

3. **`f3` = aggression/pursuit.**
   - `f3 = 0` vs `f3 = 200` (`C8 00`): does pursuit tenacity / how hard it
     presses change? Compare to a stock pirate (75) and an ace (also 75).

4. **`f6` = flee caution (cleanest dial).**
   - `f6 = 0`: pirate should fight to the death even at low health.
     `f6 = 128` (`80 00`): should bolt/flee early. This is the field we're most
     confident about — use it as your control to confirm your block address and
     that pokes are taking effect.

Jot down, per poke: the field, the value, and the observed break-off /
commit / flee distance from the HUD. That's exactly the data we need to confirm
or relabel `f0`/`f1` in `docs/ai_model.md`.

---

## 11. Reset / cleanup

- `BPDEL *` clears breakpoints; `F5` resumes.
- Pokes are RAM-only — they vanish when you quit DOSBox-X; nothing on disk
  changes.
- The whole `re/dosbox/` area (config, symlink, logs, dumps) is gitignored.
