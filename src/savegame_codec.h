#pragma once
// -----------------------------------------------------------------------------
// savegame_codec.h — INTERNAL seam of the savegame module (#525).
//
//   savegame.cpp        files + slots + the public API (savegame.h)
//   savegame_write.cpp  PlayerState -> save document  (encode)
//   savegame_read.cpp   save document -> PlayerState  (decode, peek)
//
// Callers outside the module include savegame.h, never this. Each field group
// (loadout, hold, missions, ordnance, ...) has one writer + one reader, so a
// format bump touches small, separate hunks in the two codec files.
// -----------------------------------------------------------------------------

#include "json.h"
#include "player.h"
#include "savegame.h"

#include <string>

namespace savegame::codec {

// On-disk key per missile rack slot, indexed like PlayerState::missiles.
// A persistence contract: append new types, never rename or reorder.
inline constexpr const char* k_missile_save_keys[k_missile_rack_types] = {
    "df", "hs", "ir", "ff"
};

// The complete save document for `p`: version + timestamp + label + player.
std::string encode(const PlayerState& p);

// Decode a parsed save document into `out`. Returns false (logged, tagged with
// `slot`) on a missing/unsupported version or a missing player object. May
// throw on a malformed-but-parseable document; load() owns the catch.
bool decode(const json::Value& root, int slot, PlayerState& out);

// Load-menu metadata from a parsed save document; leaves `path` empty and
// sets `exists` iff the version is valid. May throw; peek_path() catches.
SlotInfo peek(const json::Value& root);

} // namespace savegame::codec
