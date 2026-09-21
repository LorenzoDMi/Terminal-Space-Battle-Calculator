#include "unit_data.hpp"
#include <json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {

WeaponData parse_weapon(const json& j) {
    WeaponData w;
    w.id = j.at("id").get<std::string>();
    w.name = j.at("name").get<std::string>();
    w.category = j.at("category").get<std::string>();
    w.damage_min = j.at("damage_min").get<int>();
    w.damage_max = j.at("damage_max").get<int>();
    w.penetration = j.at("penetration").get<int>();
    w.accuracy = j.at("accuracy").get<double>();
    w.tracking = j.at("tracking").get<double>();
    w.shots = j.at("shots").get<int>();

    if (j.contains("ammo") && !j.at("ammo").is_null())
        w.ammo = j.at("ammo").get<int>();

    const auto& vt = j.at("valid_targets");
    if (vt.is_string()) {
        w.uses_group = true;
        w.target_group = vt.get<std::string>();
    } else if (vt.is_array()) {
        w.uses_group = false;
        for (const auto& t : vt)
            w.explicit_targets.push_back(unit_type_from_string(t.get<std::string>()));
    } else {
        throw std::runtime_error("valid_targets must be a string or array for weapon " + w.id);
    }
    return w;
}

std::optional<EscortData> parse_escort(const json& j) {
    if (!j.is_object()) return std::nullopt; // null in JSON -> no escort capability
    EscortData e;
    e.chance = j.at("chance").get<double>();
    e.max_intercepts_per_round = j.at("max_intercepts_per_round").get<int>();
    for (const auto& t : j.at("protects"))
        e.protects.push_back(unit_type_from_string(t.get<std::string>()));
    return e;
}

UnitClassData parse_unit_class(const std::string& id, const json& j) {
    UnitClassData u;
    u.class_id = id;
    u.name = j.at("name").get<std::string>();
    u.type = unit_type_from_string(j.at("type").get<std::string>());
    u.point_cost = j.at("point_cost").get<int>();
    u.hp = j.at("hp").get<int>();
    u.hardness = j.at("hardness").get<int>();
    u.maneuverability = j.at("maneuverability").get<double>();
    u.initiative = j.at("initiative").get<int>();

    for (const auto& wj : j.at("weapons"))
        u.weapons.push_back(parse_weapon(wj));

    for (auto& [type_name, weight] : j.at("target_priorities").items())
        u.target_priorities[unit_type_from_string(type_name)] = weight.get<double>();

    u.escort = parse_escort(j.at("escort"));
    u.rearm_capacity = j.value("rearm_capacity", 0);
    return u;
}

} 

void UnitDatabase::load_from_file(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("Cannot open " + json_path);
    json root;
    f >> root;

    classes_.clear();
    for (auto& [id, body] : root.at("unit_classes").items())
        classes_.emplace(id, parse_unit_class(id, body));
}

const UnitClassData& UnitDatabase::get(const std::string& class_id) const {
    auto it = classes_.find(class_id);
    if (it == classes_.end())
        throw std::runtime_error("Unknown unit class: " + class_id);
    return it->second;
}

bool UnitDatabase::contains(const std::string& class_id) const {
    return classes_.count(class_id) > 0;
}