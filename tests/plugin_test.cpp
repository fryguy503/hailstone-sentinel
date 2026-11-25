// SPDX-License-Identifier: MIT
// Exercises the exported plugin through a fake public SDK host. No client internals.
#include "hailstone_plugin.h"
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using sentinel::Json;
namespace fs = std::filesystem;

struct HailstoneArgs { Json values = Json::array(); };
struct HailstoneOut {
    struct Frame { Json value; std::string key; };
    Json root;
    std::vector<Frame> open;
};

extern "C" void hailstone_plugin_set_host(const PluginHostApi*);
extern "C" void InitializePlugin();
extern "C" void ShutdownPlugin();
extern "C" void OnPulse();
extern "C" void OnBeginZone();
extern "C" void OnZoned();
extern "C" void SetGameState(int);
extern "C" int OnIncomingChat(const char*, uint32_t);
#ifdef SENTINEL_TESTING
extern "C" void sentinel_test_time(uint64_t);
#endif

namespace {
int checks = 0;
void check(bool yes, const std::string& what) {
    ++checks;
    if (!yes) throw std::runtime_error(what);
}
int copy_text(const std::string& text, char* out, size_t capacity) {
    if (out && capacity) {
        const size_t count = std::min(capacity - 1, text.size());
        std::memcpy(out, text.data(), count);
        out[count] = 0;
    }
    return static_cast<int>(text.size());
}
Json from_value(const HailstoneValue& v) {
    switch (v.kind) {
    case HAILSTONE_BOOL: return v.i != 0;
    case HAILSTONE_INT: return v.i;
    case HAILSTONE_FLOAT: return v.f;
    case HAILSTONE_STRING: return v.s ? Json(v.s) : Json("");
    default: return nullptr;
    }
}
int to_value(const Json& j, HailstoneValue* out) {
    if (!out) return 0;
    *out = {};
    if (j.is_boolean()) { out->kind = HAILSTONE_BOOL; out->i = j.get<bool>() ? 1 : 0; }
    else if (j.is_number_integer()) { out->kind = HAILSTONE_INT; out->i = j.get<int64_t>(); }
    else if (j.is_number()) { out->kind = HAILSTONE_FLOAT; out->f = j.get<double>(); }
    else if (j.is_string()) { out->kind = HAILSTONE_STRING; out->s = j.get_ref<const std::string&>().c_str(); }
    else if (j.is_object() || j.is_array()) out->kind = HAILSTONE_TABLE;
    return 1;
}
void append(HailstoneOut* out, Json value) {
    if (out->open.empty()) { out->root = std::move(value); return; }
    auto& f = out->open.back();
    if (f.value.is_array()) f.value.push_back(std::move(value));
    else {
        check(!f.key.empty(), "SDK table output has a key");
        f.value[f.key] = std::move(value);
        f.key.clear();
    }
}

struct Fixture {
    struct Function { HailstoneUiFn fn; void* user; };
    struct Command { HailstoneCommandFn fn; void* user; };
    PluginHostApi api{};
    HailstonePlayer player{};
    std::map<std::string, Function> functions;
    std::map<std::string, Command> commands;
    std::map<uint32_t, HailstoneSpawn> spawns;
    std::map<uint32_t, HailstoneBuff> buffs;
    std::set<uint32_t> failed_buff_slots;
    std::map<uint32_t, HailstoneSpell> spells;
    std::map<std::string, std::string> parsed;
    std::vector<std::string> chat, errors;
    fs::path root;
    std::string character = "Alice";
    std::thread::id ui_thread = std::this_thread::get_id();
    bool loaded = false;
    uint64_t time = 1000;

