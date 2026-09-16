#include "battle.hpp"
#include <iostream>
#include <random>
#include <algorithm>
#include <limits>
#include <unordered_map>

namespace {

std::mt19937& rng() {
    static std::mt19937 engine(std::random_device{}());
    return engine;
}

double roll01() {
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng());
}

int roll_damage(int min_dmg, int max_dmg) {
    std::uniform_int_distribution<int> dist(min_dmg, max_dmg);
    return dist(rng());
}

double sum_hp(const std::vector<UnitInstance>& fleet) {
    double total = 0.0;
    for (const auto& u : fleet) total += u.current_hp;
    return total;
}

int count_alive(const std::vector<UnitInstance>& fleet) {
    int n = 0;
    for (const auto& u : fleet) if (u.alive) ++n;
    return n;
}

double avg_initiative(const std::vector<UnitInstance>& fleet) {
    int n = 0;
    double total = 0.0;
    for (const auto& u : fleet) {
        if (!u.alive) continue;
        total += u.unit_class->initiative;
        ++n;
    }
    return n > 0 ? total / n : 0.0;
}

double avg_maneuverability(const std::vector<UnitInstance>& fleet) {
    int n = 0;
    double total = 0.0;
    for (const auto& u : fleet) {
        if (!u.alive) continue;
        total += u.unit_class->maneuverability;
        ++n;
    }
    return n > 0 ? total / n : 0.0;
}

// Shared shape for both escape rolls: base chance, nudged by relative
// initiative and the retreating side's own maneuverability, then clamped.
// Tune base/scale freely — this is a first pass, not a balanced formula.
bool escape_roll(const SideState& self, const SideState& enemy, double base_chance) {
    double init_self = avg_initiative(*self.fleet);
    double init_enemy = avg_initiative(*enemy.fleet);
    double maneuver_self = avg_maneuverability(*self.fleet);

    double chance = base_chance
                  + 0.05 * (init_self - init_enemy)
                  + 0.25 * maneuver_self;
    chance = std::clamp(chance, 0.05, 0.90);

    return roll01() < chance;
}

std::string read_line_trimmed() {
    std::string line;
    std::getline(std::cin, line);
    size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = line.find_last_not_of(" \t\r\n");
    return line.substr(a, b - a + 1);
}

// --- combat math ---

// hit_chance = accuracy * (1 - maneuverability * (1 - tracking)), then clamped.
// An ace's accuracy_bonus applies to the attacker; an ace's evasion_bonus adds
// to the target's effective maneuverability.
double compute_hit_chance(const WeaponData& weapon, const UnitInstance& attacker, const UnitInstance& target) {
    double accuracy = weapon.accuracy + (attacker.ace.has_value() ? attacker.ace->accuracy_bonus : 0.0);

    double maneuverability = target.unit_class->maneuverability
                            + (target.ace.has_value() ? target.ace->evasion_bonus : 0.0);
    maneuverability = std::clamp(maneuverability, 0.0, 1.0);

    double chance = accuracy * (1.0 - maneuverability * (1.0 - weapon.tracking));
    return std::clamp(chance, 0.02, 0.95);
}

// Banded penetration check: penetration/hardness ratio >= 1.0 -> full damage,
// >= 0.6 -> a 35% "graze", otherwise the shot bounces for zero. Hardness <= 0
// (fighters, most planes) always takes full damage regardless of penetration.
double compute_penetration_multiplier(const WeaponData& weapon, const UnitClassData& target_cls) {
    if (target_cls.hardness <= 0) return 1.0;
    double ratio = static_cast<double>(weapon.penetration) / target_cls.hardness;
    if (ratio >= 1.0) return 1.0;
    if (ratio >= 0.6) return 0.35;
    return 0.0;
}

