# Privateer canonical database

This directory holds the **canonical game data** for *Wing Commander: Privateer*,
extracted from the original 1995 release files via a clean-room toolchain.

## What's here

| File             | Source IFF in `PRIV.TRE`        | Contents                                   |
|------------------|----------------------------------|--------------------------------------------|
| `cargo.toml`     | `DATA/TYPES/CARGO.IFF`           | 50 commodities, 11 categories              |
| _(coming next)_  | `DATA/TYPES/SHIPS.IFF` + `*TYPE.IFF` | Ship classes (Centurion, Orion, Galaxy, …) |
| _(coming next)_  | `DATA/TYPES/GUNS.IFF`            | Gun catalog                                |
| _(coming next)_  | `DATA/TYPES/SHIELDS.IFF`         | Shield variants                            |
| _(coming next)_  | `DATA/SECTORS/SECTORS.IFF`       | Galaxy systems + jump links                |
| _(coming next)_  | `DATA/SECTORS/BASES.IFF`         | Bases + per-base commodity pricing         |

## How it's generated

```bash
# 1. One-time: mount your legal GOG copy + extract the archives.
tools/extract_privateer.sh

# 2. Parse each IFF into TOML.
python3 tools/import_privateer_db/cargo.py
# (more parsers coming: ships.py, guns.py, shields.py, sectors.py, ...)
```

Both steps are **idempotent and safe to re-run**.

## Legal model

Same as OpenMW with Morrowind: every contributor supplies their own legal
copy of the game. We never redistribute the original Origin/EA `.IFF` /
`.TRE` / `.SHP` / etc. files. The **derived facts** in this directory
(ship stats, commodity labels, jump topology, etc.) are uncopyrightable
factual information — like a fan wiki, except machine-readable.

The extraction pipeline lives in `tools/import_privateer_db/`. The TRE
archive tool lives at `third_party/wctools/` (vendored from DMJC/wctools
with two small macOS build patches — see `third_party/wctools/VENDORED.md`).

## Schema notes

Each `*.toml` file starts with a `[_meta]` table:

```toml
[_meta]
source = "PRIV.TRE/DATA/TYPES/CARGO.IFF"
count = 50
categories = [...]
```

Fields prefixed `_` (e.g. `_index`, `_file_offset`, `_trailing_hex`) are
**debugging metadata** preserved so we can re-validate against the original
binary. Game logic should ignore them.

The `_trailing_hex` field in `cargo.toml` captures unparsed bytes from each
commodity record — we know they encode something category-specific (the
shape of the trailing bytes correlates perfectly with category) but we
haven't fully decoded the meaning yet. Likely candidates: base price,
base availability, faction-affinity flags. To be resolved when we parse
`BASES.IFF` and see what *that* refers to.

## References

- **dpjudas/WCPrivateer/Sources/FileFormat/** — best active reference
  implementation of the Privateer file formats (C++, 2025).
- **Mario "HCL" Brito** at `hcl.solsector.net` — original reverse-engineer
  of all Wing Commander binary formats.
- **DMJC/wctools** — `wctre` TRE extractor (vendored).
