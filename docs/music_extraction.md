# Privateer music extraction (np-m96 AdLib OPL · np-gln General MIDI / MT-32)

Privateer shipped its soundtrack for **three** sound devices. All three use the
SAME **XMIDI** sequences in the SAME Origin multi-container layout — only the
patch/controller data and the companion timbre bank differ:

| Device              | Sequence file | Timbre bank  | Driver       | Character            |
|---------------------|---------------|--------------|--------------|----------------------|
| AdLib / OPL2 FM     | `*.ADL`       | `TIMBRES.AD` | `ADLIB.DRV`  | "8-bit", bleepy FM   |
| General MIDI        | `*.GEN`       | (stock GM)   | `SB.DRV` etc | rich, real samples   |
| Roland MT-32        | `*.GEN`       | `TIMBRES.MT` | `ROLAND.DRV` | richest / most authentic |

We render the **General MIDI** path by default now (np-gln) — it sounds like a
real "16-bit" soundtrack (strings, choir, tubular bells, guitar) instead of the
bleepy OPL FM. The AdLib path (np-m96) is kept as a fallback. **No audio is
committed** — see the legal note at the bottom.

## File formats (what's in `DATA/SOUND/`)

| File            | What it is                                                        |
|-----------------|-------------------------------------------------------------------|
| `*.ADL`         | AdLib/OPL2 music — Origin container around **XMIDI** sub-songs     |
| `*.GEN`         | the **same** container/XMIDI, but with **General MIDI** program/controller data (our HQ source) |
| `TIMBRES.AD`    | Miles/AIL **FM timbre bank** (the authentic OPL instrument set)    |
| `TIMBRES.MT`    | **MT-32 custom timbre bank** — a directory of 248-byte Roland timbres the game uploads to a real MT-32 via SysEx (bank 0x40). Only needed for the Munt/MT-32 render. |
| `ROLAND.DRV`    | the original MT-32 DOS driver (reference only)                    |
| `ADLIB.DRV` etc | the other original DOS sound drivers (reference only)             |

Music tracks: `BASETUNE` (flight ambient + per-base tunes), `COMBAT`, `OPENING`,
`VICTORY`, `CREDITS`.

### `.ADL` vs `.GEN` (verified by hexdump)

Byte-for-byte the two share the **identical** envelope: a small offset table,
then back-to-back `FORM..XDIR` + `CAT..XMID` XMIDI containers (see below). The
only difference is *inside* the `EVNT` stream:

* `.ADL` selects AdLib FM patches (played through `TIMBRES.AD`).
* `.GEN` issues **General MIDI** program changes — e.g. BASETUNE sub-song 0
  opens with `C4 31 C5 0E C6 1D …` = GM patches 49 (String Ensemble), 14
  (Tubular Bells), 29 (Overdriven Guitar), 24 (Nylon Guitar), 52 (Choir Aahs)
  — a plausible orchestral arrangement; the leading `TIMB` chunk lists the same
  patch numbers. So `.GEN` is **standard GM** and renders correctly on any GM
  synth/SoundFont; the MT-32 mix uses these same sequences plus the custom
  `TIMBRES.MT` instruments.

### `.ADL` layout (MULTI-SONG — np-xa2)

A `.ADL` is **not one tune** — it is a *concatenation of complete XMIDI
containers*, one per sub-song. Each container is an XMIDI directory
`FORM..XDIR` (whose `INFO` chunk gives the sequence count) immediately
followed by `CAT..XMID` holding that many `FORM..XMID` sequences. In
Practice every Privateer container holds exactly **one** sequence, so the
multi-song-ness comes from there being **many containers** back-to-back, not
many sequences inside one.

We enumerate sub-songs by scanning for the directory magic directly: every
container starts with `FORM` and has `XDIR` at +8; a sub-song runs from one
such marker to the next (or EOF). A clean `FORM..XDIR`+`CAT..XMID` slice is
standard XMIDI an OPL renderer reads. We render **every** sub-song to its
own WAV — `<stem>_NN.wav`.