// Weighted target pick: candidates are alive enemy units the weapon can
// legally target; weight comes from the attacker's target_priorities for
// that candidate's type. Priorities are weights, not probabilities — they
// are normalised here over only the types actually present, so an absent
// type never eats probability mass. If none of the present candidate types
// have a priority entry, falls back to a uniform pick among them.
UnitInstance* select_target(const UnitClassData& attacker_cls, const WeaponData& weapon,
                             std::vector<UnitInstance>& enemy_fleet) {
    std::vector<UnitInstance*> candidates;
    for (auto& u : enemy_fleet)
        if (u.alive && weapon.can_target(u.unit_class->type))
            candidates.push_back(&u);

    if (candidates.empty()) return nullptr;

    std::vector<double> weights;
    weights.reserve(candidates.size());
    double total = 0.0;
    for (auto* c : candidates) {
        double w = 0.0;
        auto it = attacker_cls.target_priorities.find(c->unit_class->type);
        if (it != attacker_cls.target_priorities.end()) w = it->second;
        weights.push_back(w);
        total += w;
    }

    if (total <= 0.0) {
        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
        return candidates[dist(rng())];
    }

    double r = roll01() * total;
    double cumulative = 0.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        cumulative += weights[i];
        if (r <= cumulative) return candidates[i];
    }
    return candidates.back(); // floating-point fallback
}

// Attempts to redirect a hit onto an escort. Only escorts on the SAME side as
// the original target are eligible, whose protects list covers the target's
// type, and who haven't hit their per-round intercept cap. Ties/multiple
// eligible escorts resolve highest escort-chance first. Escort hits ignore
// hardness entirely (full damage), matching the earlier design decision that
// escorting is a sacrifice, not a free damage sponge.
bool try_escort_intercept(UnitInstance& original_target, std::vector<UnitInstance>& target_side_fleet,
                           UnitInstance** actual_target) {
    std::vector<UnitInstance*> escorts;
    for (auto& u : target_side_fleet) {
        if (!u.alive || &u == &original_target) continue;
        if (!u.unit_class->escort.has_value()) continue;
        const auto& esc = *u.unit_class->escort;
        if (u.intercepts_used_this_round >= esc.max_intercepts_per_round) continue;

        bool protects_type = false;
        for (auto t : esc.protects) if (t == original_target.unit_class->type) { protects_type = true; break; }
        if (!protects_type) continue;

        escorts.push_back(&u);
    }

    if (escorts.empty()) { *actual_target = &original_target; return false; }

    std::sort(escorts.begin(), escorts.end(), [](UnitInstance* a, UnitInstance* b) {
        return a->unit_class->escort->chance > b->unit_class->escort->chance;
    });

    for (auto* esc_unit : escorts) {
        if (roll01() < esc_unit->unit_class->escort->chance) {
            esc_unit->intercepts_used_this_round += 1;
            *actual_target = esc_unit;
            return true;
        }
    }

    *actual_target = &original_target;
    return false;
}

} // namespace

RetreatPolicy prompt_retreat_policy(const std::string& side_label) {
    while (true) {
        std::cout << "\nRetreat policy for " << side_label << ":\n"
                  << "  1) Avoid battle   - attempt to disengage before the fight starts\n"
                  << "  2) Low damage     - retreat once losses exceed ~15%\n"
                  << "  3) Medium damage  - retreat once losses exceed ~35%\n"
                  << "  4) High damage    - retreat once losses exceed ~60%\n"
                  << "  5) No retreat     - fight to the last unit\n"
                  << "Choice [1-5]: ";

        std::string input = read_line_trimmed();
        if (input == "1") return RetreatPolicy::AvoidBattle;
        if (input == "2") return RetreatPolicy::LowDamage;
        if (input == "3") return RetreatPolicy::MediumDamage;
        if (input == "4") return RetreatPolicy::HighDamage;
        if (input == "5") return RetreatPolicy::NoRetreat;

        std::cout << "Not a valid choice, try again.\n";
    }
}

double retreat_threshold_fraction(RetreatPolicy p) {
    switch (p) {
        case RetreatPolicy::LowDamage:    return 0.15;
        case RetreatPolicy::MediumDamage: return 0.35;
        case RetreatPolicy::HighDamage:   return 0.60;
        case RetreatPolicy::NoRetreat:    return std::numeric_limits<double>::infinity();
        case RetreatPolicy::AvoidBattle:  return 0.0; // handled separately, pre-battle
    }
    return std::numeric_limits<double>::infinity();
}

double fraction_hp_lost(const SideState& side) {
    if (side.starting_total_hp <= 0.0) return 0.0;
    double remaining = sum_hp(*side.fleet);
    return 1.0 - (remaining / side.starting_total_hp);
}

bool attempt_disengagement(const SideState& self, const SideState& enemy) {
    return escape_roll(self, enemy, 0.55);
}

