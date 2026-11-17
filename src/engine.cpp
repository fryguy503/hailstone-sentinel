// SPDX-License-Identifier: MIT
#include "engine.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sentinel {
namespace {
struct EventDef { const char *id, *label, *group, *subject, *threshold, *help; };
constexpr EventDef definitions[] = {
    {"health_below", "My health is low", "Survival", "", "Health (%)", "Warn once when your health reaches this percentage. Recover by more than 3% to rearm."},
    {"mana_below", "My mana is low", "Survival", "", "Mana (%)", "Only evaluates when a valid mana reading is available."},
    {"endurance_below", "My endurance is low", "Survival", "", "Endurance (%)", "Watch your endurance without repeating while it stays low."},
    {"player_health_below", "A player's health is low", "Party", "Exact player name", "Health (%)", "Watch a named player in your current zone. Unknown health never counts as zero."},
    {"player_mana_below", "A player's mana is low", "Party", "Exact player name", "Mana (%)", "Remote mana must be supplied by the server; unavailable readings are ignored."},
    {"pet_health_below", "My pet's health is low", "Party", "", "Health (%)", "Watch your current pet. No pet means no health warning."},
    {"target_health_below", "My target's health is low", "Combat", "", "Health (%)", "Each target rearms independently."},
    {"group_health_below", "A group member's health is low", "Party", "Player name (optional)", "Health (%)", "Watch other group members, or enter one exact name. Each member rearms independently."},
    {"buff_fading", "A buff is about to fade", "Buffs", "Exact buff name", "Seconds remaining", "Warn when a known, timed buff reaches this many seconds remaining."},
    {"buff_missing", "A required buff is missing", "Buffs", "Exact buff name", "", "Warn once while the buff is missing; it rearms when the buff returns."},
    {"buff_received", "I receive a buff", "Buffs", "Exact spell name (optional)", "", "Watch newly gained beneficial effects. Leave the name empty for any buff. Existing effects on login, zoning or enabling this rule, and timer refreshes, stay quiet."},
    {"debuff_received", "I receive a debuff", "Buffs", "Exact spell name (optional)", "", "Watch newly gained harmful effects. Leave the name empty for any debuff. Existing effects on login, zoning or enabling this rule, and timer refreshes, stay quiet."},
    {"spawn_appears", "An NPC appears nearby", "Awareness", "Exact NPC name", "Range (units)", "Watch an exact NPC name within this distance. Corpses are excluded."},
    {"player_appears", "A player appears nearby", "Awareness", "Exact player name", "Range (units)", "Watch an exact player name within this distance."},
    {"target_cast", "My target begins casting", "Combat", "Exact spell name (optional)", "", "Detect a new observed cast on your target; leave the spell name empty for any spell."},
    {"incoming_tell", "I receive a tell", "Messages", "Text filter (optional)", "", "Match received tells, optionally containing text or matching a wildcard pattern."},
    {"raid_message", "I receive raid chat", "Messages", "Text filter (optional)", "", "Match raid chat delivered by the client."},
    {"guild_message", "I receive guild chat", "Messages", "Text filter (optional)", "", "Match guild chat delivered by the client."},
    {"fellowship_message", "I receive fellowship chat", "Messages", "Text filter (optional)", "", "Match fellowship chat delivered by the client."},
    {"missed_note_or_fizzle", "I fizzle or miss a note", "Combat", "Text filter (optional)", "", "Recognize your spell-fizzle and missed-bard-note messages."},
    {"chat_text_match", "Chat matches a pattern", "Messages", "Full-line wildcard pattern", "", "Use * for any text, ? for one character, {S1} for text, and {N1>=1000} for a number."},
    {"invisibility_fading", "My invisibility starts fading", "Buffs", "Text filter (optional)", "", "Recognize the client's invisibility-fading warning messages."},
    {"group_message", "I receive group chat", "Messages", "Text filter (optional)", "", "Match group chat delivered by the client."},
    {"spell_resisted", "My spell is resisted", "Combat", "Text filter (optional)", "", "Recognize resist messages for your own spells."},
    {"pet_lost", "My pet disappears", "Party", "", "", "Warn after a present pet disappears. Logging in without a pet is quiet."}
};

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    return s;
}
bool equal(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
bool starts(const std::string& a, const std::string& b) { return lower(a).rfind(lower(b), 0) == 0; }
// Hailstone 0.7's native chat callback exposes the RGB override rather than the
// channel colour. Recognize only complete English social-message envelopes;
// never search the message body for a channel phrase. The raw colour path below
// remains available for hosts that expose channel colours directly.
std::string social_event(const std::string& text) {
    const size_t space = text.find(' ');
    if (space == std::string::npos || space == 0 || space > 64 || text.size() < space + 5 || text.back() != '\'') return {};
    const std::string sender = text.substr(0, space);
    if (equal(sender, "You")) return {};
    bool last_dot = true;
    size_t dots = 0;
    for (char c : sender) {
        if (c == '.') { if (last_dot || ++dots > 1) return {}; last_dot = true; }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) last_dot = false;
        else return {};
    }
    if (last_dot) return {};
    const std::string tail = lower(text.substr(space));
    struct Marker { const char* text; const char* event; };
    constexpr Marker markers[] = {{" tells you,", "incoming_tell"}, {" tells the group,", "group_message"},
        {" tells the guild,", "guild_message"}, {" tells the raid,", "raid_message"}, {" tells the fellowship,", "fellowship_message"}};
    for (const auto& marker : markers) {
        const std::string prefix = marker.text;
        if (tail.rfind(prefix, 0) != 0) continue;
        size_t at = prefix.size();
        while (at < tail.size() && tail[at] == ' ') ++at;
        if (tail.compare(at, 3, "in ") == 0) {
            const size_t comma = tail.find(',', at + 3);
            if (comma == std::string::npos || comma - at > 80 || comma == at + 3) return {};
            for (size_t i = at + 3; i < comma; ++i) {
                const char c = tail[i];
                if (!((c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '\'')) return {};
            }
            at = comma + 1;
            while (at < tail.size() && tail[at] == ' ') ++at;
        }
        if (at < tail.size() - 1 && tail[at] == '\'') return marker.event;
        return {};
    }
    return {};
}
std::string clean(std::string s, size_t limit) {
    if (s.size() > limit) {
        size_t boundary = limit;
        // JSON and Lua strings are UTF-8. Keep the byte limit without leaving
        // half a code point that would make the next JSON save invalid.
        while (boundary > 0 && (static_cast<unsigned char>(s[boundary]) & 0xc0u) == 0x80u) --boundary;
        s.resize(boundary);
    }
    for (char& c : s) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return s;
}
std::string str(const Json& j, const char* key, std::string fallback = {}, size_t limit = 512) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? clean(it->get<std::string>(), limit) : fallback;
}
double num(const Json& j, const char* key, double fallback, double low, double high) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const double n = it->get<double>();
    return std::isfinite(n) ? std::clamp(n, low, high) : fallback;
}
bool boolean(const Json& j, const char* key, bool fallback) {
    auto it = j.find(key); return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
const EventDef* find_event(const std::string& id) {
    for (const auto& e : definitions) if (id == e.id) return &e;
    return nullptr;
}
bool percentage(const std::string& e) { return e.size() >= 6 && e.compare(e.size() - 6, 6, "_below") == 0; }
bool buff_arrival(const std::string& e) { return e == "buff_received" || e == "debuff_received"; }
bool requires_subject(const std::string& e) {
    return e == "player_health_below" || e == "player_mana_below" || e == "buff_fading" ||
        e == "buff_missing" || e == "spawn_appears" || e == "player_appears" || e == "chat_text_match";
}
bool known(const std::optional<double>& x) { return x && std::isfinite(*x) && *x >= 0 && *x <= 100; }
std::string number(double x) {
    std::ostringstream out; out << std::fixed << std::setprecision(x == std::floor(x) ? 0 : 1) << x; return out.str();
}

struct Token {
    enum Kind { Literal, Any, One, Numeric } kind = Literal;
    std::string text, capture, op;
    double value = 0;
};
bool parse_number(std::string text, double& value) {
    text.erase(std::remove(text.begin(), text.end(), ','), text.end());
    if (text.empty()) return false;
    size_t at = 0;
    try { value = std::stod(text, &at); } catch (...) { return false; }
    return at == text.size() && std::isfinite(value);
}
std::vector<Token> tokenize(const std::string& pattern, const std::string& character) {
    std::vector<Token> out;
    std::string literal;
    auto flush = [&] { if (!literal.empty()) { out.push_back({Token::Literal, lower(literal), {}, {}, 0}); literal.clear(); } };
    for (size_t at = 0; at < pattern.size();) {
        if (pattern[at] == '*' || pattern[at] == '?') {
            flush(); out.push_back({pattern[at] == '*' ? Token::Any : Token::One, {}, {}, {}, 0}); ++at; continue;
        }
        if (pattern[at] == '{') {
            const auto end = pattern.find('}', at + 1);
            if (end != std::string::npos) {
                const std::string tag = upper(pattern.substr(at + 1, end - at - 1));
                if (tag == "C") { literal += character; at = end + 1; continue; }
                size_t n = 1;
                while (n < tag.size() && tag[n] >= '0' && tag[n] <= '9') ++n;
                if (!tag.empty() && tag[0] == 'S' && n == tag.size()) {
                    flush(); out.push_back({Token::Any, {}, tag, {}, 0}); at = end + 1; continue;
                }
                if (!tag.empty() && tag[0] == 'N') {
                    Token token; token.kind = Token::Numeric; token.capture = tag.substr(0, n);
                    bool valid = n == tag.size();
                    if (!valid) {
                        for (const char* op : {">=", "<=", "!=", ">", "<", "="}) {
                            const std::string operation(op);
                            if (tag.compare(n, operation.size(), operation) == 0 &&
                                parse_number(tag.substr(n + operation.size()), token.value)) {
                                token.op = operation; valid = true; break;
                            }
                        }
                    }
                    if (valid) { flush(); out.push_back(std::move(token)); at = end + 1; continue; }
                }
            }
        }
        literal += pattern[at++];
    }
    flush(); return out;
}
bool compare(double n, const Token& t) {
    if (t.op == "=") return n == t.value;
    if (t.op == "!=") return n != t.value;
    if (t.op == ">") return n > t.value;
    if (t.op == ">=") return n >= t.value;
    if (t.op == "<") return n < t.value;
    if (t.op == "<=") return n <= t.value;
    return true;
}
bool match(const std::string& input, const std::string& pattern, const std::string& character,
           std::map<std::string, std::string>& captures) {
    if (pattern.empty() || pattern.size() > 512 || input.size() > 2048) return false;
    const auto tokens = tokenize(pattern, character);
    if (tokens.size() > 128) return false;
    const std::string text = lower(input);
    size_t budget = 30000;
    std::function<bool(size_t, size_t)> visit = [&](size_t ti, size_t at) -> bool {
        if (budget == 0) return false;
        --budget;
        if (ti == tokens.size()) return at == text.size();
        const Token& t = tokens[ti];
        if (t.kind == Token::Literal) return text.compare(at, t.text.size(), t.text) == 0 && visit(ti + 1, at + t.text.size());
        if (t.kind == Token::One) return at < text.size() && visit(ti + 1, at + 1);
        size_t end = text.size();
        if (t.kind == Token::Numeric) {
            end = at;
            if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
            bool decimal = false;
            while (end < text.size()) {
                const char c = text[end];
                if ((c >= '0' && c <= '9') || c == ',') ++end;
                else if (c == '.' && !decimal) { decimal = true; ++end; }
                else break;
            }
            if (end == at) return false;
        }
        const auto old = captures.find(t.capture);
        const bool had_old = old != captures.end();
        const std::string old_value = had_old ? old->second : "";
        for (size_t candidate = end;; --candidate) {
            if (budget == 0) break;
            double n = 0;
            if (t.kind != Token::Numeric || (candidate > at && parse_number(input.substr(at, candidate - at), n) && compare(n, t))) {
                if (!t.capture.empty()) captures[t.capture] = input.substr(at, candidate - at);
                if (visit(ti + 1, candidate)) return true;
            }
            if (candidate == at) break;
        }
        if (!t.capture.empty()) { if (had_old) captures[t.capture] = old_value; else captures.erase(t.capture); }
        return false;
    };
    const bool result = visit(0, 0);
    if (!result) captures.clear();
    return result;
}
std::string expand(const std::string& pattern, const Snapshot& s, const std::string& line, uint64_t counter,
                   const std::map<std::string, std::string>& captures) {
    std::string out;
    for (size_t at = 0; at < pattern.size() && out.size() < 512;) {
        if (pattern[at] != '{') { out += pattern[at++]; continue; }
        const auto end = pattern.find('}', at + 1);
        if (end == std::string::npos) { out += pattern[at++]; continue; }
        std::string tag = upper(pattern.substr(at + 1, end - at - 1));
        const auto comparison = tag.find_first_of("<>=!");
        if (comparison != std::string::npos) tag.resize(comparison);
        if (tag == "C") out += s.character;
        else if (tag == "L") out += line;
        else if (tag == "COUNTER") out += std::to_string(counter);
        else if (auto it = captures.find(tag); it != captures.end()) out += it->second;
        else out += pattern.substr(at, end - at + 1);
        at = end + 1;
    }
    return clean(out, 512);
}
} // namespace

