#include "unit_data.hpp"
#include "fleet_loader.hpp"
#include "battle.hpp"
#include <iostream>

// After combat resolves, print a line for every ace whose unit didn't survive.
void report_ace_losses(const std::vector<UnitInstance>& fleet) {
    for (const auto& u : fleet) {
        if (u.ace.has_value() && !u.alive) {
            std::cout << "[ACE LOST] " << u.ace->name
                      << " (" << u.unit_class->name << ", instance #"
                      << u.instance_id << ")\n";
        }
    }
}

int main() {
    UnitDatabase units;
    units.load_from_file("fleet_data.json");

    AceDatabase aces;
    aces.load_from_file("aces.json");

    int next_id = 0;
    auto aggressor = load_fleet("aggressor.txt", units, &aces, Side::Aggressor, next_id);
    auto defender  = load_fleet("defender.txt",  units, &aces, Side::Defender,  next_id);

    std::cout << "Aggressor: " << aggressor.size() << " units\n";
    std::cout << "Defender:  " << defender.size()  << " units\n";

    RetreatPolicy aggressor_policy = prompt_retreat_policy("Aggressor");
    RetreatPolicy defender_policy  = prompt_retreat_policy("Defender");

    BattleOutcome outcome = run_battle(aggressor, defender, aggressor_policy, defender_policy);

    print_outcome(outcome);
    print_survivors(aggressor, "Aggressor");
    print_survivors(defender, "Defender");
    report_ace_losses(aggressor);
    report_ace_losses(defender);

    return 0;
}