> **Why not libADLMIDI's `--song N` (adl_getSongsCount / adl_selectSongNum)?**
> It doesn't see them. Fed the whole `.ADL`, libADLMIDI parses only the first
> container ("File contains 1 song(s)") and renders the rest as trailing
> garbage (a ~1 GB nonsense WAV). Each *extracted* container also reports a
> single song. The **manual XDIR/CAT split is the only method that yields the
> full set.** The old renderer kept only the single **largest** container per
> file and discarded the other 10/12 — which is why the main in-flight theme
> and the per-base tunes were missing.

### `TIMBRES.AD` layout (Miles/AIL `.AD`)

A 6-byte directory of `(patch, bank, u32 offset)` entries (terminated by
`0xFF`), each pointing at an instrument: `len(2) + transpose(1) + 11 OPL
register bytes` laid out `mod{20,40,60,80,E0}, fb/conn(C0), car{20,40,60,80,E0}`.
`bank == 0x7F` marks percussion. (Authority: libADLMIDI
`utils/gen_adldata/file_formats/load_ail.h`.)

## Renderer — HQ General-MIDI path (default, np-gln)

FluidSynth / Munt speak **SMF**, not XMIDI, so `tools/xmidi2smf.py` converts each
XMIDI sub-song to a standard **SMF type-0** `.mid` first. XMIDI differs from SMF
in three ways the converter undoes (authority: Pentagram/Exult `xmidi.cc`,
ScummVM `midiparser_xmi.cpp`):

1. Delta time is a *sum of consecutive bytes < 0x80* (not a single VLQ).
2. Note-On carries its own **duration** (a trailing VLQ); we synthesise the
   matching Note-Off.
3. Meta events use a *single-byte* length field.

**Timebase — the fixed 120 Hz clock (TEMPO FIX, np-vcr)**: XMIDI/Miles AIL plays
on a **fixed clock of 120 ticks per SECOND** and **ignores the embedded `FF51`
tempo meta** entirely. The authentic OPL reference (libADLMIDI reading the `.ADL`
directly) proves it: for **all 27** sub-songs the reference duration equals
`tick_span / 120` + the synth's ~1 s release tail — *including* the multi-tempo
cues (`OPENING` carries 25 `FF51` events, `VICTORY` 6) whose tempo changes have
**zero** effect on the wall-clock length.

* **The bug**: the old converter emitted `PPQN=120` **and passed `FF51`
  through**, so FluidSynth *obeyed* the tempo. A cue authored at 60 BPM landed
  at exactly 120 ticks/s — correct *by luck*, which is why the 60-BPM base/agri
  tunes (e.g. `basetune_04`) sounded fine — but a 140 BPM combat cue ran at
  `140/60·120 = 280` ticks/s = **2.33× too fast**, and the 142 BPM stingers
  faster still. Every cue was off by `BPM/60`, so *most* tracks galloped while a
  couple were fine. Validating against only the 60 BPM `basetune_04` masked it.
* **The fix**: reproduce the fixed clock. Emit at **120 ticks/second for every
  track** using the ScummVM-canonical XMIDI division — `PPQN=60` with a single
  injected 120 BPM tempo (`500000` µs/qtr ⇒ `1e6/500000 · 60 = 120` ticks/s) at
  tick 0 — and **drop the source `FF51` events** (AIL never honoured them). Both
  constants live in `tools/xmidi2smf.py` (`XMIDI_PPQN`, `XMIDI_FIXED_TEMPO_US`).
