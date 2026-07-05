# assets/cinematics/

Cinematic timelines + their media. See `../../docs/cinematic_format.md` for
the DSL and `schema.json` here for the machine-readable shape.

```
assets/cinematics/
  <id>.json            # a cinematic timeline (e.g. demo_flyby.json)
  schema.json          # JSON-schema for the timeline format
  audio/               # music / sfx / voice clips referenced by filepath
                       #   (dropped in here, NO manifest rebuild, NO recompile)
  portraits/<char>/    # PRE-GENERATED 512x640 PNG portraits (Phase 3 tooling)
                       #   the engine only LOADS these by path; it never
                       #   generates art. Missing PNG => neutral frame.
```

Trigger a cinematic today (Phase 1):
- **In-game debug keys** (from Flight): `F8` = `demo_flyby`, `F9` = `demo_exchange`.
  `Esc` skips a playing (skippable) cinematic.
- **CLI**: `./build/new_privateer --skip-title --play-cinematic <id> --cine-at <secs>`
  auto-plays after `<secs>` of Flight (headless smoke path).

The dev_remote `POST /cinematic/play|stop|reload|seek` endpoints land in Phase 2.