Json defaults() { return {{"version", 1}, {"enabled", true}, {"alerts", Json::array()}}; }
Json events() {
    Json out = Json::array();
    for (const auto& e : definitions) out.push_back({{"id", e.id}, {"label", e.label}, {"group", e.group},
        {"subject_label", e.subject}, {"threshold_label", e.threshold}, {"help", e.help}});
    for (auto& e : out) {
        const auto id = e["id"].get<std::string>();
        if (id == "incoming_tell" || id == "group_message" || id == "guild_message" || id == "raid_message" || id == "fellowship_message")
            e["help"] = e["help"].get<std::string>() + " On Hailstone 0.7, channel detection uses the English game-message format.";
        else if (id == "spell_resisted" || id == "missed_note_or_fizzle" || id == "invisibility_fading")
            e["help"] = e["help"].get<std::string>() + " Recognition uses English game messages.";
    }
    return out;
}
Json normalize_alert(const Json& input) {
    if (!input.is_object()) throw std::invalid_argument("Each alert must be an object.");
    const std::string e = str(input, "event", "health_below", 48);
    if (!find_event(e)) throw std::invalid_argument("Unknown alert event: " + e);
    const double maximum = percentage(e) ? 100 : e == "buff_fading" ? 36000 : 10000;
    const double minimum = percentage(e) || e == "buff_fading" || e == "spawn_appears" || e == "player_appears" ? 1 : 0;
    const double threshold = num(input, "threshold", percentage(e) ? 30 : e == "buff_fading" ? 60 : buff_arrival(e) ? 0 : 250, minimum, maximum);
    std::string color = str(input, "color", "cyan", 16), tone = str(input, "tone", "chime", 16);
    if (std::set<std::string>{"red", "orange", "yellow", "green", "cyan", "blue", "purple", "white"}.count(color) == 0) color = "cyan";
    if (std::set<std::string>{"chime", "alert", "critical", "question", "default"}.count(tone) == 0) tone = "chime";
    std::string sound = str(input, "sound_file", "", 96);
    if (sound.find_first_of("/\\:") != std::string::npos || sound.find("..") != std::string::npos ||
        (!sound.empty() && (sound.size() < 5 || lower(sound.substr(sound.size() - 4)) != ".wav"))) sound.clear();
    Json out = {{"id", str(input, "id", "", 80)}, {"name", str(input, "name", find_event(e)->label, 96)},
        {"event", e}, {"enabled", boolean(input, "enabled", true)}, {"category", str(input, "category", find_event(e)->group, 48)},
        {"section", str(input, "section", "General", 64)}, {"subsection", str(input, "subsection", "", 64)},
        {"zone_id", static_cast<uint32_t>(num(input, "zone_id", 0, 0, 65535))}, {"subject", str(input, "subject", "", 512)},
        {"threshold", threshold}, {"message", str(input, "message", "", 512)}, {"cooldown", num(input, "cooldown", 10, 0, 3600)},
        {"chat", boolean(input, "chat", true)}, {"overlay", boolean(input, "overlay", true)},
        {"sound", boolean(input, "sound", true)}, {"speech", boolean(input, "speech", false)},
        {"volume", num(input, "volume", 75, 0, 100)}, {"tone", tone}, {"sound_file", sound},
        {"speech_text", str(input, "speech_text", "", 512)}, {"voice", static_cast<int>(num(input, "voice", 0, 0, 1))},
        {"rate", static_cast<int>(num(input, "rate", 0, -10, 10))}, {"color", color},
        {"size", static_cast<int>(num(input, "size", 2, 0, 4))}, {"duration", num(input, "duration", 4, .5, 30)}};
    if (out["name"].get<std::string>().empty()) out["name"] = find_event(e)->label;
    if (out["category"].get<std::string>().empty()) out["category"] = "General";
    if (out["section"].get<std::string>().empty()) out["section"] = "General";
    return out;
}
Json normalize_config(const Json& input) {
    if (!input.is_object()) throw std::invalid_argument("Settings must be an object.");
    Json out = defaults(); out["enabled"] = boolean(input, "enabled", true);
    for (const char* key : {"overlay_x", "overlay_y"}) {
        auto it = input.find(key);
        if (it != input.end() && it->is_number() && std::isfinite(it->get<double>())) out[key] = *it;
    }
    for (const char* key : {"locked", "quiet"}) {
        auto it = input.find(key);
        if (it != input.end() && it->is_boolean()) out[key] = *it;
    }
    auto alerts = input.find("alerts");
    if (alerts != input.end() && !alerts->is_array()) throw std::invalid_argument("Alerts must be a list.");
    std::set<std::string> ids;
    if (alerts != input.end()) {
        if (alerts->size() > 128) throw std::invalid_argument("A library can contain at most 128 alerts.");
        size_t index = 0;
        for (const auto& item : *alerts) {
            Json a = normalize_alert(item);
            std::string id = a["id"];
            if (id.empty() || ids.count(id)) {
                do { id = "alert-" + std::to_string(++index); } while (ids.count(id));
                a["id"] = id;
            }
            ids.insert(id); out["alerts"].push_back(std::move(a));
        }
    }
    return out;
}
std::string describe(const Json& input) {
    const Json a = normalize_alert(input);
    const std::string e = a["event"], subject = a["subject"];
    std::string text = find_event(e)->label;
    if (!subject.empty()) text += " (" + subject + ")";
    if (percentage(e)) text += " at " + number(a["threshold"].get<double>()) + "%";
    else if (e == "buff_fading") text += " within " + number(a["threshold"].get<double>()) + " seconds";
    else if (e == "spawn_appears" || e == "player_appears") text += " within " + number(a["threshold"].get<double>()) + " units";
    if (a["zone_id"].get<uint32_t>()) text += " in zone " + std::to_string(a["zone_id"].get<uint32_t>());
    return text;
}
std::string export_alerts(const Json& alerts) {
    if (!alerts.is_array() || alerts.size() > 128) throw std::invalid_argument("Choose up to 128 alerts to export.");
    Json normalized = Json::array();
    for (const auto& a : alerts) normalized.push_back(normalize_alert(a));
    const std::string text = Json{{"format", "hailstone-sentinel"}, {"version", 1}, {"alerts", normalized}}.dump(2);
    if (text.size() > 128 * 1024) throw std::invalid_argument("This export exceeds 128 KiB. Export a smaller section.");
    return text;
}
Json parse_import(std::string text) {
    if (text.size() > 128 * 1024) throw std::invalid_argument("Import is limited to 128 KiB.");
    Json input;
    try {
        input = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 12) throw std::invalid_argument("Sentinel imports cannot contain deeply nested data.");
            return true;
        });
    } catch (const Json::exception&) { throw std::invalid_argument("This is not valid Sentinel JSON."); }
    Json alerts;
    if (input.is_array()) alerts = input;
    else if (input.is_object() && input.value("format", Json()) == "hailstone-sentinel" && input.value("version", Json()) == 1)
        alerts = input.value("alerts", Json());
    else throw std::invalid_argument("Use a Sentinel version 1 export.");
    if (!alerts.is_array() || alerts.empty() || alerts.size() > 128) throw std::invalid_argument("Import must contain 1 to 128 alerts.");
    Json out = Json::array();
    for (const auto& a : alerts) {
        if (!a.is_object() || !a.contains("event") || !a["event"].is_string() || !find_event(a["event"].get<std::string>()))
            throw std::invalid_argument("An imported alert has an unknown or missing event.");
        Json rule = normalize_alert(a);
        const std::string event = rule["event"];
        if (requires_subject(event) && rule["subject"].get<std::string>().empty()) throw std::invalid_argument("An imported alert needs a subject or pattern.");
        if (!rule["chat"].get<bool>() && !rule["overlay"].get<bool>() && !rule["sound"].get<bool>() && !rule["speech"].get<bool>())
            throw std::invalid_argument("An imported alert has no notification outputs.");
        out.push_back(std::move(rule));
    }
    return out;
}

