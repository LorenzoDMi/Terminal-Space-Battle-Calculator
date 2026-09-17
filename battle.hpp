#pragma once
#include <vector>
#include <string>
#include "fleet.hpp"

enum class RetreatPolicy {
    AvoidBattle,   // try to escape before round 1 even starts (ambush scenario)
    LowDamage,     // retreat attempt once losses cross ~15% of starting HP
    MediumDamage,  // ~35%
    HighDamage,    // ~60%
    NoRetreat      // fight to the last unit, no retreat attempts ever
};

enum class BattleOutcome {
    AggressorExterminatedDefender,
    DefenderExterminatedAggressor,
    MutualAnnihilation,
    AggressorWithdrew,
    DefenderWithdrew,
    BothWithdrew,
    AggressorAvoidedBattle,   // escaped before round 1; no combat happened
    DefenderAvoidedBattle,
    BothAvoidedBattle,
    AbortedByUser,            // user answered "no" to the continue prompt
    StalemateMaxRounds
};

struct SideState {
    std::string label;
    std::vector<UnitInstance>* fleet = nullptr;
    RetreatPolicy policy;
    double starting_total_hp = 0.0;
    bool withdrawn = false;
};

// Terminal prompt for one side, retried until a valid 1-5 choice is entered.
RetreatPolicy prompt_retreat_policy(const std::string& side_label);

// Fraction of starting HP that must be lost before a retreat roll is attempted.
// AvoidBattle isn't a damage threshold at all — see attempt_disengagement.
double retreat_threshold_fraction(RetreatPolicy p);

double fraction_hp_lost(const SideState& side);

// Pre-battle escape attempt for a side using AvoidBattle. Weighted by relative
// initiative and maneuverability of living units on both sides. On failure the
// side is committed to the fight and its policy downgrades to LowDamage.
bool attempt_disengagement(const SideState& self, const SideState& enemy);

// Mid-battle retreat attempt once a side's damage threshold has been crossed.
// Same spirit as attempt_disengagement but a lower base success chance, since
// breaking contact mid-fight is harder than never engaging at all.
bool attempt_retreat_roll(const SideState& self, const SideState& enemy);

// PLACEHOLDER. The actual weapon-fire / hit / penetration resolution for one
// round is not implemented yet — this is where it goes. It must mutate
// current_hp and alive on the UnitInstances in both fleets. Everything else
// in this file works regardless of what this function ends up doing.
void resolve_round(std::vector<UnitInstance>& aggressor, std::vector<UnitInstance>& defender, int round_number);

// Runs the full battle: resolves the AvoidBattle case up front, then loops
// round by round calling resolve_round, printing a summary, checking for
// extermination or a successful retreat, and pausing to ask the user whether
// to continue. Returns the final outcome.
BattleOutcome run_battle(
    std::vector<UnitInstance>& aggressor,
    std::vector<UnitInstance>& defender,
    RetreatPolicy aggressor_policy,
    RetreatPolicy defender_policy,
    int max_rounds = 20
);

void print_outcome(BattleOutcome outcome);

// Lists every unit still alive at the end of the battle, with its current
// and max HP, and a note if it's mid-rearm when the battle ended. Call once
// per side, after print_outcome.
void print_survivors(const std::vector<UnitInstance>& fleet, const char* side_label);

// How many rounds a rearm cycle takes once a plane/mecha docks. Single knob.
constexpr int kRearmDurationRounds = 2;

// Chance a docked plane/mecha goes down with its carrier instead of being
// safely ejected, when the carrier is destroyed mid-rearm.
constexpr double kHangarCasualtyChance = 0.4;

// Scans one side's fleet: advances anyone currently docked (redeploying them
// once their timer hits zero; if their host carrier died first, rolls a
// hangar-casualty check — kHangarCasualtyChance odds they're destroyed
// along with it, otherwise they're ejected still out of ammo), then tries
// to dock any plane/mecha whose antiship weapons have just run dry at a
// carrier-capable ship on the same side with a free bay. A carrier's usable
// capacity scales down with its own current HP, and a destroyed carrier
// offers none. Prints one line per dock/redeploy/eject/casualty event. Call
// once per side, once per round — after resolve_round, so this round's
// ammo use and carrier losses are already reflected.
void process_rearming(std::vector<UnitInstance>& fleet, const char* side_label);