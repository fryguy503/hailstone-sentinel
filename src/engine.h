// SPDX-License-Identifier: MIT
#pragma once

#include "json.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sentinel {
using Json = nlohmann::json;
inline constexpr int max_timer_seconds = 24 * 60 * 60;

struct Entity {
    uint32_t id = 0;
    std::string name;
    bool npc = false, corpse = false, group = false;
    double distance = 0;
    std::optional<double> hp, mana;
    uint32_t cast_id = 0;
    std::string cast_name;
};
struct Buff {
    uint32_t id = 0;
    std::string name;
    std::optional<double> seconds;
    bool invisible = false;
    std::optional<bool> beneficial;
};
struct TimerReading {
    std::string name;
    uint64_t remaining_ms;
    std::optional<uint64_t> previous_ms;
};
struct Snapshot {
    bool in_zone = false, alive = false;
    uint32_t zone_id = 0;
    std::string character;
    std::optional<double> hp, mana, endurance;
    uint32_t target_id = 0, pet_id = 0;
    std::vector<Entity> entities;
    std::vector<Buff> buffs;
    bool buffs_complete = true;
    std::map<std::string, TimerReading> timers;
};
struct Fired {
    std::string id, name, message, speech;
    Json alert;
    uint64_t at = 0;
    bool test = false;
};

Json defaults();
Json events();
Json presets();
Json normalize_alert(const Json&);
Json normalize_config(const Json&);
std::string describe(const Json&);
std::string export_alerts(const Json& alerts);
Json parse_import(std::string text);

class Engine {
public:
    Json config;
    Engine();
    void reset();
    std::vector<Fired> evaluate(const Snapshot&, uint64_t now);
    std::vector<Fired> chat(const Snapshot&, std::string text, uint32_t color, uint64_t now);
    Fired preview(const Json&, const Snapshot&, uint64_t now);

private:
    struct RuleState {
        std::map<uint32_t, bool> latched;
        std::map<uint32_t, uint32_t> casts;
        std::optional<uint64_t> last;
        uint64_t counter = 0;
        bool pet_initialized = false;
        uint32_t pet_id = 0;
        bool buffs_initialized = false;
        std::set<uint32_t> buff_ids;
        std::string fingerprint;
    };
    std::map<std::string, RuleState> state_;
    uint32_t zone_ = 0;
    std::string character_;
    bool active_ = false;
    std::optional<uint64_t> last_tell_at_;
    std::string last_tell_;
    RuleState& state(const Json&);
    bool prepare(const Snapshot&);
    Fired fire(const Json&, const Snapshot&, uint64_t now, std::string detail,
               const std::map<std::string, std::string>& captures = {}, bool test = false);
};
} // namespace sentinel
