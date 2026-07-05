# Cinematic Authoring SDK + Portrait Pipeline

Python-only tooling that pre-generates the 512x640 RGBA character portraits the
in-engine cinematic director loads by path. **The C++ engine has zero AI/network
code** — it only ever opens a PNG. All generation, caching, and QC live here.

Per-line generation makes **character consistency** the central problem, so this
pipeline is *reference-first*: paint one canonical headshot per character, get a
human to bless it, then condition every line's portrait on that reference via an
image-EDIT endpoint. Fresh text-to-image per line drifts identity — we don't do
that.

## Files

| file | what |
|------|------|
| `../../assets/data/characters.json` | the **character bible** — cast pulled from `src/campaign.h` + `fixers.json` (grayson, sandoval, tayla, lynch, masterson, murphy, monkhouse, cross, terrell + generic pirate/militia/confed/bounty_hunter). Appearance/personality/wardrobe/ref-path per character. |
| `style_bible.md` | the shared style prefix every call prepends (modern painted sci-fi, chest-up 3/4, cockpit lighting, 512x640). Mirrors `STYLE_PREFIX` in `portraits.py`. |
| `portraits.py` | the CLI + library (`gen-ref`, `gen-line`, `qc`, `list`). |
| `backends.py` | `ImageBackend` abstraction + `OpenAIBackend`, `GeminiBackend`, `PlaceholderBackend` + `finalize_portrait` (force 512x640 RGBA). |
| `cache.py` | content-hash cache (free retakes). |
| `qc.py` | consistency QC seam (`check(portrait, ref) -> {pass, reason, score}`). |
| `builder.py` | **Phase 4** fluent authoring API — emits a valid `assets/cinematics/<id>.json`, auto-generating line portraits. Also `apply_overrides()` for surgical per-line edits (Studio refine). |
| `voices.py` | MiniMax `t2a_v2` per-line TTS (cloned cast voices, content-hash cached, silent no-op without a key). |
| `studio_bridge.py` | **Studio Phase C** — the daemon that answers the in-game Studio panel's requests (see below). |
| `validate.py` | **Phase 4** schema + semantic validator (`python -m tools.cinematics.validate <id>`). |
| `preview.py` | **Phase 4** dev_remote client — reload/play/seek/screenshot iterate loop + contact sheet. |
| `examples/` | reference cinematics authored *via* `builder.py` (`example_flyby.py`, `example_confrontation.py`). |
| `requirements.txt` | Pillow (+ httpx for the cloud path + preview client; jsonschema optional). |

## Install

```bash
pip install -r tools/cinematics/requirements.txt
```

## Workflow:  bible → gen-ref → (human approve) → gen-line → qc

Run from the repo root.

### 1. See the cast

```bash
python -m tools.cinematics.portraits list
```

### 2. `gen-ref` — canonical reference headshot (once per character)

```bash
python -m tools.cinematics.portraits gen-ref grayson
python -m tools.cinematics.portraits gen-ref pirate
```

Writes `assets/cinematics/portraits/<char>/_ref.png`. **Eyeball it and approve
it** — everything downstream conditions on this file.

### 3. `gen-line` — one portrait per spoken line (cached)

```bash
python -m tools.cinematics.portraits gen-line grayson \
    --scene demo --seq 01 \
    --emotion "cold warning, jaw set" \
    --text "Cut your engines and drop the cargo. Last warning."
```

Writes `assets/cinematics/portraits/grayson/demo_01.png` — that path string is
exactly what a `"line"` cue's `portrait` field references in the timeline JSON.

`gen-line` conditions on `_ref.png` via the image-EDIT endpoint (OpenAI
gpt-image-1 edits / Gemini image gen with the reference as input), so identity
holds across lines. Flags:

* `--retries N` — QC retry budget (default 2). On repeated QC failure it falls
  back to copying `_ref.png` as a neutral portrait so the cinematic never has a
  missing frame.
* `--max-images N` — per-run cost/budget guard.
* `--no-cache` — force a fresh call.
* `--backend openai|gemini|placeholder` — override auto-selection.

### 4. `qc` — check identity drift

