#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include "unit_data.hpp"

enum class Side { Aggressor, Defender };

// Aces don't exist as game content yet — this is deliberately the smallest
// possible struct so it's cheap to extend once the design is settled.
struct AceData {
    std::string ace_id;
    std::string name;
    double accuracy_bonus = 0.0;
    double evasion_bonus = 0.0;
    int hp_bonus = 0;
};

// One deployed unit in a specific battle. Points at its immutable class
// template rather than copying weapons/priorities per instance.
struct UnitInstance {
    int instance_id = -1;
    const UnitClassData* unit_class = nullptr; // never owns; UnitDatabase must outlive the battle
    Side side = Side::Aggressor;

    int current_hp = 0;
    bool alive = true;

    std::unordered_map<std::string, int> ammo_remaining; // weapon_id -> shots left (finite-ammo weapons only)
    int intercepts_used_this_round = 0;
    int damage_taken_this_round = 0;

    std::optional<AceData> ace; // present only if this instance was tagged with ace=<id>
};
