#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include "types.hpp"

// One weapon mounted on a unit class. Pure data — no combat math lives here.
struct WeaponData {
    std::string id;
    std::string name;
    std::string category;        // "antiship" | "aa" | "anti_mecha"
    int damage_min = 0;
    int damage_max = 0;
    int penetration = 0;
    double accuracy = 0.0;
    double tracking = 0.0;
    int shots = 1;
    std::optional<int> ammo;     // nullopt = unlimited

    // valid_targets in the JSON is either a group name ("capitals") or an explicit
    // array of type names. Exactly one of these two is populated after parsing.
    bool uses_group = true;
    std::string target_group;             // set if uses_group
    std::vector<UnitType> explicit_targets; // set if !uses_group

    bool can_target(UnitType t) const {
        if (uses_group) {
            for (auto candidate : resolve_type_group(target_group))
                if (candidate == t) return true;
            return false;
        }
        for (auto candidate : explicit_targets)
            if (candidate == t) return true;
        return false;
    }
};

struct EscortData {
    double chance = 0.0;
    int max_intercepts_per_round = 0;
    std::vector<UnitType> protects;
};

// Immutable template loaded once from JSON. Many UnitInstances point at the same
// UnitClassData — it is never copied per-instance.
struct UnitClassData {
    std::string class_id;
    std::string name;
    UnitType type = UnitType::Unknown;
    int point_cost = 0;
    int hp = 0;
    int hardness = 0;
    double maneuverability = 0.0;
    int initiative = 0;
    std::vector<WeaponData> weapons;
    std::unordered_map<UnitType, double> target_priorities; // weights, normalised at runtime
    std::optional<EscortData> escort;
};

class UnitDatabase {
public:
    void load_from_file(const std::string& json_path);
    const UnitClassData& get(const std::string& class_id) const;
    bool contains(const std::string& class_id) const;

private:
    std::unordered_map<std::string, UnitClassData> classes_;
};
