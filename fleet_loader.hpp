#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "fleet.hpp"

class AceDatabase {
public:
    void load_from_file(const std::string& json_path);
    const AceData* find(const std::string& ace_id) const; // nullptr if not found
private:
    std::unordered_map<std::string, AceData> aces_;
};

// Reads a plain-text fleet roster and instantiates UnitInstances against a UnitDatabase.
// File format, one entry per line:
//   class_id [xN] [ace=ace_id]
// Lines starting with '#' and blank lines are ignored. xN repeats the line N times;
// ace=id may only be combined with a bare (count-1) line. The same ace_id cannot
// appear twice in one fleet file (throws — catches copy-paste typos).
//
// next_instance_id is a shared counter: pass the same int& for both fleets so
// aggressor and defender instance IDs never collide.
std::vector<UnitInstance> load_fleet(
    const std::string& fleet_txt_path,
    const UnitDatabase& units,
    const AceDatabase* aces,   // may be nullptr if no ace file exists yet
    Side side,
    int& next_instance_id
);