    Fixture() {
        const auto serial = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("sentinel-plugin-test-" + std::to_string(serial));
        fs::create_directories(root);
        player.size = sizeof(player);
        player.spawn_id = 1;
        copy_text("Alice", player.name, sizeof(player.name));
        player.game_state = GAMESTATE_INGAME;
        player.in_zone = 1;
        player.stand_state = 100;
        player.zone_id = 2;
        player.max_hp = 1000;
        player.cur_hp = 1000;
        player.max_mana = player.max_endurance = 100;
        player.cur_mana = player.cur_endurance = 100;

        api.abi = HAILSTONE_PLUGIN_ABI;
        api.size = sizeof(api);
        api.host = this;
        api.caps = [](void*) { return HAILSTONE_CAP_READ | HAILSTONE_CAP_COMMANDS | HAILSTONE_CAP_EVENTS | HAILSTONE_CAP_UI; };
        api.name = [](void*) { return "sentinel"; };
        api.log = [](void* h, int level, const char* text) {
            auto& f = of(h); f.same_thread();
            if (level >= 3) f.errors.emplace_back(text ? text : "");
        };
        api.write_chat = [](void* h, const char* text, uint32_t) { of(h).say(text); };
        api.write_chat_color = api.write_chat;
        api.add_command = [](void* h, const char* name, HailstoneCommandFn fn, void* user) {
            auto& f = of(h); f.same_thread(); f.commands[name] = {fn, user}; return 1;
        };
        api.remove_command = [](void* h, const char* name) { auto& f = of(h); f.same_thread(); f.commands.erase(name); };
        api.add_ui_function = [](void* h, const char* name, HailstoneUiFn fn, void* user) {
            auto& f = of(h); f.same_thread(); f.functions[name] = {fn, user}; return 1;
        };
        api.remove_ui_function = [](void* h, const char* name) { auto& f = of(h); f.same_thread(); f.functions.erase(name); };
        api.add_menu = [](void* h, const char*, const char*) { of(h).same_thread(); return 1; };
        api.add_option = [](void* h, const char*, const char*, int, int, HailstoneOptionGetFn, HailstoneOptionSetFn, void*) {
            of(h).same_thread(); return 1;
        };
        api.arg_count = [](void*, const HailstoneArgs* a) { return a ? a->values.size() : size_t{0}; };
        api.arg = [](void*, const HailstoneArgs* a, size_t i, HailstoneValue* out) {
            return a && i < a->values.size() ? to_value(a->values[i], out) : 0;
        };
        api.arg_field = [](void*, const HailstoneArgs* a, size_t i, const char* key, HailstoneValue* out) {
            if (!a || i >= a->values.size() || !a->values[i].is_object()) return 0;
            const auto it = a->values[i].find(key);
            return it == a->values[i].end() ? 0 : to_value(*it, out);
        };
        api.out_value = [](void*, HailstoneOut* out, const HailstoneValue* v) { append(out, from_value(*v)); };
        api.out_begin_array = [](void*, HailstoneOut* out) { out->open.push_back({Json::array(), {}}); };
        api.out_begin_table = [](void*, HailstoneOut* out) { out->open.push_back({Json::object(), {}}); };
        api.out_key = [](void*, HailstoneOut* out, const char* key) {
            check(!out->open.empty(), "out_key inside a container"); out->open.back().key = key;
        };
        api.out_end = [](void*, HailstoneOut* out) {
            check(!out->open.empty(), "out_end balances a container");
            auto value = std::move(out->open.back().value); out->open.pop_back(); append(out, std::move(value));
        };
        api.config_dir = [](void* h, char* out, size_t cap) {
            auto& f = of(h); f.same_thread();
            const auto dir = f.root / "global"; fs::create_directories(dir); return copy_text(dir.string(), out, cap);
        };
        api.server_config_dir = [](void* h, char* out, size_t cap) {
            auto& f = of(h); f.same_thread();
            const auto dir = f.root / "server"; fs::create_directories(dir); return copy_text(dir.string(), out, cap);
        };
        api.character_config_dir = [](void* h, char* out, size_t cap) {
            auto& f = of(h); f.same_thread();
            if (!f.player.in_zone || f.character.empty()) return 0;
            const auto dir = f.root / f.character; fs::create_directories(dir); return copy_text(dir.string(), out, cap);
        };
        api.get_player = [](void* h, HailstonePlayer* out) { auto& f = of(h); f.same_thread(); *out = f.player; return 1; };
        api.get_spawn = [](void* h, uint32_t id, HailstoneSpawn* out) {
            auto& f = of(h); f.same_thread();
            if (id != f.player.spawn_id) {
                const auto found = f.spawns.find(id);
                if (found == f.spawns.end()) return 0;
                *out = found->second; return 1;
            }
            *out = {}; out->size = sizeof(*out); out->id = id; copy_text(f.player.name, out->name, sizeof(out->name));
            out->hp_percent = static_cast<uint32_t>(f.player.cur_hp * 100 / f.player.max_hp); return 1;
        };
        api.next_spawn = [](void* h, uint32_t after, HailstoneSpawn* out) {
            auto& f = of(h); const auto found = f.spawns.upper_bound(after);
            if (after < f.player.spawn_id && (found == f.spawns.end() || f.player.spawn_id < found->first))
                return f.api.get_spawn(h, f.player.spawn_id, out);
            if (found == f.spawns.end()) return 0;
            return f.api.get_spawn(h, found->first, out);
        };
        api.get_buff = [](void* h, uint32_t index, HailstoneBuff* out) {
            auto& f = of(h); f.same_thread();
            if (index >= f.player.buff_slots || f.failed_buff_slots.count(index)) return 0;
            const auto found = f.buffs.find(index);
            *out = {}; out->size = sizeof(*out);
            if (found != f.buffs.end()) *out = found->second;
            // A readable empty slot succeeds with spell_id zero, as the real SDK does.
            return 1;
        };
        api.get_spell = [](void* h, uint32_t id, HailstoneSpell* out) {
            auto& f = of(h); f.same_thread(); const auto found = f.spells.find(id);
            if (found == f.spells.end()) return 0;
            *out = found->second; return 1;
        };
        api.parse = [](void* h, const char* expression, char* out, size_t cap) {
            auto& f = of(h); f.same_thread(); const auto found = f.parsed.find(expression);
            return copy_text(found == f.parsed.end() ? "NULL" : found->second, out, cap);
        };
        api.zone_name = [](void* h, uint32_t, char* out, size_t cap) { of(h).same_thread(); return copy_text("qeynos2", out, cap); };
    }
    ~Fixture() {
        if (loaded) ShutdownPlugin();
        hailstone_plugin_set_host(nullptr);
        // root was created uniquely under the OS temporary directory, never supplied by a test input.
        std::error_code ec; fs::remove_all(root, ec);
    }
    static Fixture& of(void* h) { return *static_cast<Fixture*>(h); }
    void same_thread() const {
        if (std::this_thread::get_id() != ui_thread) throw std::runtime_error("worker called the host API");
    }
    void say(const char* text) {
        same_thread(); chat.emplace_back(text ? text : "");
        // Deliberately feed output back as incoming: even a less careful host must not recurse.
        if (chat.size() < 200) OnIncomingChat(chat.back().c_str(), 15);
    }
    void start() {
        hailstone_plugin_set_host(&api);
#ifdef SENTINEL_TESTING
        sentinel_test_time(time);
#endif
        InitializePlugin(); loaded = true; SetGameState(GAMESTATE_INGAME); tick();
    }
    void stop() { ShutdownPlugin(); loaded = false; }
    void tick() {
        time += 1000;
#ifdef SENTINEL_TESTING
        sentinel_test_time(time);
#endif
        OnPulse();
    }
    Json call(const std::string& name, Json args = Json::array()) {
        const auto it = functions.find(name);
        check(it != functions.end(), "registered UI function " + name);
        HailstoneArgs a{std::move(args)}; HailstoneOut out;
        check(it->second.fn(it->second.user, &a, &out) != 0, "UI function returned a value: " + name);
        check(out.open.empty(), "balanced SDK output: " + name);
        return out.root;
    }
    Json state() { return call("state"); }
    Json action(const std::string& op, const std::string& id = "") { return call("action", Json::array({op, id})); }
    void commandline(const std::string& args) {
        auto found = commands.find("/sentinel");
        if (found == commands.end()) found = commands.find("sentinel");
        check(found != commands.end(), "Sentinel command is registered before invocation");
        found->second.fn(found->second.user, args.c_str());
    }
    Json save(const Json& rule) { return call("save", Json::array({rule})); }
    size_t count() { return state().at("alerts").size(); }
    void choose(const std::string& name) {
        OnBeginZone(); player.in_zone = 0; SetGameState(GAMESTATE_CHARSELECT); tick();
        character = name; copy_text(name, player.name, sizeof(player.name));
        player.in_zone = 1; player.cur_hp = player.max_hp; SetGameState(GAMESTATE_INGAME); OnZoned(); tick();
    }
};

Json low_health_rule() {
    return {{"name", "Integration HP"}, {"event", "health_below"}, {"threshold", 30}, {"enabled", true},
            {"chat", true}, {"overlay", true}, {"sound", false}, {"speech", false},
            {"message", "Harness health low"}, {"cooldown", 0}, {"category", "Tests"}};
}

void actions_and_persistence() {
    Fixture f; f.start();
    check(f.commands.count("/sentinel") + f.commands.count("sentinel") == 1, "one Sentinel command registered");
    for (const char* name : {"state", "action", "save", "preview", "export", "inspect", "import", "settings",
         "timer_save", "timer_action", "timer_layout"})
        check(f.functions.count(name) == 1, std::string("UI API available: ") + name);
    const size_t initial = f.count();
    const auto saved = f.save(low_health_rule());
    check(saved.value("ok", false), "rule saves through SDK table arguments");
    const std::string id = saved.at("id");
    check(!id.empty() && f.count() == initial + 1, "save creates stable identity");
    check(f.action("off").value("ok", false) && !f.state().at("enabled").get<bool>(), "master pause");
    check(f.action("on").value("ok", false) && f.state().at("enabled").get<bool>(), "master resume");
    check(f.action("hide").value("ok", false) && !f.state().at("open").get<bool>(), "hide window");
    check(f.action("show").value("ok", false) && f.state().at("open").get<bool>(), "show window");
    check(f.action("duplicate", id).value("ok", false), "duplicate selected rule");
    check(f.count() == initial + 2, "duplicate adds one rule");
    auto all = f.state().at("alerts");
    std::string clone;
    for (const auto& rule : all) if (rule.at("id") != id && rule.at("name").get<std::string>().find("Integration HP") != std::string::npos) clone = rule.at("id");
    check(!clone.empty() && clone != id, "duplicate receives independent identity");
    check(f.action("toggle_alert", clone).value("ok", false), "toggle one rule");
    check(f.action("delete", clone).value("ok", false) && f.count() == initial + 1, "delete one rule");
    check(!f.action("delete", "missing-rule").value("ok", true), "unknown rule is an error");
    auto update = low_health_rule(); update["id"] = id; update["name"] = "Updated HP";
    check(f.save(update).value("ok", false) && f.count() == initial + 1, "saving an existing id updates without append");
    const auto path = fs::path(f.state().at("config_path").get<std::string>());
    check(fs::is_regular_file(path), "settings committed to character directory");
    f.stop(); f.start();
    all = f.state().at("alerts");
    check(std::any_of(all.begin(), all.end(), [&](const Json& rule) { return rule.value("id", "") == id && rule.value("name", "") == "Updated HP"; }), "restart reloads saved values");
    f.choose("Bob");
    all = f.state().at("alerts");
    check(std::none_of(all.begin(), all.end(), [&](const Json& rule) { return rule.value("id", "") == id; }), "different character cannot inherit Alice's rule");
    f.choose("Alice");
    all = f.state().at("alerts");
    check(std::any_of(all.begin(), all.end(), [&](const Json& rule) { return rule.value("id", "") == id; }), "returning character reloads its own rule");
}

void sharing_and_validation() {
    Fixture f; f.start();
    const auto saved = f.save(low_health_rule());
    check(saved.value("ok", false), "export source saves");
    const std::string id = saved.at("id");
    const auto export_value = f.call("export", Json::array({id}));
    check(export_value.is_string() && !export_value.get_ref<const std::string&>().empty(), "export produces selectable text");
    const std::string text = export_value;
    const size_t before = f.count();
    const auto inspected = f.call("inspect", Json::array({text}));
    check(inspected.value("ok", false) && inspected.at("alerts").size() == 1, "inspect validates one shared alert");
    check(f.count() == before, "inspect never mutates config");
    const auto imported = f.call("import", Json::array({text}));
    check(imported.value("ok", false) && imported.value("count", 0) == 1, "apply reviewed share");
    check(f.count() == before + 1, "import appends one rule");
    const auto all = f.state().at("alerts");
    check(all.back().at("id") != id && !all.back().at("enabled").get<bool>(), "imports get new identity and start disabled");
    for (const std::string bad : {"{invalid", "{\"format\":\"hailstone-sentinel\",\"version\":999,\"alerts\":[]}", "[{}]"}) {
        check(!f.call("inspect", Json::array({bad})).value("ok", true), "inspect rejects malformed or unsupported share");
        check(!f.call("import", Json::array({bad})).value("ok", true), "import rejects malformed or unsupported share");
        check(f.count() == before + 1, "invalid import is atomic");
    }
    auto malicious = low_health_rule(); malicious["event"] = "unknown_event";
    check(!f.save(malicious).value("ok", true), "unknown event rejected at editor boundary");
    const auto too_long = std::string(128 * 1024 + 1, 'x');
    check(!f.call("import", Json::array({too_long})).value("ok", true), "oversize share is bounded");
}

void evaluation_and_lifecycle() {
    Fixture f; f.start();
    const auto saved = f.save(low_health_rule());
    check(saved.value("ok", false), "live rule saves");
    f.chat.clear(); f.tick();
    check(f.chat.empty(), "healthy state stays quiet");
    f.player.cur_hp = 200; f.tick();
    const auto first = f.chat.size();
    check(first == 1 && f.chat.front().find("Harness health low") != std::string::npos, "threshold transition delivers one chat message");
    f.tick();
    check(f.chat.size() == first, "persistent low health does not repeat or recurse");
    f.player.cur_hp = 900; f.tick(); f.player.cur_hp = 200; f.tick();
    check(f.chat.size() == first + 1, "recovery rearms threshold");
    f.action("off"); f.chat.clear(); f.player.cur_hp = 900; f.tick(); f.player.cur_hp = 200; f.tick();
    check(f.chat.empty(), "master pause prevents all evaluation");
    f.action("on"); f.player.cur_hp = 900; f.tick();
    OnBeginZone(); f.player.in_zone = 0; f.tick();
    check(f.state().at("overlays").empty(), "zone transition clears overlay queue");
    const auto n = f.chat.size(); OnIncomingChat("Bob tells you, 'hello'", 13);
    check(f.chat.size() == n, "out-of-zone chat cannot trigger rules");
    check(!f.state().at("in_zone").get<bool>(), "UI reports not watching outside zone");
    f.stop();
    check(f.commands.empty(), "shutdown removes commands");
    check(f.functions.empty(), "shutdown removes UI callbacks");
}

void snapshot_adapter() {
    Fixture f; f.start();
    HailstoneSpawn tank{}; tank.size = sizeof(tank); tank.id = 2; tank.hp_percent = 100; tank.in_group = 1;
    copy_text("Tank", tank.name, sizeof(tank.name)); f.spawns[2] = tank;
    HailstoneSpawn npc{}; npc.size = sizeof(npc); npc.id = 3; npc.hp_percent = 100; npc.is_npc = 1;
    copy_text("Dread Captain", npc.name, sizeof(npc.name)); f.spawns[3] = npc; f.player.target_id = 3;
    HailstoneBuff buff{}; buff.size = sizeof(buff); buff.spell_id = 100; buff.tics = 10;
    f.buffs[5] = buff; f.player.buff_slots = 43;
    HailstoneSpell spell{}; spell.size = sizeof(spell); spell.id = 100;
    copy_text("Clarity", spell.name, sizeof(spell.name)); f.spells[100] = spell;
    HailstoneBuff song = buff; song.spell_id = 101; f.buffs[42] = song;
    HailstoneSpell song_spell = spell; song_spell.id = 101;
    copy_text("Battle Chorus", song_spell.name, sizeof(song_spell.name)); f.spells[101] = song_spell;

    auto rule = low_health_rule(); rule["event"] = "player_mana_below"; rule["subject"] = "Tank"; rule["message"] = "Mapped mana";
    check(f.save(rule).value("ok", false), "named player mana rule saves");
    rule["event"] = "target_cast"; rule["subject"] = "Fireball"; rule["message"] = "Mapped cast";
    check(f.save(rule).value("ok", false), "target cast rule saves");
    rule["event"] = "buff_fading"; rule["subject"] = "Clarity"; rule["threshold"] = 15; rule["message"] = "Mapped buff";
    check(f.save(rule).value("ok", false), "buff duration rule saves");
    rule["subject"] = "Battle Chorus"; rule["message"] = "Mapped song";
    check(f.save(rule).value("ok", false), "song duration rule saves");
    f.chat.clear(); f.tick();
    check(f.chat.empty(), "unknown remote mana, cast and buff countdown are not guessed");
    f.parsed["${Spawn[id 2].PctMana}"] = "20";
    f.parsed["${Spawn[id 3].Casting.ID}"] = "55";
    f.parsed["${Spawn[id 3].Casting.Name}"] = "Fireball";
    f.parsed["${Me.CountBuffs}"] = "1";
    f.parsed["${Me.CountSongs}"] = "1";
    f.parsed["${Me.Buff[1].Spell.ID}"] = "100";
    f.parsed["${Me.Buff[1].Duration.TotalSeconds}"] = "12";
    f.parsed["${Me.Song[1].Spell.ID}"] = "101";
    f.parsed["${Me.Song[1].Duration.TotalSeconds}"] = "10";
    f.tick();
    for (const auto* message : {"Mapped mana", "Mapped cast", "Mapped buff", "Mapped song"})
        check(std::any_of(f.chat.begin(), f.chat.end(), [&](const std::string& text) { return text.find(message) != std::string::npos; }),
              std::string("SDK snapshot adapter produces ") + message);
    check(f.chat.size() == 4, "each mapped event fires once");
    f.tick(); check(f.chat.size() == 4, "mapped events retain their latches");
}

void damaged_settings_are_preserved() {
    Fixture f; f.start();
    check(f.save(low_health_rule()).value("ok", false), "settings fixture saves");
    const auto path = fs::path(f.state().at("config_path").get<std::string>());
    for (const std::string damaged : {"{invalid: original bytes must survive}", "{\"version\":999,\"enabled\":true,\"alerts\":[]}"}) {
        f.stop();
        { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << damaged; }
        f.start();
        check(!f.state().value("error", std::string{}).empty(), "unreadable or unsupported config exposes useful error");
        check(!f.save(low_health_rule()).value("ok", true), "saving cannot overwrite unreadable or unsupported config");
        std::ifstream in(path, std::ios::binary);
        const std::string remaining((std::istreambuf_iterator<char>(in)), {});
        check(remaining == damaged, "unreadable or unsupported config remains byte-for-byte intact");
    }
}

void incomplete_rules_cannot_be_enabled() {
    Fixture f; f.start();
    const size_t before = f.count();
    for (const auto* event : {"player_health_below", "player_mana_below", "buff_fading", "buff_missing", "spawn_appears", "player_appears", "chat_text_match"}) {
        auto rule = low_health_rule(); rule["event"] = event; rule["subject"] = " \t ";
        check(!f.save(rule).value("ok", true), std::string("required subject checked for ") + event);
        check(f.count() == before, "incomplete rule never enters library");
    }
    auto silent = low_health_rule(); silent["chat"] = false; silent["overlay"] = false;
    check(!f.save(silent).value("ok", true) && f.count() == before, "rule without any notification cannot save");
    check(f.save(low_health_rule()).value("ok", false), "valid rule still saves after rejected changes");
    const auto path = fs::path(f.state().at("config_path").get<std::string>());
    f.stop();
    auto incomplete = low_health_rule(); incomplete["event"] = "buff_missing"; incomplete["subject"] = "";
    incomplete["enabled"] = false; incomplete["id"] = "disabled-incomplete";
    const Json settings = {{"version", 1}, {"enabled", true}, {"alerts", Json::array({incomplete})}};
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << settings.dump(); }
    f.start();
    check(!f.action("toggle_alert", "disabled-incomplete").value("ok", true), "enabling revalidates an incomplete saved rule");
    check(!f.state().at("alerts").at(0).at("enabled").get<bool>(), "failed enable preserves disabled state");
}

