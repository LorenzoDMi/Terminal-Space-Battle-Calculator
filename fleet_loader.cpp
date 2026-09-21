#include "fleet_loader.hpp"
#include <json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>

using json = nlohmann::json;

void AceDatabase::load_from_file(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("Cannot open " + json_path);
    json root;
    f >> root;

    aces_.clear();
    for (auto& [id, body] : root.at("aces").items()) {
        AceData a;
        a.ace_id = id;
        a.name = body.value("name", id);
        a.accuracy_bonus = body.value("accuracy_bonus", 0.0);
        a.evasion_bonus = body.value("evasion_bonus", 0.0);
        a.hp_bonus = body.value("hp_bonus", 0);
        aces_.emplace(id, a);
    }
}

const AceData* AceDatabase::find(const std::string& ace_id) const {
    auto it = aces_.find(ace_id);
    return it == aces_.end() ? nullptr : &it->second;
}

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

} // namespace

std::vector<UnitInstance> load_fleet(
    const std::string& fleet_txt_path,
    const UnitDatabase& units,
    const AceDatabase* aces,
    Side side,
    int& next_instance_id)
{
    std::ifstream f(fleet_txt_path);
    if (!f) throw std::runtime_error("Cannot open " + fleet_txt_path);

    std::vector<UnitInstance> fleet;
    std::unordered_map<std::string, int> ace_seen_on_line; // ace_id -> line_no, catches duplicates
    std::string line;
    int line_no = 0;

    while (std::getline(f, line)) {
        ++line_no;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto tokens = split_ws(trimmed);
        std::string class_id = tokens[0];
        if (!units.contains(class_id))
            throw std::runtime_error("Line " + std::to_string(line_no) +
                                      ": unknown unit class '" + class_id + "'");

        int count = 1;
        std::string ace_id;

        for (size_t i = 1; i < tokens.size(); ++i) {
            const std::string& tok = tokens[i];
            if (tok.size() > 1 && tok[0] == 'x' && std::isdigit((unsigned char)tok[1])) {
                count = std::stoi(tok.substr(1));
            } else if (tok.rfind("ace=", 0) == 0) {
                ace_id = tok.substr(4);
            } else {
                throw std::runtime_error("Line " + std::to_string(line_no) +
                                          ": unrecognised token '" + tok + "'");
            }
        }

        if (!ace_id.empty() && count != 1)
            throw std::runtime_error("Line " + std::to_string(line_no) +
                                      ": an ace line must specify exactly one unit (no xN)");

        if (!ace_id.empty()) {
            auto seen = ace_seen_on_line.find(ace_id);
            if (seen != ace_seen_on_line.end())
                throw std::runtime_error("Line " + std::to_string(line_no) +
                                          ": ace '" + ace_id + "' already assigned on line " +
                                          std::to_string(seen->second) + " of this fleet");
            ace_seen_on_line[ace_id] = line_no;
        }

        const UnitClassData& cls = units.get(class_id);

        for (int i = 0; i < count; ++i) {
            UnitInstance u;
            u.instance_id = next_instance_id++;
            u.unit_class = &cls;
            u.side = side;
            u.current_hp = cls.hp;
            u.alive = true;

            for (const auto& w : cls.weapons)
                if (w.ammo.has_value())
                    u.ammo_remaining[w.id] = *w.ammo;

            if (!ace_id.empty()) {
                if (!aces)
                    throw std::runtime_error("Line " + std::to_string(line_no) +
                                              ": ace specified but no ace database was loaded");
                const AceData* ace_data = aces->find(ace_id);
                if (!ace_data)
                    throw std::runtime_error("Line " + std::to_string(line_no) +
                                              ": unknown ace id '" + ace_id + "'");
                u.ace = *ace_data;
                u.current_hp += ace_data->hp_bonus;
            }

            fleet.push_back(std::move(u));
        }
    }

    return fleet;
}