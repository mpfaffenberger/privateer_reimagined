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

struct PlayerState;

namespace savegame {

// Bumped whenever the on-disk shape changes incompatibly. v1 = the initial
// PlayerState serialization; v2 added the accepted-missions list (np-zte.1);
// v3 added missile inventory + afterburner fuel (np-zte.2). load() tolerates
// older saves by defaulting the new keys (missiles -> 0, fuel -> full tank),
// so a v1/v2 save keeps working — the version bump just records the addition.
constexpr int k_format_version = 3;

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
// `label` (base name / credits) for the future load menu.
bool save(const PlayerState& p, int slot);

// Deserialize slot `slot` into `p` (overwriting it wholesale on success).
// Returns false — leaving `p` untouched — if the file is missing, corrupt,
// or from an unsupported format version. Never throws.
bool load(PlayerState& p, int slot);

// Lightweight slot metadata for a load menu / "last autosave" readout,
// read without applying the save. `exists` is false when the slot file is
// absent or unreadable; the other fields are then meaningless.
struct SlotInfo {
    bool        exists    = false;
    int         version   = 0;
    int64_t     timestamp = 0;          // unix seconds, 0 if absent
    std::string label;                  // "Achilles - 2000 cr"
    std::string base;                   // last_docked_base
    std::string system;                 // current_system
    int64_t     credits   = 0;
};
SlotInfo peek(int slot);

} // namespace savegame