void window_visibility_survives_zoning_and_reload() {
    Fixture f;
    check(fs::is_empty(f.root), "first-use fixture has no existing settings");
    f.start();
    check(f.state().at("open").get<bool>(), "first install opens the Sentinel window");
    check(f.action("hide").value("ok", false), "closing through the UI succeeds");
    check(!f.state().at("open").get<bool>(), "UI close hides the window immediately");

    // Exercise the callback-only lifecycle as well as the host's native-plugin reload.
    OnBeginZone(); f.player.in_zone = 0; SetGameState(GAMESTATE_LOGGINGIN); f.tick();
    check(!f.state().at("open").get<bool>(), "closed window stays closed while leaving a zone");
    f.player.zone_id = 3; f.player.in_zone = 1; SetGameState(GAMESTATE_INGAME); OnZoned(); f.tick();
    check(!f.state().at("open").get<bool>(), "zone callbacks do not reopen a closed window");
    OnBeginZone(); f.player.in_zone = 0; SetGameState(GAMESTATE_LOGGINGIN); f.tick();
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "full shutdown and reinitialization restore a closed window");
    f.player.zone_id = 4; f.player.in_zone = 1; OnZoned(); f.tick();
    check(!f.state().at("open").get<bool>(), "reloaded plugin remains closed upon entering the next zone");

    f.commandline("");
    check(f.state().at("open").get<bool>(), "plain /sentinel opens the window");
    f.stop(); f.start();
    check(f.state().at("open").get<bool>(), "plain /sentinel saves the open preference across reloads");
    f.choose("Bob");
    check(f.state().at("open").get<bool>(), "changing character retains an open window");
    f.commandline("hide");
    check(!f.state().at("open").get<bool>(), "/sentinel hide closes the window");
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "command close saves the closed preference across reloads");
    f.choose("Alice");
    check(!f.state().at("open").get<bool>(), "returning to another character does not reopen the window");

    check(f.action("show").value("ok", false), "UI show opens the window");
    f.choose("Bob");
    check(f.state().at("open").get<bool>(), "UI show remains open across a character transition");
    check(f.action("hide").value("ok", false), "final UI close succeeds");
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "UI close stays saved after earlier open preferences");
}