Json presets() {
    Json out = Json::array();
    auto add = [&](const char* id, const char* name, const char* event, double threshold, const char* color, const char* message, const char* description,
                   const char* subject = "", const char* tone = "chime", double cooldown = 10) {
        Json a = normalize_alert({{"id", id}, {"name", name}, {"event", event}, {"threshold", threshold}, {"color", color},
            {"message", message}, {"subject", subject}, {"tone", tone}, {"cooldown", cooldown}});
        a["description"] = description; out.push_back(std::move(a));
    };
    add("critical-health", "Critical health", "health_below", 15, "red", "HEAL ME NOW", "A clear emergency warning at 15% health.", "", "critical", 5);
    add("low-health", "Low health", "health_below", 30, "orange", "Health is low", "Get an earlier warning at 30% health.", "", "alert");
    add("early-health", "Early health warning", "health_below", 50, "yellow", "", "A gentle heads-up when health reaches halfway.");
    add("low-mana", "Low mana", "mana_below", 20, "cyan", "LOW MANA", "Know when your mana reserve is running low.", "", "alert", 15);
    add("early-mana", "Mana halfway", "mana_below", 50, "cyan", "", "Plan ahead when mana reaches 50%.");
    add("low-endurance", "Low endurance", "endurance_below", 20, "yellow", "LOW ENDURANCE", "Save enough endurance for your next ability.");
    add("tank-health", "Watch the tank", "player_health_below", 50, "red", "HEAL TANK", "Enter your tank's exact name to watch their health.", "", "critical", 5);
    add("player-critical", "Player emergency", "player_health_below", 25, "red", "HEAL PLAYER", "Watch one player's health at an emergency threshold.", "", "critical", 5);
    add("healer-mana", "Watch healer mana", "player_mana_below", 30, "cyan", "HEALER LOW MANA", "Enter a healer's name. Requires available remote mana data.", "", "alert", 15);
    add("pet-health", "Protect my pet", "pet_health_below", 35, "orange", "HEAL PET", "Warn when your pet reaches 35% health.", "", "alert");
    add("execute", "Execute range", "target_health_below", 20, "yellow", "EXECUTE", "A target-specific heads-up at 20% health.");
    add("group-critical", "Group emergency", "group_health_below", 35, "red", "GROUP MEMBER LOW", "Watch every other group member independently.", "", "critical", 5);
    add("group-member", "Watch a group member", "group_health_below", 40, "red", "GROUP MEMBER LOW", "Enter an exact name to watch one member, or leave it empty for everyone.", "", "alert", 5);
    add("buff-soon", "Buff fading soon", "buff_fading", 120, "yellow", "BUFF FADING", "Enter a buff's exact name for a two-minute warning.", "", "question", 30);
    add("buff-refresh", "Refresh a buff", "buff_fading", 30, "orange", "REFRESH BUFF", "Enter a buff's exact name for a 30-second warning.", "", "alert", 15);
    add("buff-missing", "Required buff missing", "buff_missing", 0, "purple", "BUFF MISSING", "Enter a buff that should always be active.", "", "question", 30);
    add("buff-received", "Buff received", "buff_received", 0, "cyan", "", "See the names of newly received beneficial effects. Existing effects and refreshes stay quiet.", "", "chime", 3);
    add("debuff-received", "Debuff received", "debuff_received", 0, "red", "", "Notice newly received detrimental effects by name. Existing effects and refreshes stay quiet.", "", "alert", 5);
    add("named-npc", "Named NPC nearby", "spawn_appears", 250, "yellow", "NAMED NPC NEARBY", "Enter an exact NPC name to watch within 250 units.", "", "alert", 30);
    add("nearby-player", "Player nearby", "player_appears", 250, "cyan", "PLAYER NEARBY", "Enter a player's exact name to watch within 250 units.", "", "chime", 30);
    add("interrupt", "Interrupt a spell", "target_cast", 0, "red", "INTERRUPT CAST", "Enter an exact spell name, or leave it empty for any cast.", "", "critical", 3);
    add("target-cast", "Target casting", "target_cast", 0, "orange", "", "Watch each newly observed cast on your target.", "", "alert", 3);
    add("tell", "Never miss a tell", "incoming_tell", 0, "cyan", "{L}", "An on-screen notification for incoming tells.", "", "chime", 0);
    add("group-chat", "Group chat", "group_message", 0, "cyan", "{L}", "Bring group messages into view.", "", "chime", 0);
    add("raid-chat", "Raid chat", "raid_message", 0, "orange", "{L}", "Keep raid instructions visible.", "", "alert", 0);
    add("guild-chat", "Guild chat", "guild_message", 0, "green", "{L}", "Keep up with your guild.", "", "chime", 0);
    add("fellowship-chat", "Fellowship chat", "fellowship_message", 0, "purple", "{L}", "Bring fellowship messages into view.", "", "chime", 0);
    add("resisted", "Spell resisted", "spell_resisted", 0, "orange", "SPELL RESISTED", "Know when your own spell is resisted.", "", "alert", 0);
    add("fizzle", "Fizzle or missed note", "missed_note_or_fizzle", 0, "red", "MISSED NOTE / FIZZLE", "Notice failed spell and bard-song attempts.", "", "critical", 0);
    add("text-match", "Custom text pattern", "chat_text_match", 0, "yellow", "{L}", "Replace keyword with the text you want to notice.", "*keyword*", "alert", 5);
    add("invis", "Invisibility fading", "invisibility_fading", 0, "purple", "INVIS FADING", "React to the game's invisibility-fading message.", "", "critical", 10);
    add("pet-lost", "Pet lost or charm broke", "pet_lost", 0, "red", "PET LOST", "Warn only after your present pet disappears.", "", "critical", 3);
    return out;
}

