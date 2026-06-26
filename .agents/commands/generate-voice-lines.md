# Generate Voice Lines (cloned-cast comms)

## Purpose

Produce new in-character spoken comms for the game using the **cloned MiniMax
voices** of the original Privateer cast (bar characters + flight faction voice
actors). Use this to add taunts, greetings, low-hp/kill barks, mission lines,
or whole multi-character conversations, then render them to MP3.

This is the *content* loop. The one-time *recovery + cloning* loop (how the
cast got cloned in the first place) is documented in the Appendix and in
`docs/bar_speech_vpk_format.md`.

## TL;DR

```bash
# 0. activate the RE venv (has numpy/torch/speechbrain/sklearn) + key
export MINIMAX_API_KEY=$(grep '^export MINIMAX_API_KEY=' ~/.zshrc | cut -d= -f2- | tr -d '"'"'"'"')
PY=re/.venv/bin/python

# 1. edit/extend the line pools in tools/gen_comms.py (see "Authoring")
# 2. generate the text corpus (variety knob = --variants)
$PY tools/gen_comms.py --variants 10        # -> assets/speech/generated/comms.json
# 3. synthesize (idempotent, prints billed usage_characters)
$PY tools/synth_comms.py                     # -> generated/audio/*.mp3 + generated/scenes/*.mp3
# 4. listen
afplay assets/speech/generated/scenes/conv_00.mp3
```

## Prerequisites

- **`re/.venv`** Python env with deps already installed: `numpy torch torchaudio
  speechbrain scikit-learn` (only numpy/torch/etc. needed for *re-clustering*;
  pure generation/synthesis just needs the stdlib + `ffmpeg` + `curl`).
- **`ffmpeg`** + **`curl`** on PATH.
- **`MINIMAX_API_KEY`** exported in `~/.zshrc` (never echo/commit it).
- The cast is **already cloned** on MiniMax. The clone map lives in
  `assets/speech/clone_refs/uploads.json` (`speaker -> file_id -> voice_id`).
  To clone a *new* speaker, see "Add a new speaker".

## Voice map (cloned `voice_id`s)

Source of truth: `assets/speech/clone_refs/uploads.json` and the `VOICE` dict in
`tools/gen_comms.py`. Faction -> primary cloned flight voice:

```txt
merchant       PrivFlightV1401 / V0001 / V0601
militia        PrivFlightV0101 / V0701
pirate         PrivFlightV1501 / V0201 / V1201
bounty_hunter  PrivFlightV0501 / V1101 / V1301
confed         PrivFlightV0801
kilrathi       PrivFlightV0901
retro          PrivFlightV0401
steltek        PrivFlightV1001
player (pc)    PrivBarPc01
```

### Bar / plot / PC voices (cloned)

These are the named story characters, the player, and the generic bar NPCs from
`CONV/*.VPK` (speaker labels recovered from the PFC metadata). Use them for
plot dialogue, mission-giver lines, and base/bar chatter. `n` = distinct source
lines the clone was trained on (more = better fidelity).

