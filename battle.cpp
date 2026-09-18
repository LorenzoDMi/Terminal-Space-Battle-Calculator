#include "battle.hpp"
#include <iostream>
#include <random>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace {

// ANSI colors for the round-by-round salvo log only. Never used in
// print_outcome/print_survivors (the final battle result).
constexpr const char* kColorReset  = "\033[0m";
constexpr const char* kColorYellow = "\033[33m"; // an ace is attacking or defending
constexpr const char* kColorRed    = "\033[31m"; // a unit was destroyed this line

// Prints one turn-log line, colored red if it records a destruction, else
// yellow if an ace was involved (attacking or defending), else plain.
// Red takes priority over yellow when both apply.
void print_log_line(const std::string& text, bool ace_involved, bool destroyed) {
    if (destroyed)      std::cout << kColorRed    << text << kColorReset << "\n";
    else if (ace_involved) std::cout << kColorYellow << text << kColorReset << "\n";
    else                 std::cout << text << "\n";
}

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

double compute_hit_chance(const WeaponData& weapon, const UnitInstance& attacker, const UnitInstance& target) {
    double accuracy = weapon.accuracy + (attacker.ace.has_value() ? attacker.ace->accuracy_bonus : 0.0);

    double maneuverability = target.unit_class->maneuverability
                            + (target.ace.has_value() ? target.ace->evasion_bonus : 0.0);
    maneuverability = std::clamp(maneuverability, 0.0, 1.0);

    double chance = accuracy * (1.0 - maneuverability * (1.0 - weapon.tracking));
    return std::clamp(chance, 0.02, 0.95);
}

// Banded penetration check bundled with a human readable label, since both
// the combat math and the salvo log need the same classification.
struct PenetrationResult {
    double multiplier;
    const char* label;
};

PenetrationResult compute_penetration(const WeaponData& weapon, const UnitClassData& target_cls) {
    if (target_cls.hardness <= 0) return {1.0, "no armor"};
    double ratio = static_cast<double>(weapon.penetration) / target_cls.hardness;
    if (ratio >= 1.0) return {1.0, "penetrated"};
    if (ratio >= 0.6) return {0.35, "grazed"};
    return {0.0, "bounced"};
}

UnitInstance* select_target(const UnitClassData& attacker_cls, const WeaponData& weapon,
                             std::vector<UnitInstance>& enemy_fleet) {
    std::vector<UnitInstance*> candidates;
    for (auto& u : enemy_fleet)
        if (u.alive && u.rearm_status == RearmStatus::Available && weapon.can_target(u.unit_class->type))
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
    return candidates.back();
}

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