void damaged_window_preferences_do_not_reopen_or_change_alerts() {
    Fixture f; f.start();
    check(f.save(low_health_rule()).value("ok", false), "window preference fixture has a saved alert");
    const fs::path alert_path(f.state().at("config_path").get<std::string>());
    std::string alert_bytes;
    { std::ifstream in(alert_path, std::ios::binary); alert_bytes.assign(std::istreambuf_iterator<char>(in), {}); }
    f.stop();
    const fs::path ui_path = f.root / "global" / "ui-settings.json";
    const std::string damaged = "{window settings must not cause a popup loop}";
    { std::ofstream out(ui_path, std::ios::binary | std::ios::trunc); out << damaged; }
    f.start();
    check(!f.state().at("open").get<bool>(), "damaged window preferences fall back to a closed window");
    check(!f.state().value("error", std::string{}).empty(), "damaged window preferences report a useful error");
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "damaged preferences do not reopen on repeated reloads");
    f.commandline("");
    check(f.state().at("open").get<bool>(), "explicit /sentinel can reopen after damaged preferences");
    f.stop(); f.start();
    check(f.state().at("open").get<bool>(), "explicit open repairs and saves the visibility preference");
    std::ifstream in(alert_path, std::ios::binary);
    const std::string remaining((std::istreambuf_iterator<char>(in)), {});
    check(remaining == alert_bytes, "repairing window preferences leaves character alerts byte-for-byte intact");
}