Engine::Engine() : config(defaults()) {}
void Engine::reset() {
    state_.clear(); active_ = false; zone_ = 0; character_.clear();
    last_tell_at_.reset(); last_tell_.clear();
}
bool Engine::prepare(const Snapshot& s) {
    if (!s.in_zone || !s.alive || !config.value("enabled", true)) { reset(); return false; }
    if (!active_ || zone_ != s.zone_id || character_ != s.character) {
        reset(); zone_ = s.zone_id; character_ = s.character; active_ = true;
    }
    return true;
}
Engine::RuleState& Engine::state(const Json& a) {
    const std::string id = a.value("id", "");
    auto& r = state_[id];
    const std::string fingerprint = a.dump();
    if (r.fingerprint != fingerprint) { r = RuleState{}; r.fingerprint = fingerprint; }
    return r;
}
Fired Engine::fire(const Json& a, const Snapshot& s, uint64_t now, std::string detail,
                   const std::map<std::string, std::string>& captures, bool test) {
    auto& r = state(a);
    const uint64_t counter = test ? r.counter + 1 : ++r.counter;
    const std::string message = a.value("message", "");
    const std::string speech = a.value("speech_text", "");
    Fired f;
    f.id = a.value("id", ""); f.name = a.value("name", "Alert"); f.alert = a; f.at = now; f.test = test;
    f.message = expand(message.empty() ? detail : message, s, detail, counter, captures);
    f.speech = expand(speech.empty() ? (message.empty() ? detail : message) : speech, s, detail, counter, captures);
    if (!test) r.last = now;
    return f;
}
Fired Engine::preview(const Json& input, const Snapshot& s, uint64_t now) {
    const Json a = normalize_alert(input);
    const std::string detail = "Preview: " + describe(a);
    Fired f;
    f.id = a["id"]; f.name = a["name"]; f.alert = a; f.at = now; f.test = true;
    const auto found = state_.find(f.id);
    const uint64_t counter = found == state_.end() ? 1 : found->second.counter + 1;
    const std::string message = a["message"], speech = a["speech_text"];
    f.message = expand(message.empty() ? detail : message, s, detail, counter, {});
    f.speech = expand(speech.empty() ? (message.empty() ? detail : message) : speech, s, detail, counter, {});
    return f;
}

