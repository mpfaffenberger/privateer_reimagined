#pragma once
// -----------------------------------------------------------------------------
// savegame.h — JSON save slots + autosave for the persistent PlayerState.
//
// This is the durable companion to player.h: PlayerState is the half of the
// player a save file cares about (credits, cargo, owned ship + equipment,
// per-faction reputation, location), and this module is the thing that
// turns it into bytes on disk and back, bit-exact.
//
// WHERE saves live (macOS):
//   ~/Library/Application Support/new_privateer/saves/
// Apple's File System Programming Guide nominates Application Support for
// "files your app creates and manages on behalf of the user" that aren't
// user documents and shouldn't be surfaced in a file browser or synced like
// ~/Documents. Save games fit exactly — they're app-managed state, not
// something the player hand-edits. We create the directory tree on first
// save (std::filesystem::create_directories). Linux/Windows ports can swap
// the base dir later; the slot/format machinery is platform-agnostic.
//
// SLOTS: numbered. Slot 0 is the autosave (written on dock); slots 1..N are
// manual saves. Each slot is one file, save_<n>.json. Numbered rather than
// named because the harness + the (minimal) UI only need a handful and the
// future load menu can title each slot from the embedded `label` field
// without parsing the whole player blob.
//
// FORMAT versioning + compat: every file carries a `version` int. load()
// rejects a file from a NEWER format than it understands (better to refuse
// than to silently drop fields), tolerates MISSING fields from older formats
// by defaulting them, and never throws/crashes on a corrupt file — it logs
// and returns false so the caller falls back to new_game().
//
// STABLE KEYS: reputation is serialized as an OBJECT keyed by faction NAME
// (faction::to_name), not by array index, so reordering the Faction enum
// never silently scrambles who-likes-whom in old saves. Same discipline the
// rest of the codebase uses for enum<->disk boundaries.
//
// no third-party deps: serialization emits JSON via a tiny local writer
// (savegame.cpp); deserialization reuses the existing read-only json.cpp
// parser. (json.{h,cpp} is parse-only by design — see its header — so the
// writer lives here rather than bloating that module's documented scope.)
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

struct PlayerState;

namespace savegame {

// Bumped whenever the on-disk shape changes incompatibly. v1 = the initial
// PlayerState serialization; v2 added the accepted-missions list (np-zte.1);
// v3 added missile inventory + afterburner fuel (np-zte.2). load() tolerates
// older saves by defaulting the new keys (missiles -> 0, fuel -> full tank),
// so a v1/v2 save keeps working — the version bump just records the addition.
// v4 (np-3dp.19) added career faction-kill tallies + a live ship-damage
// snapshot (per-facing armor/shield + energy), and switched the on-disk
// label to the full timestamped title. Older saves default the new keys
// (kills -> 0, hp_valid -> false => spawn at full health).
// v5 (#8) expanded the per-mission payload on the accepted-missions list
// (source, target_system, nav_targets, nav_count, hostiles_required,
// target_base, bounty_region, last_seen(_alt)?_system) for the new
// mission types (#7). The new keys default on load for older saves; the
// only back-compat hazard is a `type` outside 0..5 — load() skips that
// entry and logs once.
// v6 (#16) added the two guild-membership bools (merc_guild_member,
// merchant_guild_member). Older saves default both to false (non-member),
// so a v5 save still loads cleanly.
// v7 (#138, campaign epic #136) added the plot state: `plot_flags` +
// `plot_items`, two flat string arrays (see plot.h). Older saves default
// both to empty == campaign not started, so every pre-campaign save keeps
// loading (and playing) exactly as before.
// v8 (Gemini Lives #171) added the persistent `day` counter. Older saves
// default to day zero (stardate 2669.135).
// v9 (#143) added the fitted `scanner_id`. Older saves load with
// player::k_starting_scanner (a v8 pilot never had one to sell).
// v10 (#145) added `turrets`, the owned turret-hardware slot ids. A pre-v10
// save is grandfathered: every turret slot with a gun already fitted in one
// of its mounts counts as owned, so an old Centurion keeps its rear turret.
constexpr int k_format_version = 10;

// Slot 0 is the autosave; manual saves start at 1.
constexpr int k_autosave_slot = 0;

// Absolute path to the saves directory, creating the tree if missing.
// Returns "" only if HOME is unset (degenerate; logged).
std::string saves_dir();

// Absolute path to one slot's file (does not touch the filesystem).
std::string slot_path(int slot);

// Serialize `p` to slot `slot` (atomic-ish: write a temp then rename so a
// crash mid-write can't truncate an existing good save). Returns false on
// any IO failure (logged). Embeds version + a unix timestamp + a human
// `label` (the full timestamped title) for the load menu.
bool save(const PlayerState& p, int slot);

// Write `p` to a brand-new uniquely-named file (save_<unix_nanos>.json) so
// saves ACCUMULATE without bound — nothing is ever overwritten (np-3dp.19).
// This is the autosave-on-land + manual-save path. Returns the file path on
// success ("" on failure, logged).
std::string save_timestamped(const PlayerState& p);

// Deserialize slot `slot` into `p` (overwriting it wholesale on success).
// Returns false — leaving `p` untouched — if the file is missing, corrupt,
// or from an unsupported format version. Never throws.
bool load(PlayerState& p, int slot);

// Deserialize an explicit save file PATH into `p` (the load-menu path).
// Same guarantees as the slot overload.
bool load(PlayerState& p, const std::string& path);

// Lightweight slot metadata for a load menu / "last autosave" readout,
// read without applying the save. `exists` is false when the slot file is
// absent or unreadable; the other fields are then meaningless.
struct SlotInfo {
    bool        exists    = false;
    int         version   = 0;
    int64_t     timestamp = 0;          // unix seconds, 0 if absent
    std::string label;                  // full timestamped title
    std::string base;                   // last_docked_base
    std::string system;                 // current_system
    std::string ship;                   // ship_class_name
    int64_t     credits   = 0;
    std::string path;                   // absolute file path (for load())
};
SlotInfo peek(int slot);

// Read metadata for an explicit save file path (load-menu rows).
SlotInfo peek_path(const std::string& path);

// Every save file in the saves dir, newest-first (by embedded timestamp).
// Scans save_*.json; corrupt/unreadable files are skipped. Each entry's
// `path` feeds load(p, path). Unbounded — reflects the full accumulation.
std::vector<SlotInfo> list_saves();

} // namespace savegame