void unwritable_first_use_does_not_create_a_popup_loop() {
    Fixture f;
    const fs::path blocker = f.root / "blocked-global";
    { std::ofstream out(blocker, std::ios::binary); out << "Keep this regular file"; }
    f.api.config_dir = [](void* h, char* out, size_t cap) {
        auto& fixture = Fixture::of(h); fixture.same_thread();
        // This directory cannot be created because an ancestor is a regular file.
        return copy_text((fixture.root / "blocked-global" / "sentinel").string(), out, cap);
    };
    f.start();
    check(!f.state().at("open").get<bool>(), "failed first-use preference write defaults to a closed window");
    check(!f.state().value("error", std::string{}).empty(), "unwritable preferences expose an error");
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "unwritable first-use preferences do not repeat an automatic popup");
    const auto shown = f.action("show");
    check(!shown.value("ok", true), "manual open reports failure to save its preference");
    check(f.state().at("open").get<bool>(), "manual open is honored immediately despite a failed preference write");
    const auto hidden = f.action("hide");
    check(!hidden.value("ok", true) && !f.state().at("open").get<bool>(), "manual close is honored despite a failed preference write");
    f.stop(); f.start();
    check(!f.state().at("open").get<bool>(), "failed manual preference writes still cannot cause startup popup loops");
    std::ifstream in(blocker, std::ios::binary);
    const std::string remaining((std::istreambuf_iterator<char>(in)), {});
    check(remaining == "Keep this regular file", "failed preference writes leave an existing blocking file intact");
}

void put_known_buff(Fixture& f,uint32_t slot,uint32_t id,const char* name,const char* spell_type) {
    f.player.buff_slots=std::max(f.player.buff_slots,slot+1);
    HailstoneBuff buff{};buff.size=sizeof(buff);buff.spell_id=id;buff.tics=10;f.buffs[slot]=buff;
    HailstoneSpell spell{};spell.size=sizeof(spell);spell.id=id;copy_text(name,spell.name,sizeof(spell.name));f.spells[id]=spell;
    f.parsed["${Spell["+std::to_string(id)+"].SpellType}"]=spell_type;
}
size_t chat_count(const Fixture& f,const std::string& text) {
    return static_cast<size_t>(std::count_if(f.chat.begin(),f.chat.end(),[&](const auto& line){return line.find(text)!=std::string::npos;}));
}
Json received_rule(const char* event,const char* message,const char* subject="") {
    auto rule=low_health_rule();rule["event"]=event;rule["message"]=message;rule["subject"]=subject;return rule;
}

void received_effects_use_known_spell_types() {
    Fixture f;f.player.buff_slots=8;
    put_known_buff(f,0,200,"Clarity","Beneficial");put_known_buff(f,1,201,"Sluggishness","Detrimental");f.start();
    check(f.save(received_rule("buff_received","BENEFICIAL_RECEIVED {L}")).value("ok",false),"beneficial-arrival rule saves with an optional empty name");
    check(f.save(received_rule("debuff_received","DETRIMENTAL_RECEIVED {L}")).value("ok",false),"detrimental-arrival rule saves with an optional empty name");
    check(f.save(received_rule("buff_received","FILTERED_GROUP_EFFECT","gRoUp AeGiS")).value("ok",false),"arrival rule accepts an exact name filter");
    f.chat.clear();f.tick();check(f.chat.empty(),"already active beneficial and detrimental effects establish a quiet baseline");
    put_known_buff(f,2,202,"Haste","Beneficial");put_known_buff(f,3,203,"Toxin","Detrimental");f.tick();
    check(chat_count(f,"BENEFICIAL_RECEIVED")==1&&chat_count(f,"DETRIMENTAL_RECEIVED")==1,"new beneficial and detrimental effects each choose their matching event");
    check(chat_count(f,"Haste")==1&&chat_count(f,"Toxin")==1,"arrival details identify the received spell");
    check(chat_count(f,"FILTERED_GROUP_EFFECT")==0,"unrelated buff names do not match a filtered arrival");
    const auto count=f.chat.size();f.buffs[2].tics=100;f.buffs[3].tics=100;f.tick();
    check(f.chat.size()==count,"refreshing existing spell IDs does not create another arrival");
    // The public SpellType TLO represents ordinary type1 and group-only type2 as Beneficial.
    put_known_buff(f,4,204,"Group Aegis","Beneficial");f.tick();
    check(chat_count(f,"FILTERED_GROUP_EFFECT")==1,"beneficial group effect matches its exact name without case sensitivity");
    check(chat_count(f,"DETRIMENTAL_RECEIVED")==1,"group-only beneficial representation does not become a detrimental event");
    f.buffs.erase(2);f.tick();put_known_buff(f,2,202,"Haste","Beneficial");f.tick();
    check(chat_count(f,"BENEFICIAL_RECEIVED")==3,"an effect removed in a complete poll can trigger when truly received again");
}