* **Validation**: `tools/validate_music_tempo.py` renders every `.ADL` sub-song
  with libADLMIDI (the timed-by-construction reference) and compares to the GM
  `.GEN` render. After the fix **all 27 match** the AdLib reference *or* their
  own fixed-clock length `span/120` (within the synth tail) — e.g. the combat
  beds went from `0.46×` (2.2× too fast) to `1.02–1.04×`. `credits_00` is the
  one cue whose GM `.GEN` arrangement is genuinely *longer* than its `.ADL`
  twin (more bars — a content difference, **not** tempo: it is still tempo-exact
  vs its own `span/120`). Authority: Pentagram/Exult `xmidi.cc`, ScummVM
  `midiparser_xmi.cpp` ("XMIDI … 120 Hz, the same as a tempo of 500000 with a
  PPQN of 60").

The `.mid` is then rendered by **FluidSynth** + a GM SoundFont and peak-
normalised to −1.5 dBFS with **ffmpeg**. All three are **asset-time tools only**
— none are linked into the game.

```sh
brew install fluid-synth ffmpeg      # (ffmpeg may already be present)
```

### SoundFont (user-supplied, never committed)

FluidSynth needs a GM bank. In order of preference, `render_music.py` uses:

1. `--soundfont PATH` or `$PRIVATEER_SOUNDFONT` — point at a nicer `.sf2`
   (FluidR3_GM, GeneralUser GS, Arachno) for the best result; **or**
2. macOS' built-in `gs_instruments.dls` (the Roland-derived QuickTime/Sound
   Canvas bank at `/System/Library/Components/CoreAudio.component/Contents/
   Resources/gs_instruments.dls`) — a zero-download default that already sounds
   far better than OPL FM.

SoundFonts and DLS banks are third-party copyrighted assets — **gitignored,
never committed** (`*.sf2`, `*.dls`).

## Renderer — AdLib OPL fallback (np-m96)

When no FluidSynth/SoundFont is available, `render_music.py` falls back to
**libADLMIDI** (Nuked OPL3) reading the `.ADL` XMIDI directly through the game's
own `TIMBRES.AD` FM bank. AdPlug was tried first and **rejected** (does not
recognise Origin XMIDI). `tools/ail2wopl.py` converts the Miles/AIL `.AD` bank to
the WOPL format libADLMIDI's custom-bank loader wants (byte mapping per
`load_ail.h` + `adlmidi_cvt.hpp`; WOPL `operators[0]` = carrier, `[1]` = mod).

Build libADLMIDI once (anywhere; `/tmp` is fine):

```sh
git clone --depth 1 https://github.com/Wohlstand/libADLMIDI.git
cmake -S libADLMIDI -B libADLMIDI/build -DCMAKE_BUILD_TYPE=Release \
  -DlibADLMIDI_STATIC=ON -DWITH_MIDIPLAY=ON -DWITH_OLD_UTILS=ON -DUSE_NUKED_EMULATOR=ON
cmake --build libADLMIDI/build -j
# -> libADLMIDI/build/adlmidiplay  (renders MIDI/XMI -> WAV with -w)
```

## Future — Roland MT-32 path (most authentic; needs user ROMs)

The richest, most faithful Privateer sound is a real Roland MT-32 playing the
`.GEN` sequences with the game's **custom** `TIMBRES.MT` timbres uploaded via
SysEx. To produce it: convert the `.GEN` sub-songs with `tools/xmidi2smf.py`
(same as the GM path) and render the SMFs with the **Munt** MT-32 emulator
(`munt` / `mt32emu`) plus the game's `TIMBRES.MT`. Munt requires the user's own
legal MT-32 **control + PCM ROMs** — we ship neither Munt nor the ROMs. On this
machine no MT-32 ROMs were present and Munt is not packaged in Homebrew, so the
GM path above is the default. ROMs are gitignored (`MT32_*.ROM`, `CM32L_*.ROM`).

## One-shot render

`tools/render_music.py` ties it together: split each source into its XMIDI
sub-songs (np-xa2) and render **every** one to `<stem>_NN.wav` at 44.1 kHz
stereo. It auto-selects the HQ path when FluidSynth + ffmpeg + a SoundFont are
available, else the AdLib fallback.

```sh
# HQ General MIDI (default), using a custom SoundFont:
python3 tools/render_music.py \
  gog_extracted/extracted/priv/DATA/SOUND \
  gog_extracted/music_wav \
  assets/music/original \
  --soundfont /path/to/FluidR3_GM.sf2     # optional; omit to use macOS DLS

# Force the AdLib OPL fallback:
python3 tools/render_music.py \
  gog_extracted/extracted/priv/DATA/SOUND \
  gog_extracted/music_wav \
  assets/music/original \
  --synth adlib --adlmidiplay /tmp/libADLMIDI/build/adlmidiplay
```