```bash
python -m tools.cinematics.portraits qc \
    assets/cinematics/portraits/grayson/demo_01.png grayson
# (second arg accepts a character id OR a path to a _ref.png)
```

Prints `{"pass", "reason", "score"}`; exit 0 on pass, 1 on fail.

## Backend abstraction

`get_backend(name)` returns an `ImageBackend`:

```python
class ImageBackend:
    def available(self) -> bool: ...
    def txt2img(self, prompt) -> bytes: ...              # gen-ref
    def img2img(self, prompt, ref_path) -> bytes: ...    # gen-line (ref-conditioned)
```

* **OpenAIBackend** — `gpt-image-1`, `/v1/images/generations` + `/v1/images/edits`, `OPENAI_API_KEY`.
* **GeminiBackend** — Gemini image model via REST `generateContent`, `GEMINI_API_KEY`.
* **PlaceholderBackend** — always available; no key needed.

Selection: `--backend` if given (a real backend with no key gracefully degrades
to placeholder); otherwise auto — OpenAI if keyed, else Gemini if keyed, else
placeholder. Swapping in local SD/ComfyUI later = add one `ImageBackend`
subclass; nothing else changes. Every backend's output is forced to exactly
512x640 RGBA by `finalize_portrait` (center cover-crop, top-biased).

## Placeholder fallback (zero-spend demo)

With **no API key set**, generation draws an obvious labelled 512x640 card
(character name + line text + a crude head silhouette, deterministic hue per
character so ref and lines read as the same person, tagged `[OFFLINE / NO KEY]`).
This lets the whole cinematic flow be authored and rendered in-game without
spending a cent. The moment a key is present, the real API path just works — no
code change.

## How the cache works

Content-hash keyed on **(character ref hash + full prompt + backend + size)**,
stored under `tools/cinematics/.cache/<key>.png` with an `index.json` manifest.

* Re-running `gen-line` with an unchanged prompt/ref → **CACHE HIT**, copies the
  blob to the destination, **no API spend**.
* Same generation reused at two output paths → one call, second is a hit.
* Change the line text, emotion, or the approved `_ref.png` → new key → regen.
* `rm -rf tools/cinematics/.cache` to force full regeneration.

## For the cinematic-director agent

Drive the pipeline programmatically and plug in your own vision judge:

```python
from tools.cinematics.portraits import gen_line

def agent_vision_check(portrait_path, ref_path):
    # the agent LOOKS at both images and decides identity match
    return {"pass": True, "reason": "same character, expression matches line"}

out = gen_line(
    character="grayson", scene="m01", seq="03",
    text="I didn't sign up for this.",
    emotion="bitter, looking away",
    qc_fn=agent_vision_check,   # replaces the built-in aHash check
    retries=2,
)
# out -> assets/cinematics/portraits/grayson/m01_03.png  (wire into the "line" cue)
```

The built-in `qc.check` is a cheap aHash/not-blank sanity gate; the agent's
`qc_fn` is the real identity verifier. On repeated failure the pipeline auto-
falls back to `_ref.png` so authoring never blocks.

---

# Authoring SDK (Phase 4)

The portrait pipeline above is *step 2* of a larger loop. The full authoring
workflow is:

```
  builder.py  →  (auto portrait gen)  →  validate.py  →  preview.py / contact sheet  →  the game renders it
```

All three modules run from the repo root and share the DSL contract in
`docs/cinematic_format.md` + `assets/cinematics/schema.json`.

## 1. `builder.py` — emit a cinematic from Python

A fluent API where every method maps to exactly one DSL cue. `c.at(t)` binds a
time; the returned binder appends cues at that time (and chains).

```python
from tools.cinematics.builder import Cinematic

c = Cinematic("intro_troy", letterbox=True, skippable=True,
              auto_portraits=True, portrait_backend="placeholder")
c.at(0.0).fade_in(2.0)
c.at(0.0).music("audio/intro_theme.wav")
c.at(0.5).camera_path(keys=[[0,200,3000], [1500,0,1500]],
                      look_at="ship:hero", ease="smooth", dur=6.0)
c.at(1.0).spawn("hero", cls="tarsus", pos=[500,0,800])
c.at(3.0).line("grayson", "I didn't sign up for this.",
               emotion="bitter", side="left", dur=3.5)   # <-- auto-gens art
c.at(11.5).end(actions=["set_flag:intro_seen"])
c.save()   # -> assets/cinematics/intro_troy.json
```

