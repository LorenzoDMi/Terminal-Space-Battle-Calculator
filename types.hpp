#pragma once
#include <string>
#include <unordered_map>
#include <vector>

enum class UnitType {
    Striker, Fighter, Mecha, Frigate, Destroyer, Cruiser, Battleship, Carrier, Transport, Unknown
};

UnitType unit_type_from_string(const std::string& s);
std::string unit_type_to_string(UnitType t);

// Named groups of types.
// Weapons reference a group by name.
const std::vector<UnitType>& resolve_type_group(const std::string& group_name);