### Sub-song inventory (what each source actually contains)

The `.GEN` and `.ADL` have the **same number of sub-songs** at the **same
indices** (so the F8 labels in `docs/music_labels.json` still apply), but the GM
arrangements have their own lengths — e.g. several `.GEN` cues are shorter/longer
than their `.ADL` twins. Full set rendered into `gog_extracted/music_wav/`.
**Human labeling via the in-game F8 music labeler identifies each one** — the
`_NN` names are positional (container index), not semantic.

| file          | sub-songs | rendered WAVs        | notes                                              |
|---------------|-----------|----------------------|----------------------------------------------------|
| `BASETUNE.GEN`| **11**    | `basetune_00..10`    | main in-flight theme **+** the per-base tunes (Oxford / Pleasure Base / agri / mining / …). |
| `COMBAT.GEN`  | **13**    | `combat_00..12`      | 4 long combat loops (`_04`–`_07`) + 9 short stingers/cues (`_00`–`_03`, `_08`–`_12`). |
| `OPENING.GEN` | 1         | `opening_00`         |                                                    |
| `VICTORY.GEN` | 1         | `victory_00`         |                                                    |
| `CREDITS.GEN` | 1         | `credits_00`         |                                                    |

Per-sub-song durations of the **CORRECT-TEMPO GM render** (44.1 kHz stereo, peak
−1.5 dBFS) — post fixed-clock fix, each within the synth tail of its AdLib
reference (see `tools/validate_music_tempo.py`):

| basetune | s     | combat   | s     | others    | s     |
|----------|-------|----------|-------|-----------|-------|
| `_00`    | 99.9  | `_00`    | 5.8   | `opening_00` | 156.5 |
| `_01`    | 97.8  | `_01`    | 5.8   | `victory_00` | 53.0  |
| `_02`    | 87.1  | `_02`    | 5.8   | `credits_00` | 112.6 |
| `_03`    | 76.7  | `_03`    | 7.0   |           |       |
| `_04`    | 122.0 | `_04`    | 75.9  |           |       |
| `_05`    | 81.5  | `_05`    | 130.8 |           |       |
| `_06`    | 90.2  | `_06`    | 104.1 |           |       |
| `_07`    | 133.9 | `_07`    | 85.6  |           |       |
| `_08`    | 101.7 | `_08`    | 10.5  |           |       |
| `_09`    | 99.4  | `_09`    | 43.3  |           |       |
| `_10`    | 112.6 | `_10`    | 11.5  |           |       |
|          |       | `_11`    | 6.3   |           |       |
|          |       | `_12`    | 9.9   |           |       |

Every sub-song rendered to real audio (all normalised to peak ~27600/32767).
Per `docs/music_labels.json`, `combat_04` is the **main flight theme**, the
`combat_05`/`_06`/`_07` cues are the far/near/resolve combat tiers, the
`combat_08`/`_09`/`_10` cues are the jump/landing/death stings, and the
BASETUNE entries are the per-base tunes.

The runtime **music director** (`src/music.{h,cpp}`, np-ida) loads these
per-sub-song stems from `assets/music/original/<stem>.wav` (e.g.
`combat_04.wav`, `basetune_00.wav`) — `render_music.py` mirrors exactly the
director's `DIRECTOR_STEMS` set into `asset_dir` under those same filenames
(the F8 labeler reads the *full* set from `out_dir`, so it is unaffected).

## Legal

Rendered/original WAVs are **derived from copyrighted Origin/EA assets** and are
**LOCAL-ONLY / gitignored** (`gog_extracted/`, `assets/music/original/`), exactly
like the SFX. GM SoundFonts (`*.sf2`/`*.dls`) and MT-32 ROMs (`MT32_*.ROM`,
`CM32L_*.ROM`) are third-party copyrighted assets the **user supplies** — also
gitignored, never committed. Only the **tools** (`tools/ail2wopl.py`,
`tools/xmidi2smf.py`, `tools/render_music.py`) and this doc are committed. A
clean clone has no music and runs silent.