std::vector<Fired> Engine::evaluate(const Snapshot& s, uint64_t now) {
    std::vector<Fired> out;
    if (!prepare(s)) return out;
    const auto ready = [&](const RuleState& r, const Json& a) {
        return !r.last || now < *r.last || static_cast<double>(now - *r.last) >= a.value("cooldown", 10.0) * 1000.0;
    };
    std::set<std::string> retained;
    for (const auto& input : config["alerts"]) {
        const Json a = normalize_alert(input);
        const std::string id = a["id"], e = a["event"], subject = a["subject"];
        retained.insert(id);
        if (!a["enabled"].get<bool>() || (a["zone_id"].get<uint32_t>() && a["zone_id"] != s.zone_id)) { state_.erase(id); continue; }
        auto& r = state(a);
        // An unavailable/partial read is not evidence of an effect arriving or
        // disappearing. Preserve both arrival baselines and existing buff latches.
        if (!s.buffs_complete && (buff_arrival(e) || e == "buff_fading" || e == "buff_missing")) continue;
        const double threshold = a["threshold"];
        bool edge = false;
        std::string detail;
        std::set<uint32_t> observed;
        auto condition = [&](uint32_t key, bool active, std::string text) {
            observed.insert(key);
            if (active && !r.latched[key] && !edge) { edge = true; detail = std::move(text); }
            r.latched[key] = active;
        };
        auto resource = [&](uint32_t key, const std::optional<double>& sample, const std::string& who, const std::string& resource_name) {
            observed.insert(key);
            if (!known(sample)) return;
            const double clear = std::min(100.0, threshold + 3.0);
            condition(key, *sample <= (r.latched[key] ? clear : threshold), who + " " + resource_name + " is " + number(*sample) + "%.");
        };
        if (e == "health_below") resource(0, s.hp, "Your", "health");
        else if (e == "mana_below") resource(0, s.mana, "Your", "mana");
        else if (e == "endurance_below") resource(0, s.endurance, "Your", "endurance");
        else if (e == "pet_lost") {
            if (r.pet_initialized && r.pet_id && !s.pet_id) { edge = true; detail = "Your pet disappeared or charm broke."; }
            r.pet_initialized = true; r.pet_id = s.pet_id;
        } else if (buff_arrival(e)) {
            std::map<uint32_t, const Buff*> current;
            std::set<uint32_t> conflicts;
            for (const auto& b : s.buffs) {
                if (!b.id) continue;
                auto [it, inserted] = current.emplace(b.id, &b);
                if (!inserted) {
                    if (it->second->beneficial && b.beneficial && *it->second->beneficial != *b.beneficial)
                        conflicts.insert(b.id);
                    else if (!it->second->beneficial && b.beneficial) it->second = &b;
                }
            }
            std::set<uint32_t> ids;
            std::vector<std::string> names;
            for (const auto& [spell_id, b] : current) {
                // Track unknown classifications too: learning metadata about an
                // existing spell must never look like receiving a new effect.
                ids.insert(spell_id);
                if (!r.buffs_initialized || r.buff_ids.count(spell_id) || conflicts.count(spell_id) ||
                    !b->beneficial || *b->beneficial != (e == "buff_received") ||
                    (!subject.empty() && !equal(subject, b->name))) continue;
                const std::string name = b->name.empty() ? "spell #" + std::to_string(spell_id) : b->name;
                if (std::none_of(names.begin(), names.end(), [&](const auto& old) { return equal(old, name); }))
                    names.push_back(name);
            }
            r.buffs_initialized = true;
            r.buff_ids = std::move(ids);
            if (!names.empty()) {
                edge = true;
                const std::string kind = e == "buff_received" ? "buff" : "debuff";
                detail = "You received " + (names.size() == 1 ? "a " + kind : kind + "s") + ": ";
                for (size_t i = 0; i < names.size(); ++i) detail += (i ? ", " : "") + names[i];
                detail += ".";
            }
        } else if (e == "buff_fading" || e == "buff_missing") {
            const Buff* buff = nullptr;
            for (const auto& b : s.buffs) if (equal(b.name, subject)) { buff = &b; break; }
            if (!subject.empty()) {
                if (e == "buff_missing") condition(0, !buff, subject + " is missing.");
                else if (!buff) condition(0, false, "");
                else if (buff->seconds && std::isfinite(*buff->seconds) && *buff->seconds >= 0)
                    condition(0, *buff->seconds <= threshold, subject + " has " + number(*buff->seconds) + " seconds remaining.");
                else observed.insert(0);
            }
        } else if (e == "target_cast") {
            for (const auto& entity : s.entities) if (entity.id == s.target_id && !entity.corpse) {
                observed.insert(entity.id);
                const uint32_t previous = r.casts[entity.id];
                if (entity.cast_id && entity.cast_id != previous && (subject.empty() || equal(subject, entity.cast_name))) {
                    edge = true; detail = entity.name + " begins casting " + (entity.cast_name.empty() ? "a spell" : entity.cast_name) + ".";
                }
                r.casts[entity.id] = entity.cast_id;
            }
        } else {
            for (const auto& entity : s.entities) {
                if (!entity.id || entity.corpse) continue;
                if (e == "target_health_below" && entity.id == s.target_id) resource(entity.id, entity.hp, entity.name, "health");
                else if (e == "pet_health_below" && entity.id == s.pet_id) resource(entity.id, entity.hp, entity.name, "health");
                else if (e == "player_health_below" && !entity.npc && equal(entity.name, subject)) resource(entity.id, entity.hp, entity.name, "health");
                else if (e == "player_mana_below" && !entity.npc && equal(entity.name, subject)) resource(entity.id, entity.mana, entity.name, "mana");
                else if (e == "group_health_below" && entity.group && (subject.empty() || equal(entity.name, subject))) resource(entity.id, entity.hp, entity.name, "health");
                else if ((e == "spawn_appears" && entity.npc) || (e == "player_appears" && !entity.npc)) {
                    if (equal(entity.name, subject)) condition(entity.id, std::isfinite(entity.distance) && entity.distance >= 0 && entity.distance <= threshold,
                        entity.name + " is " + number(entity.distance) + " units away.");
                }
            }
        }
        for (auto it = r.latched.begin(); it != r.latched.end();) {
            if (!observed.count(it->first)) it = r.latched.erase(it); else ++it;
        }
        for (auto it = r.casts.begin(); it != r.casts.end();) {
            if (!observed.count(it->first)) it = r.casts.erase(it); else ++it;
        }
        if (edge && ready(r, a)) out.push_back(fire(a, s, now, detail));
    }
    for (auto it = state_.begin(); it != state_.end();) {
        if (!retained.count(it->first)) it = state_.erase(it); else ++it;
    }
    return out;
}