| voice_id | character | role (from dialogue) | n |
|----------|-----------|----------------------|---|
| `PrivBarPc01` | **PC** | the player (Grayson Burrows) - all player-side lines | 367 |
| `PrivBarRandNpcRandcu301` | generic patron | ambient bar **rumors** / gossip | 404 |
| `PrivBarTaryn01` | **Taryn** | mission giver (

## Guardrails

- **Do NOT render the full template cross-product.** `{c}/{g}/{p}/{s}/{n}` are a
  *generation-time variety knob*, not runtime slots. Full cross-product is
  ~8,250 clips **per voice** (~20k+ total) and pointless. Bake a sampled set
  (`--variants`) and let the game random-pick from each faction's pool, exactly
  like the original game. (If you truly need runtime insertion of the player's
  callsign/sector, that's slot-concatenation, a different design - don't
  brute-force it.)
- **Cost is per character.** Every `t2a_v2` response carries
  `extra_info.usage_characters`; `synth_comms.py` sums and prints the total.
  The full ~700-clip corpus bills ~40k characters (~a few dollars).
- **Don't commit the bulky audio.** `assets/speech/generated/audio/`,
  `.../scenes/`, `.../clone_tests/`, and the extracted source WAVs are
  regenerable + large -> gitignored. Commit the *tools*, the *corpus JSON*, and
  the *clone map*.
- **Never commit `MINIMAX_API_KEY`.**

## Steps

### 1. Authoring - add/extend lines

Edit `tools/gen_comms.py`:

- **Short/medium lines:** add to `LINES[faction][category]` or `EXTRA[...]`.
  Categories: `greeting`, `hostile`, `low_hp`, `kill`, `demand`.
  Use placeholders for variety: `{c}` callsign, `{g}` cargo, `{p}` place,
  `{s}` ship, `{n}` name. Each placeholder is rolled independently.
- **Long monologues:** add to `LONG[faction]` (sermons, briefings, sob
  stories). These are capped to <=2 variants so they don't over-duplicate.
- **Conversations:** add a `(title, [(faction, text), ...])` tuple to `PAIRS`.
  Voices stay consistent per speaker; a single-faction convo (e.g. two
  merchants) alternates two voices automatically.
- **Fillers:** widen `CALLSIGN / PLACE / CARGO / SHIP / NAME` for more variety.

### 2. Generate the corpus

```bash
re/.venv/bin/python tools/gen_comms.py --variants 10
```

`--variants N` = max distinct fills per templated line (the variety knob; 5 is
lean, 10 is rich, higher hits diminishing returns vs filler cardinality).
Writes `assets/speech/generated/comms.json` (`lines` + `conversations`).
Verify counts in the printed summary; confirm no raw `{...}` leaked:

```bash
re/.venv/bin/python - <<'PY'
import json,re
d=json.load(open("assets/speech/generated/comms.json"))
t=[l["text"] for l in d["lines"]]+[x["text"] for c in d["conversations"] for x in c["turns"]]
print("clips:",len(t),"leaks:",sum(bool(re.search(r'\{[cpgsn]\}',s)) for s in t))
PY
```

### 3. Synthesize

```bash
export MINIMAX_API_KEY=$(grep '^export MINIMAX_API_KEY=' ~/.zshrc | cut -d= -f2- | tr -d '"'"'"'"')
re/.venv/bin/python tools/synth_comms.py            # full batch
# or test first:
re/.venv/bin/python tools/synth_comms.py --limit 4 --convs 1
```

- standalone lines -> `assets/speech/generated/audio/<comm_id>.mp3`
- conversations -> per-turn clips + stitched scene `generated/scenes/<conv_id>.mp3`
- idempotent (skips existing); prints `TOTAL billed usage_characters` at the end.

Long batches: run in the background and poll file counts:

```bash
nohup re/.venv/bin/python tools/synth_comms.py > /tmp/synth.log 2>&1 &
ls assets/speech/generated/audio/*.mp3 | wc -l ; tail -3 /tmp/synth.log
```

### 4. QA listen

```bash
afplay assets/speech/generated/scenes/conv_00.mp3        # a stitched scene
afplay assets/speech/generated/audio/comm_0000.mp3       # a single line
```

## Add a new speaker (one-time clone)

If you need a voice we haven't cloned yet (new character / better reference):

```bash
# 1. build a clean ~30s reference by concatenating that speaker's distinct clips
#    (extend SOURCES in tools/build_clone_refs.py to point at the clips)
re/.venv/bin/python tools/build_clone_refs.py            # -> clone_refs/<id>.mp3 + manifest.json
# 2. upload references (purpose=voice_clone) -> file_ids
re/.venv/bin/python tools/upload_clone_refs.py           # -> clone_refs/uploads.json
# 3. clone each + render a test line
re/.venv/bin/python tools/clone_and_tts.py               # -> clone_tests/<voice_id>.mp3
# 4. add the new voice_id to VOICE in tools/gen_comms.py
```

MiniMax contract (no GroupId needed on api.minimax.io):
- upload: `POST /v1/files/upload` multipart `purpose=voice_clone file=@ref.mp3`
  -> `{"file":{"file_id":...}}`
- clone:  `POST /v1/voice_clone` json `{"file_id":...,"voice_id":"<Custom01>"}`
  (voice_id: >=8 chars, starts with a letter, has a digit)
- synth:  `POST /v1/t2a_v2` with `voice_setting.voice_id=<voice_id>`

## Artifacts

| path | what | commit? |
|------|------|---------|
| `tools/gen_comms.py` `synth_comms.py` | generate + synth | yes |
| `tools/build_clone_refs.py` `upload_clone_refs.py` `clone_and_tts.py` | cloning | yes |
| `tools/embed_speech.py` `cluster_voices.py` `classify_*_speakers.py` | classification/clustering | yes |
| `assets/speech/generated/comms.json` | the text corpus | yes |
| `assets/speech/clone_refs/uploads.json` | clone map (file_id/voice_id) | yes |
| `assets/speech/generated/audio/`, `.../scenes/`, `.../clone_tests/` | rendered audio | no (gitignored, regenerable) |

## QA checklist

- [ ] `gen_comms.py` prints expected line/conversation counts
- [ ] no raw `{...}` placeholders leaked into `comms.json`
- [ ] `synth_comms.py` test run (`--limit 4 --convs 1`) produces valid MP3s
- [ ] full run reports `fail=0` and a sane `usage_characters` total
- [ ] a stitched scene plays as a coherent multi-voice exchange
- [ ] bulky audio dirs are gitignored, not staged
- [ ] `MINIMAX_API_KEY` not staged anywhere

## Appendix - how the cast was recovered (reference)

One-time pipeline that produced the cloned voices (see
`docs/bar_speech_vpk_format.md` for the deep dive):

1. **Decode** - bar speech `CONV/*.VPK` is LZW-compressed VOC; flight speech is
   `SPEECH.PAK` VOCs. `tools/extract_bar_speech.py` + `extract_speech_pak.py`.
2. **Transcribe** - `tools/transcribe_bar_speech.py` (local faster-whisper).
3. **Classify speakers** - bar by PFC metadata
   (`classify_bar_speakers.py`); flight by content+block
   (`classify_flight_speakers.py`, with disposition).
4. **Cluster voice actors** - `embed_speech.py` (ECAPA) + `cluster_voices.py`.
5. **Clone** - build_clone_refs -> upload_clone_refs -> clone_and_tts.