**Methods** (all on `c.at(t)`): `fade_in(dur)`, `fade_out(dur)`, `music(file)`,
`sfx(file, pos=None)`, `camera_path(keys, dur, look_at=None, ease="smooth")`,
`spawn(actor, cls, pos, faction="civilian")`, `actor_path(actor, keys, dur)`,
`line(character, text, dur, emotion, side, voice_file, portrait, speaker, generate)`,
`subtitle(text, dur)`, `end(actions)`.

**Camera keys** accept bare `[x,y,z]` positions or `{"pos": ..., "look_at": ...}`
dicts; a top-level `look_at=` fills in any key that lacks its own (DRY).

**Auto portrait generation** — the killer feature. With `auto_portraits=True`
(or per-line `generate=True`), `.line(character, ...)`:
1. ensures the character's `_ref.png` exists (runs `gen-ref` on demand),
2. calls `portraits.gen_line(character, scene=<id>, seq=<auto>, text, emotion)`,
3. wires the returned PNG path straight into the cue's `portrait` field.

The nameplate `speaker` is resolved from the character bible's `display_name`
(override with `speaker=`). Pass `qc_fn=` to `Cinematic(...)` to hand the
director agent's vision judge down into every generation.

## 2. `validate.py` — schema + semantic checks

```bash
python -m tools.cinematics.validate intro_troy
```

Runs the JSON Schema (structural, via `jsonschema` if installed) **plus**
semantic rules the schema can't express:

* referenced portrait / audio files exist on disk (missing → *warning*, since
  the engine no-ops missing assets);
* every `actor_path.actor` and `look_at: "ship:<actor>"` was `spawn`-ed earlier;
* cue times are non-negative; a single `end` cue is the last cue;
* no two `camera_path` cues overlap in time.

Returns `[{severity, cue_index, message}]`; exit 0 when there are no `error`s
(warnings are fine). Library entry point: `validate(id) -> list[Issue]`.

## 3. `preview.py` — iterate against the running game

```bash
python -m tools.cinematics.preview intro_troy            # auto key times
python -m tools.cinematics.preview intro_troy --at 1,3,8 # explicit
```

A `dev_remote` client (`http://127.0.0.1:47001`) that:
1. `POST /cinematic/reload` + `/cinematic/play`;
2. polls `GET /events?since=N`, keeping `category=="cinematic"` beats and
   parsing them into a structured timeline (`started`/`cue`/`line`/`music`/
   `sfx`/`seeked`/`ended`) — so you can assert `line_shown grayson at t=3.0`
   without reading pixels;
3. at each key timestamp (from `--at`, else auto-derived from `line`/`sfx` cue
   times) `POST /cinematic/seek` + `POST /screenshot`, collects the PNGs, and
   assembles a **contact sheet** (labelled grid) into
   `tools/cinematics/previews/<id>_contact.png`.

If the game isn't running it fails in <1s with a clear message (short connect
timeout, no hang). Library entry point: `preview(id, at=None) -> {events,
shots, contact_sheet, status}`.

## Reference cinematics (proof the SDK works)

Both are authored *via* `builder.py`, not hand-written:

```bash
python -m tools.cinematics.examples.example_flyby          # camera + music only
python -m tools.cinematics.examples.example_confrontation  # dual portraits, auto-gen
python -m tools.cinematics.validate flyby_demo             # -> PASS
python -m tools.cinematics.validate confrontation_demo     # -> PASS (audio warnings only)
```

`example_confrontation.py` spawns a Tarsus (hero) and a Talon (pirate), flies a
tracking camera, and authors a three-line left/right exchange — each `.line()`
auto-generates its placeholder portrait and wires the path in.

## For the cinematic-director agent (Phase 5)

The agent's author/iterate loop is exactly these three modules:

