// -----------------------------------------------------------------------------
// campaign.cpp — story-campaign mission logic (see campaign.h).
// -----------------------------------------------------------------------------

#include "campaign.h"

#include "comm.h"
#include "faction.h"
#include "player.h"
#include "plot.h"
#include "ship_class.h"

#include <cstdio>

namespace campaign {
namespace {

// Does `base_id` refer to this base, tolerating the nav-data type suffix
// ("liverpool_refinery" vs the bare folder id "liverpool")? Prefix rule
// mirrors base_screens' folder resolver.
bool base_is(const std::string& base_id, const char* bare) {
    const std::string b(bare);
    if (base_id == b) return true;
    return base_id.size() > b.size() && base_id.rfind(b + "_", 0) == 0;
}

// ---- consignment missions (M01 #113, M02-M05 #114-#117) --------------------
// Every cargo-run plot mission is one row: accept token loads the goods,
// docking at `dest_base` settles (pay + delivered flag), docking anywhere
// with the consignment missing fails the run (active flag cleared -> the
// fixer re-offers). Chain milestones between missions are fixers.json
// done_actions, not rows here.
struct CargoMission {
    const char* token;        // accept token, e.g. "m02"
    const char* commodity;    // catalog id
    int         units;
    const char* dest_base;    // bare base id (base_is-matched)
    int64_t     payout;       // credits on delivery (0 = fixer settles later)
    const char* accept_line;  // comm feed on accept
    const char* deliver_line; // comm feed on delivery
    const char* extra_accept_flag;   // optional flag set on accept ("" = none)
    const char* extra_deliver_flag;  // optional flag set on delivery ("" = none)
    bool        stow_in_compartment; // M05: hide the goods from scans
};

constexpr CargoMission k_cargo_missions[] = {
    // M01 Sandoval (#113): payment is the artifact, settled at the bar.
    { "m01", "iron", 40, "liverpool", 0,
      "40 units of iron loaded. Destination: Liverpool, Newcastle system.",
      "Iron delivered. Sandoval promised payment back on New Detroit.",
      "", "", false },
    // M02 Tayla 1 (#114): plastics to Oakham, 10k on landing. Accept also
    // marks the player as Tayla's (tayla_employed -> pirate neutrality).
    { "m02", "plastics", 30, "oakham", 10000,
      "30 units of plastics loaded. Destination: Oakham, Pentonville system.",
      "Plastics delivered. 10,000 credits from Tayla's man on Oakham.",
      // No return leg / no debrief for M02: the chain milestone lands
      // WITH the payout (issue #114: "tayla_1_done, Tayla relocates").
      "tayla_employed", "tayla_1_done", false },
    // M03 Tayla 2 (#115): first Brilliance run, Hector (Troy). The Troy
    // militia heat is a scripted scenario gated on m03_active.
    { "m03", "brilliance", 15, "hector", 15000,
      "15 units of Brilliance aboard. Destination: Hector, Troy system. Fly casual.",
      "Brilliance delivered at Hector. 15,000 credits. Now get back to Oakham.",
      "", "", false },
    // M04 Tayla 3 (#116): Brilliance to New Constantinople. Confed pickets
    // are scripted scenarios gated on m04_active.
    { "m04", "brilliance", 25, "new_constantinople", 20000,
      "25 units of Brilliance aboard. Destination: New Constantinople. Tayla swears the patrols are bribed.",
      "Brilliance delivered. 20,000 credits. Tayla wants a word back at Oakham.",
      "", "", false },
    // M05 Tayla 4 (#117): final run, 20 units - exactly the secret
    // compartment's capacity. Riordian is scripted_encounters.json's job.
    { "m05", "brilliance", 20, "new_constantinople", 10000,
      "20 units of Brilliance stowed. Destination: New Constantinople. Watch your back.",
      "Final delivery made. 10,000 credits. Tayla is waiting at Oakham.",
      "", "", true },
};

std::string flag_active(const CargoMission& m)    { return std::string(m.token) + "_active"; }
std::string flag_delivered(const CargoMission& m) { return std::string(m.token) + "_delivered"; }

// Accept a consignment: load the goods (hold-space checked - a full hold
// refuses WITHOUT setting the active flag, so the offer stays on the
// table), set <token>_active + any extra flag.
void cargo_accept(const CargoMission& m, PlayerState& p) {
    bool loaded;
    if (m.stow_in_compartment && plot::has_item(p, "secret_compartment")) {
        loaded = player::add_compartment_cargo(p, m.commodity, m.units);
    } else {
        const ShipClass* klass = ship_class::find(p.ship_class_name);
        const int capacity = player::cargo_capacity(p, klass);
        loaded = player::add_cargo(p, m.commodity, m.units, /*price*/0, capacity);
    }
    if (!loaded) {
        comm::push("'Come back when you have room for the goods.'", true);
        std::printf("[campaign] %s accept refused: no space for %d %s\n",
                    m.token, m.units, m.commodity);
        return;
    }
    plot::set_flag(p, flag_active(m));
    if (m.extra_accept_flag[0]) plot::set_flag(p, m.extra_accept_flag);
    comm::push(m.accept_line, false);
    std::printf("[campaign] %s accepted: %d %s aboard%s\n",
                m.token, m.units, m.commodity,
                m.stow_in_compartment ? " (compartment)" : "");
}

// Dock checks for one consignment mission. Delivery at the destination;
// failure ANYWHERE the consignment turns up missing (sold/jettisoned) -
// the active flag clears and the fixer re-offers (vanilla retry policy).
void cargo_on_dock(const CargoMission& m, PlayerState& p,
                   const std::string& base_id) {
    if (!plot::has_flag(p, flag_active(m))) return;

    if (base_is(base_id, m.dest_base)) {
        if (player::remove_cargo(p, m.commodity, m.units)) {
            plot::clear_flag(p, flag_active(m));
            plot::set_flag(p, flag_delivered(m));
            if (m.extra_deliver_flag[0]) plot::set_flag(p, m.extra_deliver_flag);
            if (m.payout > 0) player::add_credits(p, m.payout);
            comm::push(m.deliver_line, false);
            std::printf("[campaign] %s delivered at %s (+%lld cr)\n",
                        m.token, base_id.c_str(), (long long)m.payout);
        } else {
            plot::clear_flag(p, flag_active(m));
            comm::push("The consignment is gone. The job is blown.", false);
            std::printf("[campaign] %s FAILED at %s (consignment missing)\n",
                        m.token, base_id.c_str());
        }
        return;
    }

    // Off-destination dock: count what's aboard (hidden stacks included -
    // remove_cargo doesn't care, we only need presence here). Short on
    // goods = the player disposed of plot cargo -> fail + re-offer.
    int aboard = 0;
    for (const CargoEntry& e : p.cargo)
        if (e.commodity_id == m.commodity) aboard += e.units;
    if (aboard < m.units) {
        plot::clear_flag(p, flag_active(m));
        comm::push("The consignment is gone. The job is blown.", false);
        std::printf("[campaign] %s FAILED at %s (consignment missing)\n",
                    m.token, base_id.c_str());
    }
}

// ---- M04 reward: the secret compartment (#116) ------------------------------
void m04_install_compartment(PlayerState& p) {
    if (!plot::give_item(p, "secret_compartment")) return;   // idempotent
    comm::push("Secret compartment installed: 20 units, invisible to scans. "
               "Contraband only.", false);
    std::printf("[campaign] secret compartment installed\n");
}

// ---- the one campaign action handler ---------------------------------------
bool handle_action(const std::string& action, PlayerState& p) {
    for (const CargoMission& m : k_cargo_missions) {
        if (action == std::string(m.token) + ":accept") {
            cargo_accept(m, p);
            return true;
        }
    }
    if (action == "m04:install_compartment") {
        m04_install_compartment(p);
        return true;
    }
    return false;   // not a campaign token — plot logs it
}

} // namespace

void init() {
    plot::set_action_handler(handle_action);
    std::printf("[campaign] action handler registered\n");
}

void on_dock(PlayerState& p, const std::string& base_id) {
    for (const CargoMission& m : k_cargo_missions)
        cargo_on_dock(m, p, base_id);
}

void tick(const PlayerState& p, const std::string& system_id) {
    // Tayla's protection (#114): Pentonville pirates read the player as
    // NEUTRAL while under her employ. Re-derived every frame from plot
    // state so save/load, chain completion, and system changes all just
    // work; the override is transient by design (faction.h).
    const bool employed = plot::has_flag(p, "tayla_employed") &&
                          !plot::has_flag(p, "tayla_done");
    const bool in_region = system_id == "pentonville";
    if (employed && in_region) {
        if (!faction::player_stance_override_active(Faction::Pirate)) {
            faction::set_player_stance_override(Faction::Pirate,
                                                Stance::Neutral);
            std::printf("[campaign] pirate-neutrality override ON (Tayla, %s)\n",
                        system_id.c_str());
        }
    } else if (faction::player_stance_override_active(Faction::Pirate)) {
        faction::clear_player_stance_override(Faction::Pirate);
        std::printf("[campaign] pirate-neutrality override OFF\n");
    }
}

} // namespace campaign