void received_effects_do_not_guess_or_replay_late_metadata() {
    Fixture f;f.player.buff_slots=8;f.start();
    check(f.save(received_rule("buff_received","KNOWN_BUFF")).value("ok",false),"unknown-metadata beneficial rule saves");
    check(f.save(received_rule("debuff_received","KNOWN_DEBUFF")).value("ok",false),"unknown-metadata detrimental rule saves");
    f.chat.clear();f.tick();
    put_known_buff(f,0,300,"Unknown venom","Detrimental");f.spells.erase(300);f.tick();
    check(f.chat.empty(),"TLO classification alone cannot classify a spell missing from get_spell");
    HailstoneSpell venom{};venom.size=sizeof(venom);venom.id=300;copy_text("Unknown venom",venom.name,sizeof(venom.name));f.spells[300]=venom;f.tick();
    check(f.chat.empty(),"discovering metadata for an already present unknown effect is not a fresh arrival");
    f.buffs.erase(0);f.tick();put_known_buff(f,0,300,"Unknown venom","Detrimental");f.tick();
    check(chat_count(f,"KNOWN_DEBUFF")==1,"known metadata works on a later actual detrimental arrival");
    put_known_buff(f,1,301,"Mismatched record","Beneficial");f.spells[301].id=999;f.tick();
    check(chat_count(f,"KNOWN_BUFF")==0,"get_spell returning the wrong spell ID cannot classify the requested effect");
    f.spells[301].id=301;f.tick();check(chat_count(f,"KNOWN_BUFF")==0,"repairing mismatched metadata while active remains quiet");
    put_known_buff(f,2,302,"Unresolved ward","1");f.tick();
    check(chat_count(f,"KNOWN_BUFF")==0,"numeric TLO values are not guessed as beneficial");
    f.parsed["${Spell[302].SpellType}"]="Beneficial";f.tick();
    check(chat_count(f,"KNOWN_BUFF")==0,"late exact classification does not replay an earlier unknown arrival");
    f.buffs.erase(2);f.tick();put_known_buff(f,2,302,"Unresolved ward","Beneficial");f.tick();
    check(chat_count(f,"KNOWN_BUFF")==1,"unknown classification is not cached permanently after it becomes available");
    put_known_buff(f,3,303,"Lowercase claim","beneficial");f.tick();
    check(chat_count(f,"KNOWN_BUFF")==1,"only the documented exact SpellType strings are accepted");
}

void incomplete_buff_polls_preserve_arrivals_and_missing_rules() {
    Fixture f;f.player.buff_slots=4;put_known_buff(f,0,400,"Clarity","Beneficial");f.start();
    check(f.save(received_rule("buff_received","RECEIVED_REQUIRED")).value("ok",false),"incomplete-poll arrival rule saves");
    auto missing=low_health_rule();missing["event"]="buff_missing";missing["subject"]="Clarity";missing["message"]="MISSING_REQUIRED";
    check(f.save(missing).value("ok",false),"incomplete-poll missing-buff rule saves");
    f.chat.clear();f.tick();check(f.chat.empty(),"present required buff establishes a quiet baseline");
    f.failed_buff_slots.insert(0);f.tick();check(f.chat.empty(),"unreadable occupied slot does not mean the required buff is missing");
    f.failed_buff_slots.clear();f.tick();check(f.chat.empty(),"recovered slot read does not fabricate a new receipt");
    f.player.buff_slots=0;f.tick();f.player.buff_slots=4;f.tick();
    check(f.chat.empty(),"zero advertised slots do not clear the previous complete buff baseline");
    f.player.buff_slots=513;f.tick();f.player.buff_slots=4;f.tick();
    check(f.chat.empty(),"oversized advertised slot counts do not create a new baseline");
    const auto get_buff=f.api.get_buff;f.api.get_buff=nullptr;f.tick();f.api.get_buff=get_buff;f.tick();
    check(f.chat.empty(),"temporarily unavailable get_buff preserves comparison state");
    f.buffs.erase(0);f.failed_buff_slots.insert(3);f.tick();
    check(f.chat.empty(),"an incomplete poll cannot prove absence even when another slot is the failed read");
    f.failed_buff_slots.clear();f.tick();check(chat_count(f,"MISSING_REQUIRED")==1,"a subsequent complete poll can prove a buff is missing");
    put_known_buff(f,0,400,"Clarity","Beneficial");f.tick();
    check(chat_count(f,"RECEIVED_REQUIRED")==1,"return after confirmed absence triggers one actual receipt");
}

void received_effect_classification_cache_is_zone_scoped() {
    Fixture f;f.player.buff_slots=4;f.start();
    check(f.save(received_rule("buff_received","ZONE_SCOPED_BUFF")).value("ok",false),"zone-scoped classification rule saves");
    f.chat.clear();f.tick();put_known_buff(f,0,500,"Zone ward","Beneficial");f.tick();
    check(chat_count(f,"ZONE_SCOPED_BUFF")==1,"known classification is used in its original zone");
    f.buffs.clear();f.tick();
    OnBeginZone();f.player.in_zone=0;SetGameState(GAMESTATE_LOGGINGIN);f.tick();
    f.player.zone_id=3;f.player.in_zone=1;SetGameState(GAMESTATE_INGAME);OnZoned();f.tick();
    put_known_buff(f,0,500,"Zone ward","Beneficial");f.spells[500].id=999;f.tick();
    check(chat_count(f,"ZONE_SCOPED_BUFF")==1,"classification from an earlier zone cannot override newly unavailable metadata");
    f.spells[500].id=500;f.tick();check(chat_count(f,"ZONE_SCOPED_BUFF")==1,"metadata restored after zoning does not replay an active effect");
    f.buffs.clear();f.tick();put_known_buff(f,0,500,"Zone ward","Beneficial");f.tick();
    check(chat_count(f,"ZONE_SCOPED_BUFF")==2,"new-zone classification becomes usable on a later actual receipt");
}