bool attempt_retreat_roll(const SideState& self, const SideState& enemy) {
    return escape_roll(self, enemy, 0.35);
}

// Full round resolution: each side's living units fire every weapon they
// have (subject to ammo) at a weighted-random target of a valid type. Hits
// are checked for escort interception, then run through the penetration
// check. All damage is queued and applied at the end of the round rather
// than as each shot lands, so units killed partway through still get to
// fire this round (matches the "simultaneous_fire" rule we discussed).
void resolve_round(std::vector<UnitInstance>& aggressor, std::vector<UnitInstance>& defender, int /*round_number*/) {
    for (auto& u : aggressor) { u.intercepts_used_this_round = 0; u.damage_taken_this_round = 0; }
    for (auto& u : defender)  { u.intercepts_used_this_round = 0; u.damage_taken_this_round = 0; }

    std::vector<UnitInstance*> aggressor_shooters;
    for (auto& u : aggressor) if (u.alive) aggressor_shooters.push_back(&u);
    std::vector<UnitInstance*> defender_shooters;
    for (auto& u : defender) if (u.alive) defender_shooters.push_back(&u);

    std::unordered_map<UnitInstance*, int> pending_damage;

    auto fire_side = [&](std::vector<UnitInstance*>& shooters, std::vector<UnitInstance>& enemy_fleet) {
        for (auto* attacker : shooters) {
            for (const auto& weapon : attacker->unit_class->weapons) {
                int* ammo_ptr = nullptr;
                auto ammo_it = attacker->ammo_remaining.find(weapon.id);
                if (ammo_it != attacker->ammo_remaining.end()) ammo_ptr = &ammo_it->second;

                for (int shot = 0; shot < weapon.shots; ++shot) {
                    if (ammo_ptr && *ammo_ptr <= 0) break;

                    UnitInstance* primary_target = select_target(*attacker->unit_class, weapon, enemy_fleet);
                    if (!primary_target) break; // nothing this weapon can legally hit right now

                    if (ammo_ptr) --(*ammo_ptr);

                    double hit_chance = compute_hit_chance(weapon, *attacker, *primary_target);
                    if (roll01() >= hit_chance) continue; // miss, ammo still spent

                    UnitInstance* actual_target = primary_target;
                    bool intercepted = try_escort_intercept(*primary_target, enemy_fleet, &actual_target);

                    int damage_roll = roll_damage(weapon.damage_min, weapon.damage_max);
                    double multiplier = intercepted ? 1.0 : compute_penetration_multiplier(weapon, *actual_target->unit_class);
                    int final_damage = static_cast<int>(damage_roll * multiplier);

                    pending_damage[actual_target] += final_damage;
                }
            }
        }
    };

    fire_side(aggressor_shooters, defender);
    fire_side(defender_shooters, aggressor);

    for (auto& [target, dmg] : pending_damage) {
        target->damage_taken_this_round += dmg;
        target->current_hp -= dmg;
        if (target->current_hp <= 0) {
            target->current_hp = 0;
            target->alive = false;
        }
    }
}

namespace {

void print_side_status(const SideState& side) {
    int alive = count_alive(*side.fleet);
    double remaining_hp = sum_hp(*side.fleet);
    double loss_pct = fraction_hp_lost(side) * 100.0;
    std::cout << "  " << side.label << ": " << alive << " units alive, "
              << remaining_hp << " HP remaining (" << loss_pct << "% losses)\n";
}

bool ask_continue() {
    std::cout << "\nContinue to next round? [Y/n/q]: ";
    std::string input = read_line_trimmed();
    if (input.empty()) return true;
    char c = static_cast<char>(std::tolower(input[0]));
    return c != 'n' && c != 'q';
}

} // namespace

