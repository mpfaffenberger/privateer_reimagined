#include "faction.h"

#include <algorithm>
#include <cstdio>

Stance g_faction_stance[kFactionCount][kFactionCount] = {};
int8_t g_faction_baseline_to_player[kFactionCount] = {};

namespace {

// Symmetric stance defaults — the matrix Mike approved in design review.
// Reading order: H = hostile, N = neutral, A = allied. Diagonal is A
// (faction always allied to itself; wingmen rely on this).
//
//             Civ  Mer  Cnf  Mil  Hnt  Pir  Ret  Kil
constexpr char k_stance_grid[kFactionCount][kFactionCount] = {
    /* Civ */ { 'A', 'N', 'N', 'N', 'N', 'H', 'H', 'H' },
    /* Mer */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Cnf */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Mil */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Hnt */ { 'N', 'N', 'N', 'N', 'A', 'H', 'H', 'H' },
    /* Pir */ { 'H', 'H', 'H', 'H', 'H', 'A', 'N', 'H' },
    /* Ret */ { 'H', 'H', 'H', 'H', 'H', 'N', 'A', 'H' },
    /* Kil */ { 'H', 'H', 'H', 'H', 'H', 'H', 'H', 'A' },
};

constexpr Stance char_to_stance(char c) {
    return (c == 'A') ? Stance::Allied
         : (c == 'H') ? Stance::Hostile
         :              Stance::Neutral;
}

// Faction baseline opinion of a stranger (no rep accumulated yet). Confed
// stay professional (0). Pirates assume you're prey (-30). Kilrathi treat
// any non-Kilrathi as the enemy (-100); even max rep won't make them allies.
constexpr int8_t k_baseline[kFactionCount] = {
    /* Civ      */ +10,
    /* Merchant */ +10,
    /* Confed   */   0,
    /* Militia  */   0,
    /* Hunter   */   0,
    /* Pirate   */ -30,
    /* Retro    */ -50,
    /* Kilrathi */ -100,
};

constexpr const char* k_names[kFactionCount] = {
    "civilian", "merchant", "confed", "militia",
    "hunter", "pirate", "retro", "kilrathi",
};

bool g_initialised = false;

} // namespace

void faction::init() {
    if (g_initialised) return;
    g_initialised = true;

    int hostile_pairs = 0;
    int allied_pairs  = 0;
    for (int a = 0; a < kFactionCount; ++a) {
        for (int b = 0; b < kFactionCount; ++b) {
            const Stance st = char_to_stance(k_stance_grid[a][b]);
            g_faction_stance[a][b] = st;
            // Count each unordered pair once (a < b) for the log line.
            if (a < b) {
                if (st == Stance::Hostile) ++hostile_pairs;
                if (st == Stance::Allied)  ++allied_pairs;
            }
        }
        g_faction_baseline_to_player[a] = k_baseline[a];
    }

    std::printf("[faction] %d factions, %d hostile pairs, %d allied pairs\n",
                kFactionCount, hostile_pairs, allied_pairs);
}

Faction faction::from_name(std::string_view s) {
    for (int i = 0; i < kFactionCount; ++i) {
        if (s == k_names[i]) return (Faction)i;
    }
    return Faction::Count;  // sentinel "unknown"
}

const char* faction::to_name(Faction f) {
    if ((int)f < 0 || (int)f >= kFactionCount) return "?";
    return k_names[(int)f];
}

float faction::default_skill_f2(Faction f) {
    switch (f) {
        case Faction::Confed:   return 60.0f;   // disciplined navy: aces
        case Faction::Kilrathi: return 58.0f;   // elite warriors
        case Faction::Hunter:   return 55.0f;   // seasoned bounty hunters
        case Faction::Militia:  return 50.0f;   // competent veterans
        case Faction::Pirate:   return 45.0f;   // scrappy, uneven
        case Faction::Retro:    return 42.0f;   // fanatics, poorly trained
        case Faction::Merchant: return 40.0f;   // green
        case Faction::Civilian: return 40.0f;   // green
        default:                return 45.0f;
    }
}