```python
from tools.cinematics.builder import Cinematic
from tools.cinematics.validate import validate, has_errors
from tools.cinematics.preview import preview, GameNotRunning

def agent_vision_check(portrait_path, ref_path):
    # the agent LOOKS at both images and judges identity + expression
    return {"pass": True, "reason": "same character, expression fits the line"}

# 1. AUTHOR (art generated inline, QC'd by the agent's own eyes)
c = Cinematic("m01_intro", auto_portraits=True, qc_fn=agent_vision_check)
c.at(0.0).fade_in(1.5)
c.at(2.0).line("grayson", "I didn't sign up for this.", emotion="bitter")
c.at(6.0).end(actions=["set_flag:m01_seen"])
cid = c.save()

# 2. VALIDATE (never ship a broken timeline)
issues = validate(cid)
if has_errors(issues):
    ...  # feed messages back into the next authoring pass

# 3. PREVIEW (assert timing from events; look at the contact sheet)
try:
    result = preview(cid)
    assert any(b["kind"] == "line" and b.get("t") == 2.0 for b in result["events"])
    # inspect result["contact_sheet"] with the agent's vision model
except GameNotRunning:
    ...  # author+validate still work headless; preview needs the game up
```

The loop is: **build → validate → (fix, repeat) → preview → eyeball the contact
sheet + assert the event timeline → adjust cue times / portraits → reload**.
Authoring and validation are fully headless (great for CI / offline placeholder
mode); only `preview` needs the game running.

---

# Cinematic Studio bridge (Phase C)

The Studio lets Mike author and refine cutscenes **from inside the game**
(docs/cinematic_studio.md). The in-game half is the ImGui panel (**Ctrl+K**);
this is the out-of-game half:

```bash
python -m tools.cinematics.studio_bridge              # daemon, polls every 2s
python -m tools.cinematics.studio_bridge --once       # drain the queue, exit
python -m tools.cinematics.studio_bridge --interval 5
```

## End-to-end workflow

1. **Start both halves**: launch the game, and in another terminal start the
   bridge (above). Keys are optional — no `OPENAI_API_KEY` means placeholder
   portraits, no `MINIMAX_API_KEY` means voice steps are skipped with a note.
2. **Compose** (Ctrl+K → Compose tab): describe the cutscene, when it fires,
   and the world state it leaves behind — all in English. Send writes
   `assets/cinematics/studio/requests/<id>.json`.
3. **The bridge picks it up** (≤2s later), answers `responses/<id>.json` with
   `status:"working"`, and for `kind:"author"` invokes the director agent
   headlessly:
   `code-puppy --agent cinematic-director -p "<compiled prompt>"` — the agent
   authors via `builder.py` (auto portraits + voices), merges the compiled
   trigger into `triggers.json`, compiles the outcome block, and validates.
   No `code-puppy` on PATH (or `STUDIO_BRIDGE_NO_AGENT=1`)? The response is an
   `error` whose message is the exact command to paste yourself.
4. **Watch the Requests tab**: pending → working → done. One click on
   Reload + Play renders the new cinematic in-game.
5. **Refine loop** (Lines tab): tweak a line's text, emotion, prompt extra,
   seed image, voice, or speed; tick the regen boxes; Send. That's a
   `kind:"refine"` request — **deterministic, no LLM**: the bridge re-runs
   `portraits.gen_line` / `voices.gen_line_voice` with your overrides,
   regenerating **to the same asset paths** (untouched cues never change,
   unchanged prompts are free cache hits), wiring text/new-voice changes in
   via `builder.apply_overrides`. Reload + Play again. Repeat until it sings.

## Pipeline override knobs (also on the CLI)

```bash
python -m tools.cinematics.portraits gen-line pirate \
    --scene ambush_troy --seq 01 \
    --text "Wrong lane, Troy-boy." --emotion "cold sneer" \
    --ref assets/cinematics/portraits/pirate/_ref.png \
    --prompt-extra "red warning light on the face"
```

`--ref` conditions on THAT image instead of the character's `_ref.png` (the
Studio's per-line `seed_image`); `--prompt-extra` appends direction after the
emotion. Both are part of the content-hash cache key: change them and it
regenerates, keep them and the retake is free. Voice delivery is `voice_id` +
`speed` only — `emotion` is deliberately not sent to TTS (see `voices.py`
docstring).
