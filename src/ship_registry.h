#pragma once
// -----------------------------------------------------------------------------
// ship_registry.h — slot-map storage for Ships (handle -> index).
//
// Replaces the startup-only `std::vector<Ship>` whose comment promised
// "if/when dynamic spawning lands, this needs to be a slot-map". It
// landed. This is that slot-map — and nothing more. No ECS, no archetype
// tables, no component streams; one container, one job: own every Ship
// in the world and hand out handles that can outlive the Ship safely.
//
// Why handles instead of pointers or indices?
//   * A raw Ship* dangles the moment storage reallocates or a slot is
//     reused. An index silently aliases: despawn ship 7, spawn a new
//     one into slot 7, and every stale index-7 reference now points at
//     a stranger. A handle pairs the index with a per-slot GENERATION
//     counter that bumps on every despawn — get() on a stale handle
//     compares generations, misses, and returns nullptr. Bugs become
//     loud nullptrs instead of quiet body-snatchers.
//
// Layout: a std::deque of slots + a free-list of vacated indices.
//   * deque, not vector, on purpose: deque growth NEVER invalidates
//     pointers/references to existing elements (only insertion in the
//     middle does, and we never do that). Code that holds a Ship& for
//     the duration of a frame keeps working even if a debug button
//     spawns a ship mid-frame.
//   * Iteration walks every slot and skips unoccupied ones. No packing,
//     no swap-with-back compaction — at this game's ship count (tens)
//     the skip test is free, and stable slots are what keep handles
//     and Ship::id-based AI targeting simple.
//   * Generations start at 1 so the all-zeros handle {0, 0} can serve
//     as the universal "invalid" sentinel — it never matches slot 0,
//     whose generation is never 0.
//
// Player convention: the player is spawned FIRST at startup and
// therefore always lives in slot 0 (the free-list is empty at that
// point, and the player is never despawned). player() encodes that
// convention in one place; callers that used to write
// `ships.front().is_player ? ...` now ask the registry instead.
//
// Ship::id (the monotonic uint32 minted by ship::spawn) is unchanged
// and remains the currency of AI targeting + perception contacts —
// those systems were already id-based and id lookups already handled
// "target gone" gracefully. find_by_id stays a linear scan, same as
// the one ship_ai.cpp had; not worth a map until ships number in the
// hundreds.
// -----------------------------------------------------------------------------

#include "ship.h"

#include <cstdint>
#include <deque>
#include <vector>

struct ShipHandle {
    uint32_t index      = 0;
    uint32_t generation = 0;   // 0 = never-valid sentinel (slots start at 1)

    bool valid() const { return generation != 0; }
    bool operator==(const ShipHandle& o) const {
        return index == o.index && generation == o.generation;
    }
};

class ShipRegistry {
public:
    struct Slot {
        Ship     ship;
        uint32_t generation = 1;   // bumped on despawn; 0 reserved for "invalid"
        bool     occupied   = false;
    };

    // Take ownership of a Ship. Reuses a free slot if one exists (LIFO:
    // most-recently-freed), else appends. Returns the handle that
    // resolves to this Ship until it's despawned.
    ShipHandle spawn(Ship&& s);

    // Vacate a slot. The Ship is reset to a default-constructed husk
    // (drops gun mounts / perception vectors immediately rather than
    // hoarding them until slot reuse) and the generation bumps so every
    // outstanding handle to this slot goes stale. Returns false on an
    // already-stale or invalid handle — callers may treat that as a
    // no-op, double-despawn is harmless by design.
    bool despawn(ShipHandle h);

    // Wipe every occupant EXCEPT the player (slot 0), bumping each cleared
    // slot's generation so outstanding handles go stale, and rebuilding the
    // free-list so the next spawns refill the vacated slots. The teardown
    // half of a system switch (np-6al.1): the player persists across the
    // jump, every NPC / encounter ship is forgotten. Returns the number of
    // ships cleared (for the unload log). Safe to call with no player
    // spawned yet — it just clears everything in that case.
    size_t clear_except_player();

    // Resolve a handle. nullptr when the handle is invalid, out of
    // range, stale (generation mismatch), or the slot is free. The ONLY
    // way to reach a Ship from a stored handle — keeps the staleness
    // check unskippable.
    Ship*       get(ShipHandle h);
    const Ship* get(ShipHandle h) const;

    // Ship::id -> handle / Ship*. Linear scan over occupied slots (see
    // header note). Returns the invalid handle / nullptr when no alive-
    // or-dead occupant carries that id.
    ShipHandle  find_handle_by_id(uint32_t id) const;
    Ship*       find_by_id(uint32_t id);
    const Ship* find_by_id(uint32_t id) const;

    // The player, by the slot-0 convention documented above. nullptr
    // until the player has been spawned (startup-only window).
    Ship*       player()       { return get(player_handle()); }
    const Ship* player() const { return get(player_handle()); }
    static constexpr ShipHandle player_handle() { return ShipHandle{ 0, 1 }; }

    // Counts. size() = occupied slots (alive or dead — a killed ship
    // still occupies its slot so explosions/HUD can read its corpse);
    // alive_count() additionally requires ship.alive. slot_count() is
    // the raw slot-array length, for index-parallel scratch buffers
    // (damage-pass snapshots in main.cpp) — index with ship_at().
    size_t size() const;
    size_t alive_count() const;
    size_t slot_count() const { return slots_.size(); }

    // Slot-indexed access for those scratch-buffer loops. nullptr for
    // free slots and out-of-range indices. NOT a handle resolve — no
    // generation check — so don't store these indices across despawns.
    Ship*       ship_at(size_t slot_index);
    const Ship* ship_at(size_t slot_index) const;

    // ---- iteration ------------------------------------------------------
    // Range-for over occupied slots: `for (Ship& s : registry)`. The
    // iterator pair below is the entire reason most call sites migrated
    // from std::vector<Ship> by changing only their parameter type.
    template <typename SlotDeque, typename ShipT>
    class iter {
    public:
        iter(SlotDeque* slots, size_t i) : slots_(slots), i_(i) { skip_free(); }
        ShipT& operator*()  const { return (*slots_)[i_].ship; }
        ShipT* operator->() const { return &(*slots_)[i_].ship; }
        iter&  operator++()       { ++i_; skip_free(); return *this; }
        bool   operator!=(const iter& o) const { return i_ != o.i_; }
        bool   operator==(const iter& o) const { return i_ == o.i_; }
    private:
        void skip_free() {
            while (i_ < slots_->size() && !(*slots_)[i_].occupied) ++i_;
        }
        SlotDeque* slots_;
        size_t     i_;
    };
    using iterator       = iter<std::deque<Slot>, Ship>;
    using const_iterator = iter<const std::deque<Slot>, const Ship>;

    iterator       begin()       { return {&slots_, 0}; }
    iterator       end()         { return {&slots_, slots_.size()}; }
    const_iterator begin() const { return {&slots_, 0}; }
    const_iterator end()   const { return {&slots_, slots_.size()}; }

private:
    std::deque<Slot>      slots_;        // deque: pointer-stable growth
    std::vector<uint32_t> free_indices_; // vacated slots, reused LIFO (back)
};