Stance faction::stance_npc_vs_npc(Faction a, Faction b) {
    return g_faction_stance[(int)a][(int)b];
}

Stance faction::stance_npc_vs_player(Faction npc, const PlayerReputation& r) {
    const int eff = std::clamp(
        (int)g_faction_baseline_to_player[(int)npc] + (int)r.rep[(int)npc],
        -100, +100);
    if (eff <= -25) return Stance::Hostile;
    if (eff >= +25) return Stance::Allied;
    return Stance::Neutral;
}

namespace {

// Reputation deltas (points on the [-100,+100] scale) for ONE player kill.
// Tuned so a short run of kills crosses the +/-25 stance thresholds in
// faction.h: ~5 favourable kills (5 x +5 = +25) flips a faction to Allied;
// two unprovoked murders (2 x -15 = -30) flips the victim's faction to
// Hostile from neutral. These are gameplay-tuning knobs, not facts, hence
// named constants.
constexpr int k_kill_outlaw_victim_penalty = -3;   // killed an outlaw: their own faction mildly annoyed
constexpr int k_kill_outlaw_enemy_bonus    = +5;   // an outlaw's enemies approve (public service)
constexpr int k_kill_lawful_victim_penalty = -15;  // murdered the lawful: their faction furious
constexpr int k_kill_lawful_ally_penalty   = -8;   // the victim's allies hear of the crime
constexpr int k_kill_lawful_enemy_bonus    = +2;   // the victim's enemies are mildly amused

// Derived (not hardcoded): factions that distrust strangers on sight
// (negative baseline-to-player) are the universe's outlaws. Killing one
// is policing; killing a lawful faction member is a crime.
bool is_outlaw_faction(Faction f) {
    return g_faction_baseline_to_player[(int)f] < 0;
}

int8_t clamp_rep(int v) {
    return (int8_t)std::clamp(v, -100, 100);
}

} // namespace

std::vector<RepKillEffect> faction::apply_player_kill(PlayerReputation& rep,
                                                      Faction victim) {
    std::vector<RepKillEffect> effects;
    if ((int)victim < 0 || (int)victim >= kFactionCount) return effects;

    const bool victim_outlaw = is_outlaw_faction(victim);

    for (int i = 0; i < kFactionCount; ++i) {
        const Faction f = (Faction)i;
        int          delta    = 0;
        KillReaction reaction = KillReaction::None;

        if (f == victim) {
            // A faction always resents losing one of its own. Outlaws
            // grumble (no taunt); the lawful are enraged.
            delta    = victim_outlaw ? k_kill_outlaw_victim_penalty
                                     : k_kill_lawful_victim_penalty;
            reaction = victim_outlaw ? KillReaction::None
                                     : KillReaction::Anger;
        } else {
            const Stance st = g_faction_stance[i][(int)victim];
            if (st == Stance::Hostile) {
                // f hates the victim, so the player did f a favour.
                delta    = victim_outlaw ? k_kill_outlaw_enemy_bonus
                                         : k_kill_lawful_enemy_bonus;
                reaction = KillReaction::Praise;
            } else if (st == Stance::Allied && !victim_outlaw) {
                // f is the lawful victim's ally and files a police
                // report. An OUTLAW's "allies" are other outlaws who
                // don't, so they're skipped (delta stays 0).
                delta    = k_kill_lawful_ally_penalty;
                reaction = KillReaction::Anger;
            }
            // Neutral -> no change.
        }

        if (delta == 0) continue;

        RepKillEffect e;
        e.faction       = f;
        e.before        = rep.rep[i];
        e.stance_before = stance_npc_vs_player(f, rep);
        rep.rep[i]      = clamp_rep((int)rep.rep[i] + delta);
        e.after         = rep.rep[i];
        e.stance_after  = stance_npc_vs_player(f, rep);
        e.reaction      = reaction;
        effects.push_back(e);
    }
    return effects;
}
