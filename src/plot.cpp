// -----------------------------------------------------------------------------
// plot.cpp — campaign plot flags + plot items (see plot.h).
//
// Storage is the two flat vectors on PlayerState. Linear scans everywhere:
// the campaign tops out at a few dozen flags, so a sorted structure would
// be complexity without payoff, and insertion order is nice to read in a
// save file ("the story so far, in order").
// -----------------------------------------------------------------------------

#include "plot.h"
#include "player.h"

#include <algorithm>

namespace plot {
namespace {

std::function<void(const std::string&)> g_observer;

void notify(const char* verb, std::string_view what) {
    if (!g_observer) return;
    std::string msg(verb);
    msg += what;
    g_observer(msg);
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Shared implementation for both lists — flags and items have identical
// storage semantics, only their verbs differ.
bool add_unique(std::vector<std::string>& v, std::string_view s,
                const char* verb) {
    if (s.empty() || contains(v, s)) return false;
    v.emplace_back(s);
    notify(verb, s);
    return true;
}

bool erase_one(std::vector<std::string>& v, std::string_view s,
               const char* verb) {
    auto it = std::find(v.begin(), v.end(), s);
    if (it == v.end()) return false;
    v.erase(it);
    notify(verb, s);
    return true;
}

} // namespace

bool has_flag(const PlayerState& p, std::string_view flag) {
    return contains(p.plot_flags, flag);
}

bool set_flag(PlayerState& p, std::string_view flag) {
    return add_unique(p.plot_flags, flag, "flag set: ");
}

bool clear_flag(PlayerState& p, std::string_view flag) {
    return erase_one(p.plot_flags, flag, "flag cleared: ");
}

bool has_item(const PlayerState& p, std::string_view id) {
    return contains(p.plot_items, id);
}

bool give_item(PlayerState& p, std::string_view id) {
    return add_unique(p.plot_items, id, "item granted: ");
}

bool remove_item(PlayerState& p, std::string_view id) {
    return erase_one(p.plot_items, id, "item removed: ");
}

void set_observer(std::function<void(const std::string&)> fn) {
    g_observer = std::move(fn);
}

} // namespace plot
