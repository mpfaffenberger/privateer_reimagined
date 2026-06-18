# privateer_db moved

The canonical Privateer database (formerly `docs/privateer_db/`) and
`docs/privateer_ship_data.json` now live under **`assets/data/`**:

- `assets/data/privateer_ship_data.json`  — ships / guns / shields / armor
- `assets/data/privateer_db/cargo.toml`   — 50 commodities
- `assets/data/privateer_db/README.md`    — extraction notes

Reason: these are **load-bearing at runtime** (gun/shield/armor/commodity
tables load them at startup). Keeping every runtime dependency under
`assets/` makes that tree fully self-contained, so a shipped build only
needs `assets/` next to the binary — no `docs/`. Regenerate with
`python3 tools/import_privateer_db/cargo.py` (now writes to assets/data/).
