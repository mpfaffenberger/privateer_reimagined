// -----------------------------------------------------------------------------
// ship_registry.cpp — slot-map implementation.
//
// Mechanically boring on purpose (see header for the design rationale).
// The only subtlety worth flagging: despawn() resets the Ship to a
// default-constructed value INSTEAD of leaving the corpse in the slot.
// That releases vector capacity (mounts, perception contacts) promptly
// and guarantees a recycled slot never leaks the previous occupant's
// state into the next spawn.
// -----------------------------------------------------------------------------

#include "ship_registry.h"

#include <cstdio>

ShipHandle ShipRegistry::spawn(Ship&& s) {
    uint32_t idx;
    if (!free_indices_.empty()) {
        // LIFO reuse: pop the most-recently-freed index off the back of
        // the free-list. (Not lowest-index-first — the order only affects
        // which vacated slot a new spawn lands in, never correctness.)
        idx = free_indices_.back();
        free_indices_.pop_back();
    } else {
        idx = (uint32_t)slots_.size();
        slots_.emplace_back();   // deque growth: existing Ship&s stay valid
    }
    Slot& slot   = slots_[idx];
    slot.ship     = std::move(s);
    slot.occupied = true;
    return ShipHandle{ idx, slot.generation };
}

bool ShipRegistry::despawn(ShipHandle h) {
    Ship* s = get(h);
    if (!s) {
        // Stale or invalid — loud in the log, harmless to the caller.
        std::printf("[ship_registry] despawn ignored: stale handle {%u, %u}\n",
                    h.index, h.generation);
        return false;
    }
    Slot& slot    = slots_[h.index];
    slot.ship     = Ship{};      // drop mounts/perception storage now
    slot.occupied = false;
    slot.generation++;           // every outstanding handle goes stale
    free_indices_.push_back(h.index);
    return true;
}

size_t ShipRegistry::clear_except_player() {
    // Slot 0 is the player by the documented spawn-first convention; keep it
    // untouched (ship, generation, occupied) so the player's handle stays
    // valid across the switch. Everything else is reset to a husk + its
    // generation bumped (staleness contract) and its index returned to the
    // free-list. We rebuild free_indices_ from scratch rather than appending
    // so a prior partial free-list can't double-list a slot.
    size_t cleared = 0;
    free_indices_.clear();
    for (size_t i = 1; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.occupied) {
            slot.ship     = Ship{};
            slot.occupied = false;
            slot.generation++;
            ++cleared;
        }
        free_indices_.push_back((uint32_t)i);
    }
    return cleared;
}

Ship* ShipRegistry::get(ShipHandle h) {
    return const_cast<Ship*>(static_cast<const ShipRegistry*>(this)->get(h));
}

const Ship* ShipRegistry::get(ShipHandle h) const {
    if (!h.valid())                 return nullptr;
    if (h.index >= slots_.size())   return nullptr;
    const Slot& slot = slots_[h.index];
    if (!slot.occupied)             return nullptr;
    if (slot.generation != h.generation) return nullptr;   // stale
    return &slot.ship;
}

ShipHandle ShipRegistry::find_handle_by_id(uint32_t id) const {
    if (id == 0) return {};   // 0 is the reserved "none" id
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].occupied && slots_[i].ship.id == id) {
            return ShipHandle{ (uint32_t)i, slots_[i].generation };
        }
    }
    return {};
}

Ship* ShipRegistry::find_by_id(uint32_t id) {
    return get(find_handle_by_id(id));
}

const Ship* ShipRegistry::find_by_id(uint32_t id) const {
    return get(find_handle_by_id(id));
}

size_t ShipRegistry::size() const {
    return slots_.size() - free_indices_.size();
}

size_t ShipRegistry::alive_count() const {
    size_t n = 0;
    for (const Slot& s : slots_) {
        if (s.occupied && s.ship.alive) ++n;
    }
    return n;
}

Ship* ShipRegistry::ship_at(size_t slot_index) {
    if (slot_index >= slots_.size() || !slots_[slot_index].occupied) return nullptr;
    return &slots_[slot_index].ship;
}

const Ship* ShipRegistry::ship_at(size_t slot_index) const {
    if (slot_index >= slots_.size() || !slots_[slot_index].occupied) return nullptr;
    return &slots_[slot_index].ship;
}