void timer_bars_save_layout_and_keep_independent_deadlines() {
    Fixture f;
    const auto old_dir = f.root / f.character;
    fs::create_directories(old_dir);
    const Json old_settings = {{"version", 1}, {"enabled", true}, {"alerts", Json::array({low_health_rule()})}};
    { std::ofstream out(old_dir / "config.json"); out << old_settings.dump(); }
    f.start();
    check(f.state().at("timers").empty() && f.count() == 1, "older settings keep alerts and start with an empty timer list");
    Json draft = {{"name", " mez refresh "}, {"duration", 6}, {"color", "purple"}, {"alpha", 70},
        {"warning_at", 4}, {"warning_color", "yellow"}, {"critical_at", 2}, {"critical_color", "red"}};
    auto save = [&](const Json& timer) { return f.call("timer_save", Json::array({timer})); };
    auto action = [&](const std::string& op, const std::string& id = "") {
        return f.call("timer_action", Json::array({op, id})).value("ok", false);
    };
    const auto first = save(draft).at("id").get<std::string>();
    auto other = draft;
    other["name"] = "Pull in";
    other["duration"] = 20;
    const auto second = save(other).at("id").get<std::string>();
    check(first != second, "timer definitions have distinct IDs");
    auto read = [&](const std::string& id) {
        const auto timers = f.state().at("timers");
        const auto it = std::find_if(timers.begin(), timers.end(), [&](const Json& timer) { return timer.at("id") == id; });
        check(it != timers.end(), "timer exists in SDK state");
        return *it;
    };
    auto layout = [&](double x, double y, double width, double height, bool minimized) {
        return f.call("timer_layout", Json::array({first, x, y, width, height, minimized})).value("ok", false);
    };
    check(read(first).at("name") == "mez refresh", "display name trims surrounding spaces");
    check(read(first).at("y") != read(second).at("y"), "new bars start in different positions");
    check(layout(.2, .3, 320, 90, false), "save a drag and resize through SDK layout");
    check(f.state().at("timers_grouped") == false, "separate bars are the default layout");
    check(action("group") && f.state().at("timers_grouped") == true, "grouped mode is visible to the HUD");
    check(f.call("timer_action", Json::array({"group_alpha", 58})).value("ok", false), "save independent group pane opacity");
    check(!f.call("timer_action", Json::array({"group_alpha", 0})).value("ok", true), "reject invisible pane opacity");
    check(f.call("timer_layout", Json::array({"group", .5, .4, 400, 410, false})).value("ok", false), "move and resize the group pane");
    check(action("separate") && read(first).at("x") == .2 && read(first).at("width") == 320,
        "switching views keeps each individual bar position and size");
    check(action("group"), "remember grouped mode across reload");
    check(action("lock") && f.state().at("timers_locked") == true, "lock setting is visible to overlay");
    check(layout(.8, .6, 480, 100, true), "locked timers can be minimized");
    auto state = read(first);
    check(state.at("x") == .2 && state.at("y") == .3 && state.at("width") == 320 && state.at("height") == 90 &&
        state.at("minimized") == true, "locked layout keeps its geometry and saves minimize state");
    check(action("start", first) && action("start", second), "start two independent timers");
    f.tick();
    check(read(first).at("remaining") == 5 && read(second).at("remaining") == 19, "countdowns use separate deadlines");
    draft["id"] = first;
    draft["duration"] = 8;
    check(save(draft).value("ok", false), "edit a running timer");
    state = read(first);
    check(state.at("duration") == 8 && state.at("run_duration") == 6 && state.at("remaining") == 5,
        "an edit preserves the deadline and original duration of the current run");
    check(action("start", first) && read(first).at("remaining") == 8, "restart uses the saved duration");
    check(f.action("off").value("ok", false) && read(first).at("running") == true, "pausing automatic alerts leaves manual timers running");

    const auto alice_profile = f.state().at("timer_profile");
    OnBeginZone();
    f.player.in_zone = 0;
    SetGameState(GAMESTATE_LOGGINGIN);
    f.tick();
    check(!action("stop", first), "commands cannot change a stale character overlay in transit");
    f.player.zone_id = 3;
    f.player.in_zone = 1;
    SetGameState(GAMESTATE_INGAME);
    OnZoned();
    f.tick();
    check(f.state().at("timer_profile") == alice_profile && read(first).at("remaining") == 6,
        "zoning preserves the current character countdown and accounts for transit time");
    for (int i = 0; i < 6; ++i) f.tick();
    state = read(first);
    check(state.at("running") == false && state.at("finished") == true && state.at("remaining") == 0,
        "a deadline transitions to finished at zero");
    check(read(second).at("running") == true, "finishing a timer does not stop another");
    check(action("stop", first) && read(first).at("finished") == false, "stopping clears the finished display");

    other["duration"] = 1.5;
    check(!save(other).value("ok", true), "fractional durations are invalid");
    other["duration"] = 20;
    other["critical_at"] = 4;
    check(!save(other).value("ok", true) && f.state().at("timers").size() == 2, "invalid thresholds do not change settings");
    f.choose("Bob");
    check(f.state().at("timers").empty() && f.state().at("timer_profile") != alice_profile, "character changes isolate timers");
    f.choose("Alice");
    check(read(second).at("running") == false && f.state().at("timers_locked") == true,
        "returning to a character restores settings with idle timers");
    check(action("start", first), "start before reload");
    f.stop();
    f.start();
    state = read(first);
    check(state.at("x") == .2 && state.at("width") == 320 && state.at("minimized") == true && state.at("alpha") == 70 &&
        state.at("running") == false && state.at("duration") == 8, "reload restores layout and duration but drops runtime state");
    check(f.count() == 1, "saving timer settings preserves the character's existing alerts");
    const auto grouped = f.state();
    check(grouped.at("timers_grouped") == true && grouped.at("timer_group_layout").at("x") == .5 &&
        grouped.at("timer_group_layout").at("height") == 410 && grouped.at("timer_group_layout").at("alpha") == 58,
        "reload restores the separate group pane geometry, opacity and mode");
    check(action("delete", first) && !action("start", first) && f.state().at("timers").size() == 1, "deleting a timer removes it from state and actions");
}

