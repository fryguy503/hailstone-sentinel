// SPDX-License-Identifier: MIT
#include "engine.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F f, const char* message) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
sentinel::Snapshot snapshot() {
    sentinel::Snapshot s; s.in_zone = s.alive = true; s.zone_id = 10; s.character = "Adventurer";
    s.hp = s.mana = s.endurance = 100; return s;
}
sentinel::Json rule(const char* event, const char* id = "rule") {
    return sentinel::normalize_alert({{"event", event}, {"id", id}, {"threshold", 30}, {"cooldown", 0}});
}
sentinel::Engine engine(const sentinel::Json& a) {
    sentinel::Engine e; e.config["alerts"].push_back(a); return e;
}
void thresholds() {
    auto e = engine(rule("health_below")); auto s = snapshot();
    check(e.evaluate(s, 0).empty(), "Healthy login stays quiet");
    s.hp = 30; check(e.evaluate(s, 100).size() == 1, "Threshold includes equality");
    s.hp = 20; check(e.evaluate(s, 200).empty(), "Low health remains latched");
    s.hp.reset(); check(e.evaluate(s, 300).empty(), "Unknown reading is quiet");
    s.hp = 20; check(e.evaluate(s, 400).empty(), "Unknown reading preserves latch");
    s.hp = 33; check(e.evaluate(s, 500).empty(), "Hysteresis includes recovery boundary");
    s.hp = 30; check(e.evaluate(s, 600).empty(), "Recovery boundary did not rearm");
    s.hp = 34; e.evaluate(s, 700);
    s.hp = 30; check(e.evaluate(s, 800).size() == 1, "Recovery above hysteresis rearms");
    s.hp = std::numeric_limits<double>::quiet_NaN(); check(e.evaluate(s, 900).empty(), "NaN reading is quiet");
    s.hp = 20; check(e.evaluate(s, 1000).empty(), "NaN preserves latch");
    s.zone_id = 11; check(e.evaluate(s, 1100).size() == 1, "Zone transition resets runtime");
    s.alive = false; check(e.evaluate(s, 1200).empty(), "Dead player is quiet");
    s.alive = true; s.hp = 100; e.evaluate(s, 1300);
    e.config["enabled"] = false; s.hp = 5; check(e.evaluate(s, 1400).empty(), "Master pause works");
    e.config["enabled"] = true; check(e.evaluate(s, 1500).size() == 1, "Resume observes new low state");
}
void cooldown_and_preview() {
    auto a = rule("health_below"); a["cooldown"] = 10; a["message"] = "{C} warning {COUNTER}";
    auto e = engine(a); auto s = snapshot(); s.hp = 10;
    auto out = e.evaluate(s, 0); check(out.size() == 1 && out[0].message == "Adventurer warning 1", "First edge fires at monotonic zero");
    a["message"] = "Unsaved edit";
    check(e.preview(a, s, 10).test, "Preview is tagged");
    check(e.evaluate(s, 20).empty(), "Edited preview does not rearm active rule");
    s.hp = 100; e.evaluate(s, 100);
    s.hp = 10; check(e.evaluate(s, 200).empty(), "Cooldown suppresses a new edge");
    check(e.evaluate(s, 11000).empty(), "Cooldown is not periodic while latched");
    s.hp = 100; e.evaluate(s, 12000);
    s.hp = 10; out = e.evaluate(s, 13000);
    check(out.size() == 1 && out[0].message == "Adventurer warning 2", "Preview never increments delivered counter");
}
void entities() {
    auto e = engine(rule("group_health_below")); auto s = snapshot();
    sentinel::Entity first; first.id = 1; first.name = "First"; first.group = true; first.hp = 20;
    sentinel::Entity second = first; second.id = 2; second.name = "Second"; second.hp = 100;
    s.entities = {first, second};
    check(e.evaluate(s, 0).size() == 1, "First low group member fires");
    s.entities[1].hp = 20;
    auto out = e.evaluate(s, 100);
    check(out.size() == 1 && out[0].message.find("Second") != std::string::npos, "Second member has independent latch");
    auto a = rule("player_mana_below"); a["subject"] = "FIRST"; e = engine(a);
    check(e.evaluate(s, 200).empty(), "Unknown remote mana does not mean zero");
    s.entities[0].mana = 20; check(e.evaluate(s, 300).size() == 1, "Named remote mana matches case-insensitively");
    e = engine(rule("target_health_below")); s.target_id = 1;
    check(e.evaluate(s, 400).size() == 1, "First target fires");
    s.target_id = 2; check(e.evaluate(s, 500).size() == 1, "Changing target rearms by entity");
    e = engine(rule("pet_lost")); s.pet_id = 0;
    check(e.evaluate(s, 600).empty(), "No startup pet-lost warning");
    s.pet_id = 3; check(e.evaluate(s, 700).empty(), "Acquiring a pet is quiet");
    s.pet_id = 0; check(e.evaluate(s, 800).size() == 1, "Losing present pet fires");
    check(e.evaluate(s, 900).empty(), "Absent pet stays quiet");
    s.zone_id = 99; check(e.evaluate(s, 1000).empty(), "Zone transition does not lose nonexistent pet");
}
void buffs_spawns_casts() {
    auto a = rule("buff_fading"); a["subject"] = "Protection"; a["threshold"] = 30;
    auto e = engine(a); auto s = snapshot();
    s.buffs.push_back({1, "protection", std::nullopt, false});
    check(e.evaluate(s, 0).empty(), "Unknown buff timer is quiet");
    s.buffs[0].seconds = 30; check(e.evaluate(s, 100).size() == 1, "Known buff fade threshold fires");
    s.buffs.clear(); check(e.evaluate(s, 200).empty(), "Missing buff is not a fading buff");
    a = rule("buff_missing"); a["subject"] = "Protection"; e = engine(a);
    check(e.evaluate(s, 300).size() == 1, "Required missing buff fires");
    check(e.evaluate(s, 400).empty(), "Missing buff is latched");
    s.buffs.push_back({1, "Protection", 200, false}); e.evaluate(s, 500);
    s.buffs.clear(); check(e.evaluate(s, 600).size() == 1, "Buff returning rearms missing warning");
    sentinel::Entity npc; npc.id = 1; npc.npc = true; npc.name = "Named NPC"; npc.distance = 200;
    s.entities = {npc}; a = rule("spawn_appears"); a["subject"] = "Named NPC"; a["threshold"] = 250; e = engine(a);
    check(e.evaluate(s, 700).size() == 1, "Named NPC in range fires");
    s.entities[0].corpse = true; e.evaluate(s, 800);
    s.entities[0].corpse = false; s.entities[0].distance = 300; check(e.evaluate(s, 900).empty(), "NPC outside range is quiet");
    a = rule("target_cast"); a["subject"] = "Danger"; e = engine(a); s.target_id = 1;
    s.entities[0].cast_id = 20; s.entities[0].cast_name = "Danger";
    check(e.evaluate(s, 1000).size() == 1, "Target spell detected");
    check(e.evaluate(s, 1100).empty(), "Same cast stays quiet");
    s.entities[0].cast_id = 0; e.evaluate(s, 1200);
    s.entities[0].cast_id = 20; check(e.evaluate(s, 1300).size() == 1, "Same spell after cast ended fires again");
    s.entities[0].cast_id = 21; s.entities[0].cast_name = "Safe"; check(e.evaluate(s, 1400).empty(), "Exact spell filter excludes different spell");
}
void chat_patterns() {
    auto a = rule("chat_text_match"); a["subject"] = "{S1} hits {C} for {N1>=1000} damage*";
    a["message"] = "{S1}: {N1} on {C} / {COUNTER}"; a["speech_text"] = "Danger from {S1}";
    auto e = engine(a); auto s = snapshot();
    auto out = e.chat(s, "A giant hits Adventurer for 1,250 damage!", 0, 0);
    check(out.size() == 1 && out[0].message == "A giant: 1,250 on Adventurer / 1", "Captures, numeric comma comparison and character expansion");
    check(out[0].speech == "Danger from A giant", "Speech has independent template");
    check(e.chat(s, "A giant hits Adventurer for 999 damage!", 0, 10).empty(), "Number comparison rejects low damage");
    check(e.chat(s, "prefix A giant hits Adventurer for 999 damage!", 0, 20).empty(), "Pattern is whole-line");
    a["subject"] = "HEL?O"; a["message"] = "{L}"; e = engine(a);
    check(e.chat(s, "hello", 0, 30).size() == 1, "One-character wildcard and case-insensitivity");
    check(e.chat(s, "well hello", 0, 40).empty(), "Bare pattern does not become substring");
    a = rule("incoming_tell"); a["subject"] = "ready"; e = engine(a);
    check(e.chat(s, "Friend tells you, 'Are you READY?'", 257, 50).size() == 1, "Channel plain text uses substring");
    check(e.chat(s, "Friend tells you, 'Are you READY?'", 257, 51).empty(), "Duplicate tell callback suppressed");
    check(e.chat(s, "Friend tells you, 'Are you READY?'", 257, 300).size() == 1, "Later intentional duplicate tell preserved");
    check(e.chat(s, "Friend says ready", 259, 60).empty(), "Different channel excluded");
    a = rule("spell_resisted"); e = engine(a);
    check(e.chat(s, "Your target resisted the Fire spell.", 289, 70).size() == 1, "Own resist recognized");
    check(e.chat(s, "Another's target resisted the Fire spell.", 406, 80).empty(), "Other-player resist excluded");
    check(e.chat(s, "Your target resisted the Fire spell.", 0, 81).size() == 1, "Actual host RGB-zero resist recognized");
    check(e.chat(s, "Your target has resisted your attempt to mesmerize it.", 0, 82).size() == 1, "Actual host mesmerize resist recognized");
    a = rule("invisibility_fading"); e = engine(a);
    check(e.chat(s, "You feel yourself starting to appear.", 0, 90).size() == 1, "Invisibility fade text recognized");
    a = rule("chat_text_match"); a["subject"] = std::string(200, '*') + "never"; e = engine(a);
    check(e.chat(s, std::string(2048, 'a'), 0, 100).empty(), "Pathological pattern is bounded");
    check(e.chat(s, std::string(3000, 'a'), 0, 110).empty(), "Oversized chat ignored");
}
void host_chat_formats() {
    auto s = snapshot();
    auto a = rule("incoming_tell"); auto e = engine(a);
    check(e.chat(s, "Friend tells you, 'Ready?'", 0, 0).size() == 1, "Actual host English tell recognized without raw colour");
    check(e.chat(s, "Friend tells you, 'Ready?'", 0, 1).empty(), "Actual host duplicate tell suppressed");
    check(e.chat(s, "Friend tells you, 'Ready?'", 0, 250).size() == 1, "Actual host later tell preserved");
    check(e.chat(s, "Friend tells you, in Elvish, 'Ready?'", 0, 500).size() == 1, "Tell language annotation accepted");
    check(e.chat(s, "Friend tells you,in an unknown tongue, 'Ready?'", 0, 750).size() == 1, "Unknown language annotation accepted");
    check(e.chat(s, "Server.Friend tells you, 'Ready?'", 0, 1000).size() == 1, "Bounded server-qualified sender accepted");
    check(e.chat(s, "You tell Friend, 'Ready?'", 0, 1100).empty(), "Outgoing tell excluded");
    check(e.chat(s, "You told Friend, 'Ready?'", 0, 1200).empty(), "Alternate outgoing tell excluded");
    check(e.chat(s, "Friend says, 'Other tells you, how are you?'", 0, 1300).empty(), "Nested tell phrase in say does not classify as tell");
    check(e.chat(s, "Friend tells the guild, 'Other tells you, how are you?'", 0, 1400).empty(), "Nested tell phrase in guild does not classify as tell");
    check(e.chat(s, "Some NPC tells you, 'Ready?'", 0, 1500).empty(), "Multi-word sender cannot forge social envelope");
    check(e.chat(s, "Friend tells you, Ready?", 0, 1600).empty(), "Missing message quote rejected");
    check(e.chat(s, "Friend tells you, 'Ready?", 0, 1700).empty(), "Unclosed message rejected");
    check(e.chat(s, "Friend0 tells you, 'Ready?'", 0, 1800).empty(), "Invalid player-name prefix rejected");
    for (const auto& pair : {std::pair<const char*,const char*>{"group_message", "group"}, {"guild_message", "guild"},
                             {"raid_message", "raid"}, {"fellowship_message", "fellowship"}}) {
        e = engine(rule(pair.first));
        check(e.chat(s, std::string("Friend tells the ") + pair.second + ", 'Ready?'", 0, 1900).size() == 1, "Actual host social channel recognized");
        check(e.chat(s, std::string("Friend says, 'I told the ") + pair.second + ", Ready?'", 0, 2000).empty(), "Say content cannot forge channel");
    }
    e = engine(rule("raid_message"));
    check(e.chat(s, "Friend tells the raid,  'Move!'", 0, 2100).size() == 1, "Actual host raid format extra space accepted");
    check(e.chat(s, "Friend tells the raid,  in Elvish, 'Move!'", 0, 2200).size() == 1, "Raid language format accepted");
    e = engine(rule("spell_resisted"));
    check(e.chat(s, "Friend says, 'Your target resisted the Fire spell.'", 0, 2300).empty(), "Quoted resist message excluded");
}
void config_and_sharing() {
    check(sentinel::events().size() == 23, "All original event types catalogued");
    check(sentinel::presets().size() >= 30, "Preset library supplied");
    auto a = rule("health_below"); a["volume"] = 1000; a["duration"] = -1; a["sound_file"] = "../private.wav";
    a = sentinel::normalize_alert(a);
    check(a["volume"] == 100 && a["duration"] == .5 && a["sound_file"] == "", "Normalize bounds and sound paths");
    auto unicode = rule("health_below"); unicode["name"] = std::string(95, 'a') + "\xc3\xa9";
    unicode = sentinel::normalize_alert(unicode);
    check(unicode["name"].get<std::string>() == std::string(95, 'a') && !unicode.dump().empty(), "Byte limits preserve valid UTF-8 boundaries");
    rejects([] { sentinel::normalize_alert({{"event", "future_unknown"}}); }, "Unknown event rejected");
    auto cfg = sentinel::defaults(); cfg["overlay_x"] = .25; cfg["overlay_y"] = .4; cfg["locked"] = false; cfg["quiet"] = true;
    cfg["alerts"] = {a, a}; cfg = sentinel::normalize_config(cfg);
    check(cfg["alerts"][0]["id"] != cfg["alerts"][1]["id"], "Duplicate IDs normalized uniquely");
    check(cfg["overlay_x"] == .25 && cfg["quiet"] == true, "Display preferences survive normalization");
    const auto encoded = sentinel::export_alerts(cfg["alerts"]);
    check(sentinel::parse_import(encoded).size() == 2, "Portable sharing round trip");
    rejects([] { sentinel::parse_import("{}"); }, "Invalid export envelope rejected");
    rejects([] { sentinel::parse_import("[{\"event\":\"unknown\"}]"); }, "Unknown imported event rejected");
    rejects([] { sentinel::parse_import("[{\"event\":\"buff_missing\"}]"); }, "Required imported subject enforced");
    rejects([] { sentinel::parse_import(std::string(128 * 1024 + 1, ' ')); }, "Oversized import rejected");
    rejects([] { sentinel::parse_import(std::string(1000, '[') + "0" + std::string(1000, ']')); }, "Deeply nested imports rejected before recursion grows");
    auto zone_rule = rule("health_below"); zone_rule["zone_id"] = 12; auto e = engine(zone_rule); auto s = snapshot(); s.hp = 5;
    check(e.evaluate(s, 0).empty(), "Zone restriction excludes other zones");
    s.zone_id = 12; check(e.evaluate(s, 10).size() == 1, "Zone restriction includes selected zone");
}
} // namespace
int main() {
    try {
        thresholds(); cooldown_and_preview(); entities(); buffs_spawns_casts(); chat_patterns(); host_chat_formats(); config_and_sharing();
        std::cout << "Sentinel engine: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Sentinel engine failure after " << checks << " checks: " << e.what() << '\n'; return 1;
    }
}