BattleOutcome run_battle(
    std::vector<UnitInstance>& aggressor,
    std::vector<UnitInstance>& defender,
    RetreatPolicy aggressor_policy,
    RetreatPolicy defender_policy,
    int max_rounds)
{
    SideState aggr{"Aggressor", &aggressor, aggressor_policy, sum_hp(aggressor), false};
    SideState def {"Defender",  &defender,  defender_policy,  sum_hp(defender),  false};

    bool aggr_avoided = false;
    bool def_avoided = false;

    if (aggr.policy == RetreatPolicy::AvoidBattle) {
        bool success = attempt_disengagement(aggr, def);
        std::cout << "\n" << aggr.label << " attempts to avoid battle... "
                  << (success ? "SUCCESS - disengaged before contact.\n" : "FAILED - committed to the fight.\n");
        if (success) aggr_avoided = true;
        else aggr.policy = RetreatPolicy::LowDamage;
    }
    if (def.policy == RetreatPolicy::AvoidBattle) {
        bool success = attempt_disengagement(def, aggr);
        std::cout << def.label << " attempts to avoid battle... "
                  << (success ? "SUCCESS - disengaged before contact.\n" : "FAILED - committed to the fight.\n");
        if (success) def_avoided = true;
        else def.policy = RetreatPolicy::LowDamage;
    }

    if (aggr_avoided && def_avoided) return BattleOutcome::BothAvoidedBattle;
    if (aggr_avoided) return BattleOutcome::AggressorAvoidedBattle;
    if (def_avoided) return BattleOutcome::DefenderAvoidedBattle;

    for (int round = 1; round <= max_rounds; ++round) {
        resolve_round(aggressor, defender, round);

        std::cout << "\n=== Round " << round << " results ===\n";
        print_side_status(aggr);
        print_side_status(def);

        bool aggr_dead = count_alive(aggressor) == 0;
        bool def_dead = count_alive(defender) == 0;
        if (aggr_dead && def_dead) return BattleOutcome::MutualAnnihilation;
        if (aggr_dead) return BattleOutcome::DefenderExterminatedAggressor;
        if (def_dead) return BattleOutcome::AggressorExterminatedDefender;

        if (!aggr.withdrawn && fraction_hp_lost(aggr) >= retreat_threshold_fraction(aggr.policy)) {
            bool success = attempt_retreat_roll(aggr, def);
            std::cout << aggr.label << " attempts to retreat... " << (success ? "SUCCESS\n" : "FAILED, still engaged\n");
            if (success) aggr.withdrawn = true;
        }
        if (!def.withdrawn && fraction_hp_lost(def) >= retreat_threshold_fraction(def.policy)) {
            bool success = attempt_retreat_roll(def, aggr);
            std::cout << def.label << " attempts to retreat... " << (success ? "SUCCESS\n" : "FAILED, still engaged\n");
            if (success) def.withdrawn = true;
        }

        if (aggr.withdrawn && def.withdrawn) return BattleOutcome::BothWithdrew;
        if (aggr.withdrawn) return BattleOutcome::AggressorWithdrew;
        if (def.withdrawn) return BattleOutcome::DefenderWithdrew;

        if (round < max_rounds && !ask_continue())
            return BattleOutcome::AbortedByUser;
    }

    return BattleOutcome::StalemateMaxRounds;
}

void print_outcome(BattleOutcome outcome) {
    std::cout << "\n=== BATTLE OUTCOME ===\n";
    switch (outcome) {
        case BattleOutcome::AggressorExterminatedDefender:
            std::cout << "Aggressor wins - Defender fleet annihilated.\n"; break;
        case BattleOutcome::DefenderExterminatedAggressor:
            std::cout << "Defender wins - Aggressor fleet annihilated.\n"; break;
        case BattleOutcome::MutualAnnihilation:
            std::cout << "Mutual annihilation - no survivors on either side.\n"; break;
        case BattleOutcome::AggressorWithdrew:
            std::cout << "Aggressor withdrew - Defender holds the field.\n"; break;
        case BattleOutcome::DefenderWithdrew:
            std::cout << "Defender withdrew - Aggressor holds the field.\n"; break;
        case BattleOutcome::BothWithdrew:
            std::cout << "Both sides withdrew - inconclusive engagement.\n"; break;
        case BattleOutcome::AggressorAvoidedBattle:
            std::cout << "Aggressor avoided battle entirely - no combat occurred.\n"; break;
        case BattleOutcome::DefenderAvoidedBattle:
            std::cout << "Defender avoided battle entirely - no combat occurred.\n"; break;
        case BattleOutcome::BothAvoidedBattle:
            std::cout << "Both sides avoided battle - no engagement took place.\n"; break;
        case BattleOutcome::AbortedByUser:
            std::cout << "Battle aborted by user before a conclusion was reached.\n"; break;
        case BattleOutcome::StalemateMaxRounds:
            std::cout << "Stalemate - max round count reached with no decisive result.\n"; break;
    }
}