void chat_can_start_a_timer_and_complete_a_second_rule() {
    Fixture f;
    f.start();
    Json timer = {{"name", "Gate check"}, {"duration", 3}, {"color", "cyan"}, {"warning_at", 0},
        {"warning_color", "yellow"}, {"critical_at", 0}, {"critical_color", "red"}};
    const auto gate = f.call("timer_save", Json::array({timer})).at("id").get<std::string>();
    timer["name"] = "Prepare";
    const auto prepare = f.call("timer_save", Json::array({timer})).at("id").get<std::string>();
    const Json text_trigger = {{"name", "Encounter call"}, {"event", "chat_text_match"}, {"subject", "*open the gates*"},
        {"chat", false}, {"overlay", false}, {"sound", false}, {"timer_id", gate}, {"cooldown", 0}};
    const auto trigger_id = f.save(text_trigger).at("id").get<std::string>();
    const Json finish_alert = {{"name", "Gate open"}, {"event", "timer_remaining"}, {"subject", gate}, {"threshold", 0},
        {"chat", true}, {"overlay", true}, {"sound", true}, {"cooldown", 0}};
    const auto finish_id = f.save(finish_alert).at("id").get<std::string>();
    auto two_seconds_left = finish_alert;
    two_seconds_left["name"] = "Prepare on approach";
    two_seconds_left["threshold"] = 2;
    two_seconds_left["message"] = "Prepare now";
    two_seconds_left["timer_id"] = prepare;
    check(f.save(two_seconds_left).value("ok", false), "a timer condition can also start a different timer");
    auto cycle = two_seconds_left;
    cycle["subject"] = prepare;
    cycle["timer_id"] = gate;
    check(!f.save(cycle).value("ok", true), "indirect timer cycles are rejected");
    cycle["timer_id"] = prepare;
    check(!f.save(cycle).value("ok", true), "direct timer cycles are rejected");

    f.chat.clear();
    OnIncomingChat("Guard Bob says, 'Open the gates!'", 0);
    auto state = f.state();
    check(state.at("timers")[0].at("running") == true && state.at("timers")[1].at("running") == false && f.chat.empty(),
        "a chat rule can start a named timer as its only result");
    f.tick();
    check(chat_count(f, "Prepare now") == 1 && f.state().at("timers")[1].at("running") == true,
        "crossing a nonzero threshold delivers a result and starts the chained timer");
    check(f.action("test", trigger_id).value("ok", false) && f.state().at("timers")[0].at("remaining") == 2,
        "preview never restarts an active live countdown");
    f.tick();
    check(chat_count(f, "Gate check finished.") == 0, "zero-second trigger waits until the deadline");
    f.tick();
    state = f.state();
    check(chat_count(f, "Gate check finished.") == 1 && state.at("timers")[0].at("finished") == true &&
        state.at("overlays").back().at("message") == "Gate check finished.", "completion goes through usual chat and banner delivery");
    f.tick();
    OnBeginZone(); f.player.in_zone = 0; SetGameState(GAMESTATE_LOGGINGIN); f.tick();
    f.player.in_zone = 1; f.player.zone_id = 3; SetGameState(GAMESTATE_INGAME); OnZoned(); f.tick();
    check(chat_count(f, "Gate check finished.") == 1 && chat_count(f, "Prepare now") == 1,
        "old crossings are not replayed during zoning");
    OnIncomingChat("Guard Bob says, 'Open the gates!'", 0);
    f.tick(); f.tick(); f.tick();
    check(chat_count(f, "Gate check finished.") == 2 && chat_count(f, "Prepare now") == 2, "new runs rearm both thresholds");

    check(f.action("test", trigger_id).value("ok", false) && f.state().at("timers")[0].at("running") == true,
        "preview demonstrates an idle timer result");
    f.tick(); f.tick(); f.tick();
    check(chat_count(f, "Gate check finished.") == 2 && chat_count(f, "Prepare now") == 2,
        "preview countdowns do not trigger ordinary timer conditions");
    OnIncomingChat("Guard Bob says, 'Open the gates!'", 0);
    check(f.action("off").value("ok", false), "pause rules with a countdown active");
    f.tick(); f.tick(); f.tick();
    check(f.action("on").value("ok", false), "resume rules after countdown elapsed");
    f.tick();
    check(chat_count(f, "Gate check finished.") == 2, "paused completions are consumed without later notification");

    const auto shared = f.call("export", Json::array({trigger_id}));
    const auto inspected = f.call("inspect", Json::array({shared}));
    check(inspected.at("alerts")[0].at("timer_id") == "unassigned-timer", "import review clears local-only timer bindings");
    check(f.call("import", Json::array({shared})).value("ok", false), "import accepts a timer-only result for re-selection");
    const auto imported_id = f.state().at("alerts").back().at("id").get<std::string>();
    check(!f.action("toggle_alert", imported_id).value("ok", true), "unbound imported rules cannot be enabled");
    check(f.call("timer_action", Json::array({"delete", gate})).value("ok", false), "remove timer used by saved rules");
    state = f.state();
    check(state.at("alerts")[0].at("timer_id") == "unassigned-timer" && state.at("alerts")[0].at("enabled") == false &&
        state.at("alerts")[1].at("subject") == "unassigned-timer" && state.at("alerts")[1].at("enabled") == false,
        "deleting a linked timer atomically disables and unbinds both kinds of rule");
    check(!f.action("toggle_alert", finish_id).value("ok", true), "a removed timer condition must be re-selected before enabling");
}
} // namespace

int main() {
    try {
        actions_and_persistence(); sharing_and_validation(); evaluation_and_lifecycle(); snapshot_adapter(); damaged_settings_are_preserved(); incomplete_rules_cannot_be_enabled(); window_visibility_survives_zoning_and_reload(); damaged_window_preferences_do_not_reopen_or_change_alerts(); unwritable_first_use_does_not_create_a_popup_loop();
        received_effects_use_known_spell_types();received_effects_do_not_guess_or_replay_late_metadata();incomplete_buff_polls_preserve_arrivals_and_missing_rules();received_effect_classification_cache_is_zone_scoped();
        timer_bars_save_layout_and_keep_independent_deadlines();
        chat_can_start_a_timer_and_complete_a_second_rule();
        std::cout << "Plugin integration checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Plugin integration test failed after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