std::vector<Fired> Engine::chat(const Snapshot& s, std::string text, uint32_t color, uint64_t now) {
    std::vector<Fired> out;
    if (!prepare(s) || text.empty() || text.size() > 2048) return out;
    text = clean(text, 2048);
    const std::string lc = lower(text);
    std::string event;
    switch (color) {
    case 257: event = "incoming_tell"; break;
    case 258: event = "group_message"; break;
    case 259: event = "guild_message"; break;
    case 327: event = "raid_message"; break;
    case 342: event = "fellowship_message"; break;
    default: break;
    }
    if (event.empty()) event = social_event(text);
    if (starts(text, "Your spell fizzles") || starts(text, "You miss a note, bringing your ") || starts(text, "You missed a note, bringing your ")) event = "missed_note_or_fizzle";
    if (starts(text, "Your invisibility is starting to fade") || starts(text, "Your invisibility is fading") ||
        starts(text, "You feel yourself starting to appear") || starts(text, "You are starting to appear")) event = "invisibility_fading";
    if (((color == 289 || event.empty()) && starts(text, "Your target resisted the ") && lc.size() >= 7 && lc.compare(lc.size() - 7, 7, " spell.") == 0) ||
        (event.empty() && equal(text, "Your target has resisted your attempt to mesmerize it."))) event = "spell_resisted";
    // The host can deliver a tell through two chat paths. Suppress only that
    // short duplicate, preserving repeated intentional messages elsewhere.
    if (event == "incoming_tell") {
        if (last_tell_at_ && text == last_tell_ && now >= *last_tell_at_ && now - *last_tell_at_ < 250) return out;
        last_tell_at_ = now; last_tell_ = text;
    }
    for (const auto& input : config["alerts"]) {
        const Json a = normalize_alert(input);
        if (!a["enabled"].get<bool>() || (a["zone_id"].get<uint32_t>() && a["zone_id"] != s.zone_id)) continue;
        const std::string e = a["event"], subject = a["subject"];
        if (e != "chat_text_match" && e != event) continue;
        std::map<std::string, std::string> captures;
        if (e == "chat_text_match" || subject.find_first_of("*?{") != std::string::npos) {
            if (!match(text, subject, s.character, captures)) continue;
        } else if (!subject.empty() && lc.find(lower(subject)) == std::string::npos) continue;
        auto& r = state(a);
        if (r.last && now >= *r.last && static_cast<double>(now - *r.last) < a["cooldown"].get<double>() * 1000) continue;
        out.push_back(fire(a, s, now, text, captures));
    }
    return out;
}
} // namespace sentinel
