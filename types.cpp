#include "types.hpp"
#include <stdexcept>

namespace {

const std::unordered_map<std::string, UnitType>& name_to_type() {
    static const std::unordered_map<std::string, UnitType> m = {
        {"striker", UnitType::Striker}, {"fighter", UnitType::Fighter}, {"mecha", UnitType::Mecha},
        {"frigate", UnitType::Frigate}, {"destroyer", UnitType::Destroyer}, {"cruiser", UnitType::Cruiser},
        {"battleship", UnitType::Battleship}, {"carrier", UnitType::Carrier}, {"transport", UnitType::Transport}
    };
    return m;
}

const std::unordered_map<std::string, std::vector<UnitType>>& groups() {
    static const std::unordered_map<std::string, std::vector<UnitType>> g = {
        {"small_craft", {UnitType::Striker, UnitType::Fighter, UnitType::Mecha}},
        {"planes",      {UnitType::Striker, UnitType::Fighter}},
        {"mecha_only",  {UnitType::Mecha}},
        {"capitals",    {UnitType::Frigate, UnitType::Destroyer, UnitType::Cruiser,
                          UnitType::Battleship, UnitType::Carrier, UnitType::Transport}},
        {"everything",  {UnitType::Striker, UnitType::Fighter, UnitType::Mecha, UnitType::Frigate,
                          UnitType::Destroyer, UnitType::Cruiser, UnitType::Battleship,
                          UnitType::Carrier, UnitType::Transport}}
    };
    return g;
}

} // namespace

UnitType unit_type_from_string(const std::string& s) {
    auto& m = name_to_type();
    auto it = m.find(s);
    if (it == m.end())
        throw std::runtime_error("Unknown unit type: " + s);
    return it->second;
}

std::string unit_type_to_string(UnitType t) {
    for (auto& [name, type] : name_to_type())
        if (type == t) return name;
    return "unknown";
}

const std::vector<UnitType>& resolve_type_group(const std::string& group_name) {
    auto& g = groups();
    auto it = g.find(group_name);
    if (it == g.end())
        throw std::runtime_error("Unknown type group: " + group_name);
    return it->second;
}