// "ClassName #id" plus an ace tag when the instance carries one, so aces are
// always visually distinct in the salvo log.
std::string format_unit_label(const UnitInstance& u) {
    std::string label = u.unit_class->name + " #" + std::to_string(u.instance_id);
    if (u.ace.has_value()) label += " (Ace: " + u.ace->name + ")";
    return label;
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
        case RetreatPolicy::AvoidBattle:  return 0.0;
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

// Resolves one round and PRINTS a line for every shot fired, in the order it
// happened: attacker, weapon, target, and the result. Damage is queued in
// pending_damage and only applied to current_hp/alive at the very end (after
// both sides have fully fired), so a unit destroyed partway through this
// function still gets to take its own shots this round. Because a unit can
// only ever be shot at by the opposing side within a single round, the
// running total in pending_damage during a given side's fire is already the
// true post-shot HP for logging purposes — that's what drives the
// [DESTROYED] tag appearing on the actual killing blow rather than at the
// end of the round.
void resolve_round(std::vector<UnitInstance>& aggressor, std::vector<UnitInstance>& defender, int /*round_number*/) {
    for (auto& u : aggressor) { u.intercepts_used_this_round = 0; u.damage_taken_this_round = 0; }
    for (auto& u : defender)  { u.intercepts_used_this_round = 0; u.damage_taken_this_round = 0; }

    std::vector<UnitInstance*> aggressor_shooters;
    for (auto& u : aggressor) if (u.alive && u.rearm_status == RearmStatus::Available) aggressor_shooters.push_back(&u);
    std::vector<UnitInstance*> defender_shooters;
    for (auto& u : defender) if (u.alive && u.rearm_status == RearmStatus::Available) defender_shooters.push_back(&u);

    // Firing order within a side is randomised each round (a unit's own shots
    // still all fire together) so it isn't always instance #0, #1, #2... —
    // the aggressor-then-defender side ordering itself is untouched.
    std::shuffle(aggressor_shooters.begin(), aggressor_shooters.end(), rng());
    std::shuffle(defender_shooters.begin(), defender_shooters.end(), rng());

    std::unordered_map<UnitInstance*, int> pending_damage;
    std::unordered_set<UnitInstance*> destroyed_flagged;

    auto fire_side = [&](std::vector<UnitInstance*>& shooters, std::vector<UnitInstance>& enemy_fleet,
                          const char* attacker_label, const char* defender_label) {
        for (auto* attacker : shooters) {
            for (const auto& weapon : attacker->unit_class->weapons) {
                int* ammo_ptr = nullptr;
                auto ammo_it = attacker->ammo_remaining.find(weapon.id);
                if (ammo_it != attacker->ammo_remaining.end()) ammo_ptr = &ammo_it->second;

                for (int shot = 0; shot < weapon.shots; ++shot) {
                    if (ammo_ptr && *ammo_ptr <= 0) break;

                    UnitInstance* primary_target = select_target(*attacker->unit_class, weapon, enemy_fleet);
                    if (!primary_target) break;

                    if (ammo_ptr) --(*ammo_ptr);

                    std::string line = "  [" + std::string(attacker_label) + "] " + format_unit_label(*attacker)
                                      + " -> " + weapon.name + " -> ["
                                      + std::string(defender_label) + "] " + format_unit_label(*primary_target);

                    double hit_chance = compute_hit_chance(weapon, *attacker, *primary_target);
                    if (roll01() >= hit_chance) {
                        bool ace_involved = attacker->ace.has_value() || primary_target->ace.has_value();
                        print_log_line(line + ": MISS", ace_involved, false);
                        continue;
                    }

                    UnitInstance* actual_target = primary_target;
                    bool intercepted = try_escort_intercept(*primary_target, enemy_fleet, &actual_target);

                    int damage_roll = roll_damage(weapon.damage_min, weapon.damage_max);
                    double multiplier;
                    std::string band_label;
                    if (intercepted) {
                        multiplier = 1.0;
                        band_label = "escort intercept, hardness ignored";
                        line += " (intercepted by " + format_unit_label(*actual_target) + ")";
                    } else {
                        PenetrationResult pen = compute_penetration(weapon, *actual_target->unit_class);
                        multiplier = pen.multiplier;
                        band_label = pen.label;
                    }
                    int final_damage = static_cast<int>(damage_roll * multiplier);

                    pending_damage[actual_target] += final_damage;

                    bool destroyed_now = false;
                    double estimated_remaining = actual_target->current_hp - pending_damage[actual_target];
                    if (estimated_remaining <= 0 && destroyed_flagged.insert(actual_target).second)
                        destroyed_now = true;

                    bool ace_involved = attacker->ace.has_value() || primary_target->ace.has_value()
                                      || actual_target->ace.has_value();
                    line += ": HIT " + std::to_string(final_damage) + " dmg (" + band_label + ")"
                          + (destroyed_now ? " [DESTROYED]" : "");
                    print_log_line(line, ace_involved, destroyed_now);
                }
            }
        }
    };

    fire_side(aggressor_shooters, defender, "Aggressor", "Defender");
    fire_side(defender_shooters, aggressor, "Defender", "Aggressor");

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

// True only if this unit has at least one antiship weapon with finite ammo,
// and every such weapon is currently at zero. A unit with no antiship
// weapon, or one whose antiship weapon has unlimited ammo, never triggers a
// rearm cycle.
bool antiship_ammo_depleted(const UnitInstance& u) {
    bool has_finite_antiship = false;
    for (const auto& w : u.unit_class->weapons) {
        if (w.category != "antiship" || !w.ammo.has_value()) continue;
        has_finite_antiship = true;
        auto it = u.ammo_remaining.find(w.id);
        int remaining = (it != u.ammo_remaining.end()) ? it->second : 0;
        if (remaining > 0) return false;
    }
    return has_finite_antiship;
}

// Finds the carrier-capable ship on this fleet with the most free bays right
// now. Capacity scales with the carrier's own damage; a destroyed carrier
// (filtered by u.alive) is never a candidate at all.
UnitInstance* find_available_carrier(std::vector<UnitInstance>& fleet) {
    UnitInstance* best = nullptr;
    int best_free = 0;

    for (auto& carrier : fleet) {
        if (!carrier.alive || carrier.unit_class->rearm_capacity <= 0) continue;

        int effective_capacity = static_cast<int>(std::floor(
            carrier.unit_class->rearm_capacity *
            (carrier.current_hp / static_cast<double>(carrier.unit_class->hp))));

        int occupied = 0;
        for (auto& docked : fleet)
            if (docked.rearm_status == RearmStatus::Rearming && docked.docked_carrier == &carrier)
                ++occupied;

        int free_slots = effective_capacity - occupied;
        if (free_slots > best_free) {
            best_free = free_slots;
            best = &carrier;
        }
    }

    return best_free > 0 ? best : nullptr;
}

} // namespace

void process_rearming(std::vector<UnitInstance>& fleet, const char* side_label) {
    // Advance anyone already docked: eject if their carrier died, otherwise
    // tick their timer down and redeploy once it hits zero.
    for (auto& u : fleet) {
        if (u.rearm_status != RearmStatus::Rearming) continue;

        if (!u.docked_carrier->alive) {
            bool killed = roll01() < kHangarCasualtyChance;
            bool ace_involved = u.ace.has_value() || u.docked_carrier->ace.has_value();
            if (killed) {
                std::string msg = "  [" + std::string(side_label) + "][REARM] " + format_unit_label(u)
                                 + " destroyed in the hangar - host carrier "
                                 + format_unit_label(*u.docked_carrier) + " was lost";
                print_log_line(msg, ace_involved, true);
                u.alive = false;
                u.current_hp = 0;
            } else {
                std::string msg = "  [" + std::string(side_label) + "][REARM] " + format_unit_label(u)
                                 + " - host carrier destroyed, ejected mid-rearm (still out of antiship ammo)";
                print_log_line(msg, ace_involved, false);
            }
            u.rearm_status = RearmStatus::Available;
            u.docked_carrier = nullptr;
            u.rearm_turns_remaining = 0;
            continue;
        }

        u.rearm_turns_remaining -= 1;
        if (u.rearm_turns_remaining <= 0) {
            for (const auto& w : u.unit_class->weapons)
                if (w.ammo.has_value())
                    u.ammo_remaining[w.id] = *w.ammo;

            std::cout << "  [" << side_label << "][REARM] " << format_unit_label(u)
                      << " finishes rearming aboard " << format_unit_label(*u.docked_carrier)
                      << " and redeploys, full ammo restored\n";

            u.rearm_status = RearmStatus::Available;
            u.docked_carrier = nullptr;
            u.rearm_turns_remaining = 0;
        }
    }

    // Try to dock anyone newly out of antiship ammo.
    for (auto& u : fleet) {
        if (!u.alive || u.rearm_status != RearmStatus::Available) continue;
        if (u.unit_class->type != UnitType::Striker &&
            u.unit_class->type != UnitType::Fighter &&
            u.unit_class->type != UnitType::Mecha) continue;
        if (!antiship_ammo_depleted(u)) continue;

        UnitInstance* carrier = find_available_carrier(fleet);
        if (!carrier) continue; // no room anywhere right now; it keeps fighting with what's left

        u.rearm_status = RearmStatus::Rearming;
        u.docked_carrier = carrier;
        u.rearm_turns_remaining = kRearmDurationRounds;

        std::cout << "  [" << side_label << "][REARM] " << format_unit_label(u)
                  << " docks with " << format_unit_label(*carrier)
                  << " to rearm (ETA " << kRearmDurationRounds << " turns)\n";
    }
}

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
        std::cout << "\n================ TURN " << round << " ================\n";
        resolve_round(aggressor, defender, round);

        process_rearming(aggressor, "Aggressor");
        process_rearming(defender, "Defender");

        std::cout << "\n--- Turn " << round << " summary ---\n";
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

void print_survivors(const std::vector<UnitInstance>& fleet, const char* side_label) {
    std::cout << "\n" << side_label << " surviving assets:\n";
    bool any = false;
    for (const auto& u : fleet) {
        if (!u.alive) continue;
        any = true;
        std::cout << "  " << format_unit_label(u) << ": " << u.current_hp << "/" << u.unit_class->hp << " HP";
        if (u.rearm_status == RearmStatus::Rearming)
            std::cout << " (rearming, " << u.rearm_turns_remaining << " turns left)";
        std::cout << "\n";
    }
    if (!any) std::cout << "  (none)\n";
}