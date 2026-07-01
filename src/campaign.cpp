// -----------------------------------------------------------------------------
// campaign.cpp — story-campaign mission logic (see campaign.h).
// -----------------------------------------------------------------------------

#include "campaign.h"

#include "comm.h"
#include "player.h"
#include "plot.h"
#include "ship_class.h"

#include <cstdio>

namespace campaign {
namespace {

// ---- M01 Sandoval (#113) ----------------------------------------------------
constexpr int   k_m01_units = 40;          // iron consignment size
constexpr char  k_m01_commodity[] = "iron";

// Does `base_id` refer to this base, tolerating the nav-data type suffix
// ("liverpool_refinery" vs the bare folder id "liverpool")? Prefix rule
// mirrors base_screens' folder resolver.
bool base_is(const std::string& base_id, const char* bare) {
    const std::string b(bare);
    if (base_id == b) return true;
    return base_id.size() > b.size() && base_id.rfind(b + "_", 0) == 0;
}

// "m01:accept" — load the consignment. Hold-space checked: on a full hold
// we refuse WITHOUT setting m01_active, so Sandoval's offer stays on the
// table (vanilla lets you come back after clearing space).
void m01_accept(PlayerState& p) {
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const int capacity = player::cargo_capacity(p, klass);
    if (!player::add_cargo(p, k_m01_commodity, k_m01_units,
                           /*price*/0, capacity)) {
        comm::push("Sandoval: 'Come back when you have room for the iron.'",
                   true);
        std::printf("[campaign] m01 accept refused: no hold space for %d iron\n",
                    k_m01_units);
        return;
    }
    plot::set_flag(p, "m01_active");
    comm::push("40 units of iron loaded. Destination: Liverpool, Newcastle system.",
               false);
    std::printf("[campaign] m01 accepted: %d iron aboard\n", k_m01_units);
}

// Dock checks for M01: delivery at Liverpool, or failure detection when
// the consignment is gone (sold/jettisoned) at the delivery dock.
void m01_on_dock(PlayerState& p, const std::string& base_id) {
    if (!plot::has_flag(p, "m01_active")) return;
    if (!base_is(base_id, "liverpool")) return;

    if (player::remove_cargo(p, k_m01_commodity, k_m01_units)) {
        plot::clear_flag(p, "m01_active");
        plot::set_flag(p, "m01_delivered");
        comm::push("Iron delivered. Sandoval promised payment back on New Detroit.",
                   false);
        std::printf("[campaign] m01 delivered at %s\n", base_id.c_str());
    } else {
        // The player disposed of plot cargo. Fail the run; the offer
        // re-appears at New Detroit (m01_active is the only gate).
        plot::clear_flag(p, "m01_active");
        comm::push("The iron consignment is gone. Sandoval's job is blown.",
                   false);
        std::printf("[campaign] m01 FAILED at %s (consignment missing)\n",
                    base_id.c_str());
    }
}

// ---- the one campaign action handler ---------------------------------------
bool handle_action(const std::string& action, PlayerState& p) {
    if (action == "m01:accept") { m01_accept(p); return true; }
    return false;   // not a campaign token — plot logs it
}

} // namespace

void init() {
    plot::set_action_handler(handle_action);
    std::printf("[campaign] action handler registered\n");
}

void on_dock(PlayerState& p, const std::string& base_id) {
    m01_on_dock(p, base_id);
}

} // namespace campaign
