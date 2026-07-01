#pragma once
// -----------------------------------------------------------------------------
// commodity.h — the trade-goods catalog (what CAN be bought and sold).
//
// Privateer's economy trades 50 commodities across 11 categories (FOOD,
// RAWMAT, LUXURY, WEAPONS, ... — including the infamous SLAVES and MAGIC
// buckets). The canonical list lives in assets/data/privateer_db/cargo.toml,
// clean-room extracted from the original game's CARGO.IFF. This module
// loads it once at startup into a static catalog the trading layer
// (np-9cu.2) can look up by id.
//
// Why parse the TOML directly instead of converting to JSON offline?
// The file is the project's single source of truth for commodity data —
// a committed JSON copy would be a second copy that someone (Mike,
// realistically a future Mike at 2am) forgets to regenerate after
// re-running the extractor. The subset of TOML the extractor emits is
// trivially regular: `[section]` / `[[array-of-table]]` headers and
// `key = "string"` / `key = number` lines, nothing nested, no inline
// tables, no multi-line strings. A targeted ~60-line reader handles it
// with zero dependencies; if the extractor ever grows fancier output,
// the loader fails loudly (unparsed-line log) instead of silently.
//
// Per assets/data/privateer_db/README.md, keys with a leading underscore
// (_index, _file_offset, _form_size, _trailing_hex) are extraction
// debug breadcrumbs — ignored here by rule, not by enumeration.
//
// Commodity ids are derived from the display label: lowercase, spaces
// and slashes to underscores ("Generic Foods" -> "generic_foods").
// Stable as long as labels are (they're canonical 1995 data — they
// don't change), readable in save files and JSON, and immune to the
// reordering that raw indices would suffer if the extractor's sort
// ever changed.
// -----------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Commodity {
    std::string id;        // derived: "luxury_foods" (see header note)
    std::string label;     // display: "Luxury Foods" (canonical, from IFF)
    std::string category;  // "FOOD", "RAWMAT", ... (one of the 11)
};

namespace commodity {

// Parse assets/data/privateer_db/cargo.toml into the static catalog. Idempotent
// (reload replaces). Returns the number of commodities loaded; 0 means
// the file was missing or unparseable — the game keeps running (trading
// screens will just be empty) but the log line makes the failure clear.
// Logs one summary line: `[commodity] 50 commodities, 11 categories`.
int load(const std::string& toml_path);

// Append AUTHORED commodities from a supplemental cargo.toml-format file
// (issue #87). Same parser as load(), but it does NOT clear the catalog —
// it adds to whatever load() already populated. Use for goods that aren't in
// the clean-room CARGO.IFF extraction (e.g. salvage like scrap_metal) so the
// extracted cargo.toml stays a pristine 1:1 copy of the original data and a
// re-extraction never clobbers our additions. Call once at startup right
// after load() (before any Commodity* is cached). Returns the count added.
int load_extra(const std::string& toml_path);

// Catalog lookups. Pointers are stable for the program's lifetime once
// load() has run (the catalog vector is never resized afterwards).
// find() returns nullptr on unknown id.
const Commodity*              find(std::string_view id);
const std::vector<Commodity>& all();

// Label -> id derivation, exposed so other layers (save/load, the
// price-table loader when bases land) derive ids the exact same way
// instead of re-implementing the rule and drifting.
std::string id_from_label(std::string_view label);

// ---- contraband (Phase 1) ---------------------------------------------
// The contraband catalog is loaded separately from assets/data/contraband.json
// (a small JSON sidecar). Missing/unparseable file = non-fatal, returns 0;
// the game runs normally and every commodity just reports as non-contraband.
// Logs one summary line: `[commodity] N contraband ids`.
int load_contraband(const std::string& path);

// True iff the given catalog id is in the contraband set (loaded via
// load_contraband). False for unknown ids and before load_contraband runs.
bool is_contraband(std::string_view id);

// Severity (1..N) for a contraband id; 0 if not contraband (or before
// load_contraband). Severity is informational for the search director —
// hailing.{h,cpp} reads it to decide how aggressive the response is.
int contraband_severity(std::string_view id);

} // namespace commodity
