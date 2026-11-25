// SPDX-License-Identifier: MIT
#include "hailstone_plugin.h"
#include "audio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

HAILSTONE_PLUGIN
namespace {
using namespace sentinel;
namespace fs = std::filesystem;
constexpr size_t max_alerts=128, max_bytes=128*1024, max_config_bytes=1024*1024;
constexpr size_t max_timers = 24;
constexpr auto unassigned_timer = "unassigned-timer";
Engine engine;
Snapshot snapshot;
std::unique_ptr<Audio> audio;
fs::path folder;
fs::path window_preferences;
bool open=false, delivering=false, initialized=false, load_failed=false;
uint64_t revision=0, last_poll=0, next_id=0;
uint64_t timer_profile = 0, next_timer_id = 0;
struct TimerRun {
    uint64_t deadline;
    int duration;
    std::optional<uint64_t> previous_ms;
    bool preview;
};
std::map<std::string, TimerRun> timer_runs;
std::string status="Choose a starter preset or create your first alert.", error;
std::string window_error;
Json history=Json::array(), overlays=Json::array();
std::vector<std::string> sounds;
std::map<uint32_t,bool> buff_types;
#ifdef SENTINEL_TESTING
uint64_t test_now=1000;
#endif
uint64_t now_ms() {
#ifdef SENTINEL_TESTING
    return test_now;
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
void say(const std::string& text) {
    if(!host||!host->write_chat||delivering)return;
    delivering=true;
    try {host->write_chat(host->host,("[Sentinel] "+text).c_str(),0x65ddd1);}catch(...){delivering=false;throw;}
    delivering=false;
}
Json failure(const std::string& message) {error=message;return {{"ok",false},{"error",message}};}
Json success() {error.clear();return {{"ok",true}};}
void log_error(const char* where) noexcept {
    try {throw;}catch(const std::exception& ex){error=std::string(where)+": "+ex.what();}
    catch(...){error=std::string(where)+": unexpected error";}
    try {if(host&&host->log)host->log(host->host,3,error.c_str());}catch(...){}
}
std::optional<double> percent(double cur,double maximum) {
    if(maximum<=0||cur<0||!std::isfinite(cur)||!std::isfinite(maximum))return {};
    return std::clamp(cur/maximum*100.0,0.0,100.0);
}
std::string parse(const std::string& expression) {
    if(!host||!host->parse)return {};
    char text[1024]{};const auto n=host->parse(host->host,expression.c_str(),text,sizeof text);
    if(n<0||static_cast<size_t>(n)>=sizeof text)return {};
    return text;
}
std::optional<double> number(const std::string& s) {
    if(s.empty()||s=="NULL")return {};
    try{size_t n=0;const double v=std::stod(s,&n);if(n==s.size()&&std::isfinite(v))return v;}catch(...){}
    return {};
}
void presentation_defaults(Json& c) {
    for(const auto* key:{"overlay_x","overlay_y"}) {
        const double fallback=std::string(key)=="overlay_x"?.5:.22;
        c[key]=c.contains(key)&&c[key].is_number()?std::clamp(c[key].get<double>(),.02,.98):fallback;
    }
    for(const auto* key:{"overlay_locked","quiet"})
        if(!c.contains(key)||!c[key].is_boolean())c[key]=std::string(key)=="overlay_locked";
    c["timers"] = Json::array();
    c["timers_locked"] = false;
    c["timers_grouped"] = false;
    c["timer_group_layout"] = {{"x", .63}, {"y", .17}, {"width", 320}, {"height", 220}, {"minimized", false},
        {"alpha", 100}, {"background", true}, {"title_bar", true}};
}
int timer_integer(const Json& field, int low, int high, const char* message) {
    if (!field.is_number()) throw std::runtime_error(message);
    const double number = field.get<double>();
    if (!std::isfinite(number) || number < low || number > high || std::trunc(number) != number)
        throw std::runtime_error(message);
    return static_cast<int>(number);
}
int timer_alpha(const Json& field) {
    return timer_integer(field, 10, 100, "Opacity must be a whole percent from 10 to 100.");
}
std::string timer_color(const Json& timer, const char* key) {
    static constexpr std::array<std::string_view, 8> colors = {
        "red", "orange", "yellow", "green", "cyan", "blue", "purple", "white"
    };
    const auto color = timer.at(key).get<std::string>();
    if (std::find(colors.begin(), colors.end(), color) == colors.end())
        throw std::runtime_error("Choose a timer color from the palette.");
    return color;
}
Json timer_definition(const Json& raw) {
    auto name = raw.value("name", std::string{});
    const auto start = name.find_first_not_of(' ');
    if (start == std::string::npos) throw std::runtime_error("Enter a timer name.");
    name = name.substr(start, name.find_last_not_of(' ') - start + 1);
    if (name.size() > 80 || std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw std::runtime_error("Use a timer name up to 80 bytes without control characters.");
    const int duration = timer_integer(raw.at("duration"), 1, max_timer_seconds,
        "Duration must be a whole number from 1 to 86400 seconds.");
    const int dismiss_after = timer_integer(raw.value("dismiss_after", Json(0)), 0, max_timer_seconds,
        "Hide delay must be a whole number from 0 to 86400 seconds.");
    const char* threshold_help = "Thresholds must be whole seconds from zero to the timer duration.";
    const int warning = timer_integer(raw.at("warning_at"), 0, duration, threshold_help);
    const int critical = timer_integer(raw.at("critical_at"), 0, duration, threshold_help);
    if (warning && critical >= warning)
        throw std::runtime_error("The critical threshold must be lower than the warning threshold.");
    return {{"name", name}, {"duration", duration}, {"dismiss_after", dismiss_after}, {"color", timer_color(raw, "color")},
        {"warning_at", warning}, {"warning_color", timer_color(raw, "warning_color")},
        {"critical_at", critical}, {"critical_color", timer_color(raw, "critical_color")},
        {"alpha", timer_alpha(raw.value("alpha", Json(100)))}};
}
Json timer_geometry(const Json& x, const Json& y, const Json& width, const Json& height, const Json& minimized, bool grouped = false) {
    auto coordinate = [](const Json& field, double low, double high) {
        if (!field.is_number() || !std::isfinite(field.get<double>()))
            throw std::runtime_error("Timer position and size must be finite numbers.");
        return std::clamp(field.get<double>(), low, high);
    };
    if (!minimized.is_boolean()) throw std::runtime_error("Expected a timer minimize state.");
    return {{"x", coordinate(x, 0, 1)}, {"y", coordinate(y, 0, 1)},
        {"width", coordinate(width, 160, 1000)}, {"height", coordinate(height, grouped ? 80 : 54, grouped ? 1000 : 300)},
        {"minimized", minimized}};
}
void load_timer_definitions(Json& config, const Json& raw) {
    if (raw.contains("timers_locked")) config["timers_locked"] = raw.at("timers_locked").get<bool>();
    if (raw.contains("timers_grouped")) config["timers_grouped"] = raw.at("timers_grouped").get<bool>();
    if (raw.contains("timer_group_layout")) {
        const auto& pane = raw.at("timer_group_layout");
        config["timer_group_layout"] = timer_geometry(pane.at("x"), pane.at("y"), pane.at("width"), pane.at("height"), pane.at("minimized"), true);
        config["timer_group_layout"]["alpha"] = timer_alpha(pane.value("alpha", Json(100)));
        config["timer_group_layout"]["background"] = pane.value("background", true);
        config["timer_group_layout"]["title_bar"] = pane.value("title_bar", true);
    }
    if (!raw.contains("timers")) return;
    const auto& saved = raw.at("timers");
    if (!saved.is_array() || saved.size() > max_timers) throw std::runtime_error("Invalid timer list.");
    auto& timers = config["timers"];
    for (const auto& item : saved) {
        const auto id = item.at("id").get<std::string>();
        if (!id.starts_with("timer-") || id.size() <= 6 || id.size() > 26 ||
            id.find_first_not_of("0123456789", 6) != std::string::npos ||
            std::any_of(timers.begin(), timers.end(), [&](const Json& timer) { return timer.at("id") == id; }))
            throw std::runtime_error("Invalid or duplicate timer ID.");
        auto timer = timer_definition(item);
        timer.update(timer_geometry(item.at("x"), item.at("y"), item.at("width"), item.at("height"), item.at("minimized")));
        timer["id"] = id;
        timers.push_back(std::move(timer));
    }
}
Json config_normalized(const Json& raw) {
    if(raw.contains("version")&&raw["version"]!=1)throw std::runtime_error("Unsupported settings version");
    auto c=normalize_config(raw);
    for(const auto* key:{"overlay_x","overlay_y","overlay_locked","quiet"})if(raw.contains(key))c[key]=raw[key];
    presentation_defaults(c);
    load_timer_definitions(c, raw);
    return c;
}
void write_atomic(const fs::path& path,const std::string& text) {
    fs::create_directories(path.parent_path());
    // Boxed clients share addon preferences; each write needs its own temp file.
    static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
    const auto process=static_cast<uint64_t>(GetCurrentProcessId());
#else
    const auto process=static_cast<uint64_t>(getpid());
#endif
    auto temp=path;temp+=".tmp-"+std::to_string(process)+"-"+std::to_string(++serial);
    try {
        {
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);
            if(!out)throw std::runtime_error("Cannot create settings file");
            out.write(text.data(),static_cast<std::streamsize>(text.size()));out.flush();
            if(!out)throw std::runtime_error("Cannot write settings file");
        }
#ifdef _WIN32
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace settings file");
#else
        fs::rename(temp,path);
#endif
    }catch(...) {
        std::error_code ec;fs::remove(temp,ec);throw;
    }
}
bool save_window_preferences() {
    try {
        if(window_preferences.empty())throw std::runtime_error("Addon settings folder is unavailable");
        write_atomic(window_preferences,Json{{"version",1},{"window_open",open}}.dump(2));
        window_error.clear();return true;
    }catch(const std::exception& ex) {
        window_error="Cannot remember window visibility: "+std::string(ex.what());return false;
    }
}
void load_window_preferences() {
    open=false;window_preferences.clear();window_error.clear();
    try {
        char path[4096]{};
        const int n=host->config_dir?host->config_dir(host->host,path,sizeof path):0;
        if(n<=0||static_cast<size_t>(n)>=sizeof path)throw std::runtime_error("Addon settings folder is unavailable");
        window_preferences=fs::path(std::u8string(path,path+n))/"ui-settings.json";
        if(!fs::exists(window_preferences)) {
            // One first-use popup per installation, independent from character alerts.
            open=true;
            if(!save_window_preferences())open=false;
            return;
        }
        if(fs::file_size(window_preferences)>4096)throw std::runtime_error("Window settings exceed 4 KiB");
        std::ifstream in(window_preferences,std::ios::binary);
        if(!in)throw std::runtime_error("Cannot open window settings");
        const std::string text((std::istreambuf_iterator<char>(in)),{});
        const auto prefs=Json::parse(text,[](int depth,Json::parse_event_t,Json&){
            if(depth>4)throw std::runtime_error("Window settings are nested too deeply");
            return true;
        });
        if(!prefs.is_object()||prefs.value("version",0)!=1||!prefs.contains("window_open")||!prefs["window_open"].is_boolean())
            throw std::runtime_error("Unrecognized window settings");
        open=prefs["window_open"].get<bool>();
    }catch(const std::exception& ex) {
        // Unreadable preferences must not create a popup loop. An explicit show/hide
        // repairs only this small UI preference; character alert files stay separate.
        window_error="Cannot restore window visibility: "+std::string(ex.what());
    }
}
Json set_window_open(bool value) {
    // Honor the click immediately even if the preference cannot be written.
    open=value;
    if(!save_window_preferences())return {{"ok",false},{"error",window_error}};
    return success();
}
bool persist() {
    if(folder.empty()){error="Enter the world before saving character settings.";return false;}
    if(load_failed){error="Settings could not be read. Restore or rename config.json before saving; the original has been preserved.";return false;}
    try {
        const auto text=engine.config.dump(2);
        if(text.size()>max_config_bytes)throw std::runtime_error("Settings exceed 1 MiB");
        write_atomic(folder/"config.json",text);
        ++revision;error.clear();return true;
    } catch(const std::exception& ex){error=ex.what();return false;}
}
template<typename Fn> Json change(Fn fn) {
    const auto previous=engine.config;
    try{fn();if(!persist()){engine.config=previous;return failure(error);}return success();}
    catch(const std::exception& ex){engine.config=previous;return failure(ex.what());}
}
void cancel_activity() {engine.reset();buff_types.clear();overlays.clear();if(audio)audio->cancel();}
void select_timer_profile(const fs::path& next) {
    if (next == folder) return;
    timer_runs.clear();
    ++timer_profile;
}
void load_for(const fs::path& next) {
    select_timer_profile(next);
    cancel_activity();history.clear();folder=next;load_failed=false;error.clear();
    engine.config=defaults();presentation_defaults(engine.config);
    const auto path=folder/"config.json";
    try {
        if(fs::exists(path)) {
            if(fs::file_size(path)>max_config_bytes)throw std::runtime_error("Settings exceed 1 MiB");
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot open settings");
            const std::string text((std::istreambuf_iterator<char>(in)),{});
            auto raw=Json::parse(text,[](int depth,Json::parse_event_t,Json&){
                if(depth>20)throw std::runtime_error("Settings are nested too deeply");return true;});
            engine.config=config_normalized(raw);
        }
        fs::create_directories(folder/"Sounds");
        sounds=Audio::files(folder/"Sounds");
        status="Watching "+snapshot.character+". Settings save automatically.";
    }catch(const std::exception& ex){load_failed=true;error="Cannot read settings: "+std::string(ex.what());}
    const auto& timers = engine.config.at("timers");
    std::erase_if(timer_runs, [&](const auto& run) {
        return std::none_of(timers.begin(), timers.end(), [&](const Json& timer) { return timer.at("id") == run.first; });
    });
    ++revision;
}
void refresh_snapshot() {
    Snapshot next;
    HailstonePlayer player{};player.size=sizeof player;
    if(!host||!host->get_player||!host->get_player(host->host,&player)||!player.in_zone) {
        if(snapshot.in_zone)cancel_activity();snapshot=next;return;
    }
    next.in_zone=true;next.alive=player.cur_hp>0&&player.stand_state!=120;
    next.zone_id=player.zone_id;next.character=player.name;next.target_id=player.target_id;next.pet_id=player.pet_id;
    if(next.zone_id!=snapshot.zone_id||next.character!=snapshot.character)buff_types.clear();
    next.hp=percent(static_cast<double>(player.cur_hp),static_cast<double>(player.max_hp));
    next.mana=percent(player.cur_mana,player.max_mana);next.endurance=percent(player.cur_endurance,player.max_endurance);
    const auto& alerts=engine.config["alerts"];
    const bool need_entities=std::any_of(alerts.begin(),alerts.end(),[](const auto& a){
        const auto e=a.value("event",std::string{});
        return a.value("enabled",false)&&(e=="player_health_below"||e=="player_mana_below"||e=="pet_health_below"||
            e=="target_health_below"||e=="group_health_below"||e=="spawn_appears"||e=="player_appears"||e=="target_cast");});
    HailstoneSpawn self{};self.size=sizeof self;
    const bool have_self=need_entities&&host->get_spawn&&host->get_spawn(host->host,player.spawn_id,&self);
    const bool need_mana=std::any_of(alerts.begin(),alerts.end(),[](const auto& a){return a.value("enabled",false)&&a.value("event",std::string{})=="player_mana_below";});
    const bool need_cast=std::any_of(alerts.begin(),alerts.end(),[](const auto& a){return a.value("enabled",false)&&a.value("event",std::string{})=="target_cast";});
    if(need_entities&&host->next_spawn) {
        uint32_t after=0;
        for(size_t count=0;count<16384;++count) {
            HailstoneSpawn spawn{};spawn.size=sizeof spawn;
            if(!host->next_spawn(host->host,after,&spawn)||spawn.id<=after)break;
            after=spawn.id;if(spawn.id==player.spawn_id)continue;
            Entity e;e.id=spawn.id;e.name=spawn.name;e.npc=spawn.is_npc!=0;e.corpse=spawn.is_corpse!=0;e.group=spawn.in_group!=0;
            if(have_self) {
                const double dx=static_cast<double>(spawn.x)-self.x,dy=static_cast<double>(spawn.y)-self.y,dz=static_cast<double>(spawn.z)-self.z;
                e.distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            } else e.distance=std::numeric_limits<double>::infinity();
            if(spawn.hp_percent<=100)e.hp=spawn.hp_percent;
            const auto base="${Spawn[id "+std::to_string(spawn.id)+"].";
            if(need_mana&&!e.npc)e.mana=number(parse(base+"PctMana}"));
            if(need_cast&&spawn.id==player.target_id) {
                const auto id=number(parse(base+"Casting.ID}"));
                if(id&&*id>0&&*id<4294967295.0)e.cast_id=static_cast<uint32_t>(*id);
                if(e.cast_id)e.cast_name=parse(base+"Casting.Name}");
            }
            next.entities.push_back(std::move(e));
        }
    }
    // MQ indexes Buff and Song as separate compact occupied lists, not raw slots.
    // Enumerate numeric indexes and spell IDs to avoid interpolating spell names.
    std::map<uint32_t,std::optional<double>> durations;
    const bool need_durations=std::any_of(alerts.begin(),alerts.end(),[](const auto& a){
        return a.value("enabled",false)&&a.value("event",std::string{})=="buff_fading";});
    if(need_durations)for(const auto* kind:{"Buff","Song"}) {
        const auto count=number(parse(std::string("${Me.Count")+(std::string(kind)=="Buff"?"Buffs}":"Songs}")));
        const auto n=count?static_cast<unsigned>(std::clamp(*count,0.0,512.0)):0;
        for(unsigned i=1;i<=n;++i) {
            const auto base=std::string("${Me.")+kind+"["+std::to_string(i)+"].";
            const auto spell=number(parse(base+"Spell.ID}"));
            if(spell&&*spell>0&&*spell<4294967295.0)
                durations[static_cast<uint32_t>(*spell)]=number(parse(base+"Duration.TotalSeconds}"));
        }
    }
    const bool need_buff_types=std::any_of(alerts.begin(),alerts.end(),[](const auto& a){
        const auto event=a.value("event",std::string{});
        return a.value("enabled",false)&&(event=="buff_received"||event=="debuff_received");});
    // get_buff succeeds for empty slots too. A failed read must not make an
    // existing effect appear removed, then newly received on the next poll.
    next.buffs_complete=host->get_buff&&player.buff_slots>0&&player.buff_slots<=512;
    if(host->get_buff)for(uint32_t i=0;i<std::min(player.buff_slots,512u);++i) {
        HailstoneBuff b{};b.size=sizeof b;
        if(!host->get_buff(host->host,i,&b)){next.buffs_complete=false;continue;}
        if(!b.spell_id)continue;
        Buff buff;buff.id=b.spell_id;
        HailstoneSpell spell{};spell.size=sizeof spell;
        const bool known=host->get_spell&&host->get_spell(host->host,b.spell_id,&spell)&&spell.id==b.spell_id;
        if(known)buff.name=spell.name;
        if(need_buff_types&&known) {
            if(const auto it=buff_types.find(b.spell_id);it!=buff_types.end())buff.beneficial=it->second;
            else {
                // Numeric lookup is stable across client SDK revisions. The bulk
                // buff record layout changed within ABI 3; don't infer its layout.
                const auto type=parse("${Spell["+std::to_string(b.spell_id)+"].SpellType}");
                if(type=="Beneficial")buff.beneficial=true;
                else if(type=="Detrimental")buff.beneficial=false;
                if(buff.beneficial.has_value())buff_types.emplace(b.spell_id,*buff.beneficial);
            }
        }
        // The TLO subtracts elapsed time; raw buff tics are only the last server snapshot.
        if(const auto it=durations.find(b.spell_id);it!=durations.end())buff.seconds=it->second;
        next.buffs.push_back(std::move(buff));
    }
    const bool changed_character=snapshot.character!=next.character;
    snapshot=std::move(next);
    if(HAILSTONE_HOST_HAS(host,character_config_dir)) {
        char path[4096]{};const int n=host->character_config_dir(host->host,path,sizeof path);
        if(n>0&&static_cast<size_t>(n)<sizeof path) {
            const auto next_folder=fs::path(std::u8string(path,path+n));
            if(changed_character||folder!=next_folder)load_for(next_folder);
        } else if(changed_character) {
            select_timer_profile({});
            folder.clear();engine.config=defaults();presentation_defaults(engine.config);cancel_activity();
            history.clear();sounds.clear();status="Waiting for the character settings folder.";
        }
    }
}
Json* find_timer(const std::string& id) {
    auto& timers = engine.config["timers"];
    const auto it = std::find_if(timers.begin(), timers.end(), [&](const Json& timer) { return timer.at("id") == id; });
    return it == timers.end() ? nullptr : &*it;
}
void start_timer(const Json& timer, uint64_t now, bool preview = false) {
    const auto& id = timer.at("id").get_ref<const std::string&>();
    if (preview) {
        const auto running = timer_runs.find(id);
        if (running != timer_runs.end() && !running->second.preview && running->second.previous_ms != 0) return;
    }
    const int duration = timer.at("duration").get<int>();
    timer_runs[id] = {now + static_cast<uint64_t>(duration) * 1000, duration, std::nullopt, preview};
}
void sample_timers(uint64_t now) {
    if (!snapshot.in_zone) return;
    for (auto& [id, run] : timer_runs) {
        if (run.preview || run.previous_ms == 0) continue;
        const auto* timer = find_timer(id);
        if (!timer) continue;
        const uint64_t remaining = run.deadline > now ? run.deadline - now : 0;
        snapshot.timers.emplace(id, TimerReading{timer->at("name").get<std::string>(), remaining, run.previous_ms});
        run.previous_ms = remaining;
    }
}
void dismiss_finished_timers(uint64_t now) {
    if (!snapshot.in_zone) return;
    std::erase_if(timer_runs, [now](const auto& entry) {
        const auto& [id, run] = entry;
        if (run.deadline > now) return false;
        const Json* timer = find_timer(id);
        if (!timer) return false;
        const int delay = timer->at("dismiss_after").get<int>();
        return delay > 0 && now - run.deadline >= static_cast<uint64_t>(delay) * 1000;
    });
}
std::string clock_text() {
    const auto t=std::time(nullptr);std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    std::ostringstream text;text<<std::put_time(&tm,"%H:%M:%S");return text.str();
}
void deliver(const Fired& f) {
    if (snapshot.in_zone) {
        if (const auto timer_id = f.alert.value("timer_id", std::string{}); !timer_id.empty()) {
            if (const auto* timer = find_timer(timer_id)) start_timer(*timer, now_ms(), f.test);
        }
    }
    if(f.alert.value("chat",true))say((f.test?"Preview: ":"")+f.message);
    history.push_back({{"id",f.id},{"name",f.name},{"message",f.message},{"at",f.at},
        {"time",clock_text()},{"test",f.test},{"color",f.alert.value("color",std::string("cyan"))}});
    while(history.size()>100)history.erase(history.begin());
    if(f.alert.value("overlay",true)) {
        const double duration=f.alert.value("duration",3.0);
        overlays.push_back({{"message",f.message},{"name",f.name},{"color",f.alert.value("color",std::string("cyan"))},
            {"size",f.alert.value("size",1)},{"duration",duration},{"expires",now_ms()+static_cast<uint64_t>(duration*1000)}});
        while(overlays.size()>4)overlays.erase(overlays.begin());
    }
    if(audio&&!engine.config.value("quiet",false))audio->enqueue(f,folder/"Sounds");
}
std::string new_id() {
    for(;;){const auto id="alert-"+std::to_string(++next_id);
        const auto& list=engine.config["alerts"];
        if(std::none_of(list.begin(),list.end(),[&](const auto& a){return a.value("id",std::string{})==id;}))return id;}
}
Json* find(const std::string& id) {
    for(auto& a:engine.config["alerts"])if(a.value("id",std::string{})==id)return &a;
    return nullptr;
}
std::string timer_condition(const Json& rule) {
    return rule.value("event", "") == "timer_remaining" ? rule.value("subject", std::string{}) : std::string{};
}
bool makes_timer_cycle(const Json& proposed, const std::string& source, const std::string& target) {
    if (source.empty() || target.empty()) return false;
    const auto edited_id = proposed.at("id");
    std::set<std::string> seen{target};
    std::vector<std::string> reachable{target};
    for (size_t i = 0; i < reachable.size(); ++i) {
        if (reachable[i] == source) return true;
        for (const auto& rule : engine.config.at("alerts")) {
            if (rule.at("id") == edited_id || !rule.value("enabled", true) || timer_condition(rule) != reachable[i]) continue;
            const auto next = rule.value("timer_id", std::string{});
            if (!next.empty() && seen.insert(next).second) reachable.push_back(next);
        }
    }
    return false;
}
void validate_rule(const Json& a) {
    const auto event=a.value("event",std::string{}),subject=a.value("subject",std::string{});
    const bool required=event=="player_health_below"||event=="player_mana_below"||event=="buff_fading"||
        event=="buff_missing"||event=="spawn_appears"||event=="player_appears"||event=="chat_text_match";
    if(required&&(subject.empty()||subject.find_first_not_of(" \t\r\n")==std::string::npos))
        throw std::runtime_error("Enter a name or text pattern for this event.");
    const auto timer_id = a.value("timer_id", std::string{});
    if (event == "timer_remaining" && !find_timer(subject))
        throw std::runtime_error("Choose a saved timer bar for the condition.");
    if (!timer_id.empty() && !find_timer(timer_id))
        throw std::runtime_error("Choose a saved timer bar to start.");
    if (event == "timer_remaining" && makes_timer_cycle(a, subject, timer_id))
        throw std::runtime_error("Choose a timer result that does not restart this timer directly or through other rules.");
    if(!a.value("chat",false)&&!a.value("overlay",false)&&!a.value("sound",false)&&!a.value("speech",false)&&timer_id.empty())
        throw std::runtime_error("Choose at least one result: banner, chat, sound, speech, or timer bar.");
}
enum class Ui {State,Action,Save,Preview,Export,Inspect,Import,Settings,TimerSave,TimerAction,TimerLayout};
constexpr std::array ui_names = {"state", "action", "save", "preview", "export", "inspect", "import", "settings",
    "timer_save", "timer_action", "timer_layout"};
std::string bounded_string(const char* s,size_t maximum=max_bytes) {
    if(!s)return {};
    size_t len=0;while(len<=maximum&&s[len])++len;
    if(len>maximum)throw std::runtime_error("Text is too long");return std::string(s,len);
}
Json value(const HailstoneValue& v) {
    if(v.kind==HAILSTONE_STRING)return bounded_string(v.s);
    if(v.kind==HAILSTONE_BOOL)return v.i!=0;
    if(v.kind==HAILSTONE_INT)return v.i;
    if(v.kind==HAILSTONE_FLOAT) {if(!std::isfinite(v.f))throw std::runtime_error("Number must be finite");return v.f;}
    return nullptr;
}
Json arg(const HailstoneArgs* args,size_t n) {
    HailstoneValue v{};if(!host->arg(host->host,args,n,&v))return nullptr;
    if(v.kind!=HAILSTONE_TABLE)return value(v);
    Json object=Json::object();
    for(const auto* key:{"id","name","event","enabled","category","section","subsection","zone_id","subject",
        "threshold","message","cooldown","chat","overlay","sound","speech","volume","tone","sound_file",
        "speech_text","voice","rate","color","size","duration","overlay_x","overlay_y","overlay_locked","quiet",
        "warning_at","warning_color","critical_at","critical_color","timer_id","alpha","dismiss_after"}) {
        HailstoneValue field{};if(host->arg_field(host->host,args,n,key,&field))object[key]=value(field);
    }return object;
}
Json default_timer_layout(const Json& timers) {
    static constexpr std::array columns = {.63, .33, .04};
    for (const double x : columns) {
        for (int row = 0; row < 8; ++row) {
            const double y = .17 + .085 * row;
            const auto nearby = [=](const Json& timer) {
                return std::abs(timer.at("x").get<double>() - x) < .06 &&
                    std::abs(timer.at("y").get<double>() - y) < .04;
            };
            if (std::none_of(timers.begin(), timers.end(), nearby)) return timer_geometry(x, y, 280, 74, false);
        }
    }
    return timer_geometry(columns.front(), .17, 280, 74, false);
}
Json save_timer(const Json& raw) {
    if (!raw.is_object()) return failure("Expected a timer definition.");
    auto timer = timer_definition(raw);
    auto id = raw.value("id", std::string{});
    auto result = change([&] {
        if (!id.empty()) {
            auto* existing = find_timer(id);
            if (!existing) throw std::runtime_error("This timer no longer exists. Reopen the editor.");
            existing->update(timer);
            return;
        }
        auto& timers = engine.config["timers"];
        if (timers.size() >= max_timers) throw std::runtime_error("Sentinel holds up to 24 timer bars.");
        do { id = "timer-" + std::to_string(++next_timer_id); } while (find_timer(id));
        timer["id"] = id;
        timer.update(default_timer_layout(timers));
        timers.push_back(std::move(timer));
    });
    if (result.value("ok", false)) result["id"] = id;
    return result;
}
Json timer_action(const std::string& op, const std::string& id) {
    if (op == "lock" || op == "unlock") {
        const bool locked = op == "lock";
        if (engine.config.at("timers_locked") == locked) return success();
        return change([&] { engine.config["timers_locked"] = locked; });
    }
    if (op == "group" || op == "separate") {
        const bool grouped = op == "group";
        if (engine.config.at("timers_grouped") == grouped) return success();
        return change([&] { engine.config["timers_grouped"] = grouped; });
    }
    const auto* timer = find_timer(id);
    if (!timer) return failure("Timer not found.");
    if (op == "start") {
        start_timer(*timer, now_ms());
        return success();
    }
    if (op == "stop") {
        timer_runs.erase(id);
        return success();
    }
    if (op == "delete") {
        auto result = change([&] {
            auto& timers = engine.config["timers"];
            timers.erase(std::find_if(timers.begin(), timers.end(), [&](const Json& entry) { return entry.at("id") == id; }));
            for (auto& rule : engine.config["alerts"]) {
                if (timer_condition(rule) == id) {
                    rule["subject"] = unassigned_timer;
                    rule["enabled"] = false;
                }
                if (rule.value("timer_id", "") == id) {
                    rule["timer_id"] = unassigned_timer;
                    rule["enabled"] = false;
                }
            }
        });
        if (result.value("ok", false)) timer_runs.erase(id);
        return result;
    }
    return failure("Unknown timer action.");
}
Json save_timer_layout(const HailstoneArgs* args) {
    const auto id = arg(args, 0).get<std::string>();
    const bool grouped = id == "group";
    auto* timer = grouped ? &engine.config.at("timer_group_layout") : find_timer(id);
    if (!timer) return failure("Timer not found.");
    auto layout = timer_geometry(arg(args, 1), arg(args, 2), arg(args, 3), arg(args, 4), arg(args, 5), grouped);
    if (engine.config.at("timers_locked").get<bool>())
        layout = {{"minimized", layout.at("minimized")}};
    auto updated = *timer;
    updated.update(layout);
    if (updated == *timer) return success();
    return change([&] { *timer = std::move(updated); });
}
Json save_group_alpha(const Json& value) {
    auto& alpha = engine.config.at("timer_group_layout").at("alpha");
    const int opacity = timer_alpha(value);
    if (alpha == opacity) return success();
    return change([&] { alpha = opacity; });
}
Json save_group_visibility(const char* key, const Json& value) {
    if (!value.is_boolean()) return failure("Expected a boolean timer pane visibility state.");
    auto& setting = engine.config.at("timer_group_layout").at(key);
    if (setting == value) return success();
    return change([&] { setting = value; });
}
void output(HailstoneOut* out,const Json& j) {
    if(j.is_object()){host->out_begin_table(host->host,out);
        for(const auto& [key,item]:j.items()){host->out_key(host->host,out,key.c_str());output(out,item);}host->out_end(host->host,out);}
    else if(j.is_array()){host->out_begin_array(host->host,out);for(const auto& item:j)output(out,item);host->out_end(host->host,out);}
    else {HailstoneValue v{};
        if(j.is_boolean()){v.kind=HAILSTONE_BOOL;v.i=j.get<bool>()?1:0;}
        else if(j.is_number_integer()){v.kind=HAILSTONE_INT;v.i=j.get<int64_t>();}
        else if(j.is_number()){v.kind=HAILSTONE_FLOAT;v.f=j.get<double>();}
        else if(j.is_string()){v.kind=HAILSTONE_STRING;v.s=j.get_ref<const std::string&>().c_str();}
        host->out_value(host->host,out,&v);}
}
Json state() {
    const auto now=now_ms();
    overlays.erase(std::remove_if(overlays.begin(),overlays.end(),[&](const auto& o){return o["expires"].template get<uint64_t>()<=now;}),overlays.end());
    Json showing=overlays;for(auto& o:showing)o["remaining"]=(o["expires"].get<uint64_t>()-now)/1000.0;
    Json result=engine.config;
    result["timer_profile"] = std::to_string(timer_profile);
    for (auto& timer : result["timers"]) {
        const auto run = timer_runs.find(timer.at("id").get_ref<const std::string&>());
        const bool started = run != timer_runs.end();
        const bool running = started && run->second.deadline > now;
        timer["running"] = running;
        timer["finished"] = started && !running;
        timer["remaining"] = running ? (run->second.deadline - now) / 1000.0 : 0.0;
        timer["run_duration"] = started ? run->second.duration : timer.at("duration").get<int>();
    }
    result["open"]=open;result["in_zone"]=snapshot.in_zone;result["character"]=snapshot.character;
    result["zone_id"]=snapshot.zone_id;
    char zone[128]{};if(host->zone_name)host->zone_name(host->host,snapshot.zone_id,zone,sizeof zone);
    static const auto catalog=events(), starter_presets=presets();
    result["zone_name"]=zone;result["catalog"]=catalog;result["presets"]=starter_presets;result["history"]=history;
    result["overlays"]=showing;result["status"]=status;result["error"]=error.empty()?window_error:error;result["revision"]=revision;
    result["audio_available"]=audio&&audio->sound_available();result["speech_available"]=audio&&audio->speech_available();
    const auto path=(folder/"config.json").u8string();
    result["sounds"]=sounds;result["config_path"]=std::string(path.begin(),path.end());
    result["hp"]=snapshot.hp?Json(*snapshot.hp):Json();result["mana"]=snapshot.mana?Json(*snapshot.mana):Json();
    result["endurance"]=snapshot.endurance?Json(*snapshot.endurance):Json();return result;
}
Json preview_rule(const Json& rule) {
    const auto timer_id = rule.value("timer_id", std::string{});
    if (!timer_id.empty() && (!snapshot.in_zone || !find_timer(timer_id)))
        return failure("Enter the world and choose a saved timer bar to preview this result.");
    deliver(engine.preview(rule, snapshot, now_ms()));
    return success();
}
Json action(const std::string& op,const std::string& id) {
    if(op=="show")return set_window_open(true);
    if(op=="hide")return set_window_open(false);
    if(op=="clear_history"){history.clear();return success();}
    if(op=="rescan_sounds"){sounds=Audio::files(folder/"Sounds");return success();}
    if(op=="test") {const auto* a=find(id);if(!a)return failure("Alert not found");return preview_rule(*a);}
    if(op=="on"||op=="off"||op=="toggle") {
        const auto r=change([&]{engine.config["enabled"]=op=="toggle"?!engine.config.value("enabled",true):op=="on";});
        if(r.value("ok",false)){cancel_activity();status=engine.config.value("enabled",true)?"Sentinel resumed.":"All alerts paused. Your library is saved.";}return r;
    }
    return change([&]{auto& list=engine.config["alerts"];
        if(op=="preset") {
            if(list.size()>=max_alerts)throw std::runtime_error("The library is full (128 alerts).");
            const auto all=presets();auto it=std::find_if(all.begin(),all.end(),[&](const auto& p){return p.value("id",std::string{})==id;});
            if(it==all.end())throw std::runtime_error("Preset not found");
            auto a=normalize_alert(*it);validate_rule(a);a["id"]=new_id();list.push_back(a);status="Added "+a.value("name",std::string("preset"))+".";return;
        }
        auto* a=find(id);if(!a)throw std::runtime_error("Alert not found");
        if(op=="toggle_alert"){if(!a->value("enabled",true))validate_rule(*a);(*a)["enabled"]=!a->value("enabled",true);}
        else if(op=="duplicate") {
            if(list.size()>=max_alerts)throw std::runtime_error("The library is full (128 alerts).");
            auto copy=*a;copy["id"]=new_id();copy["name"]=a->value("name",std::string("Alert"))+" (copy)";copy["enabled"]=false;list.push_back(copy);
        }else if(op=="delete") list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& item){return item.value("id",std::string{})==id;}),list.end());
        else throw std::runtime_error("Unknown action");
    });
}
Json ui_call(Ui kind,const HailstoneArgs* args) {
    if(kind==Ui::State)return state();
    if (kind == Ui::TimerSave || kind == Ui::TimerAction || kind == Ui::TimerLayout) {
        if (!snapshot.in_zone || folder.empty()) return failure("Enter the world to use character timer bars.");
        if (kind == Ui::TimerSave) return save_timer(arg(args, 0));
        if (kind == Ui::TimerLayout) return save_timer_layout(args);
        const auto op = arg(args, 0).get<std::string>();
        const auto target = arg(args, 1);
        if (op == "group_alpha") return save_group_alpha(target);
        if (op == "group_background") return save_group_visibility("background", target);
        if (op == "group_title") return save_group_visibility("title_bar", target);
        return timer_action(op, target.get<std::string>());
    }
    if(kind==Ui::Action) {const auto op=arg(args,0),id=arg(args,1);return action(op.is_string()?op.get<std::string>():"",id.is_string()?id.get<std::string>():"");}
    if(kind==Ui::Save||kind==Ui::Preview) {
        const auto raw=arg(args,0);if(!raw.is_object())return failure("Expected an alert");
        auto a=normalize_alert(raw);
        if(kind==Ui::Preview)return preview_rule(a);
        validate_rule(a);
        const auto requested=raw.value("id",std::string{});
        if(!requested.empty()&&!find(requested))return failure("This alert no longer exists. Reopen the editor.");
        a["id"]=requested.empty()?new_id():requested;
        auto result=change([&]{auto* current=find(a["id"].get<std::string>());
            if(current)*current=a;else{if(engine.config["alerts"].size()>=max_alerts)throw std::runtime_error("The library is full (128 alerts).");engine.config["alerts"].push_back(a);}});
        result["id"]=a["id"];return result;
    }
    if(kind==Ui::Export) {
        const auto id=arg(args,0);Json list=engine.config["alerts"];
        if(id.is_string()&&!id.get<std::string>().empty()){const auto* a=find(id.get<std::string>());if(!a)throw std::runtime_error("Alert not found");list=Json::array({*a});}
        return export_alerts(list);
    }
    if(kind==Ui::Inspect||kind==Ui::Import) {
        const auto text=arg(args,0);if(!text.is_string())return failure("Paste a Sentinel export first");
        auto list=parse_import(text.get<std::string>());
        for (auto& a : list) {
            a["enabled"] = false;
            if (!timer_condition(a).empty()) a["subject"] = unassigned_timer;
            if (a.contains("timer_id")) a["timer_id"] = unassigned_timer;
        }
        if(kind==Ui::Inspect)return {{"ok",true},{"alerts",list}};
        if(list.size()+engine.config["alerts"].size()>max_alerts)return failure("There is not enough room. Sentinel holds 128 alerts.");
        auto result=change([&]{for(auto a:list){a["id"]=new_id();engine.config["alerts"].push_back(a);}});
        result["count"]=list.size();if(result.value("ok",false))status="Imported "+std::to_string(list.size())+" alerts, disabled for review.";return result;
    }
    if(kind==Ui::Settings) {
        const auto raw=arg(args,0);if(!raw.is_object())return failure("Expected settings");
        auto result=change([&]{for(const auto* key:{"enabled","quiet","overlay_locked"})if(raw.contains(key)) {
            if(!raw[key].is_boolean())throw std::runtime_error("Switches must be true or false");engine.config[key]=raw[key];}
            for(const auto* key:{"overlay_x","overlay_y"})if(raw.contains(key)) {
                if(!raw[key].is_number())throw std::runtime_error("Overlay position must be a number");
                engine.config[key]=std::clamp(raw[key].get<double>(),.02,.98);}});
        if(result.value("ok",false)&&(!engine.config.value("enabled",true)||engine.config.value("quiet",false)))if(audio)audio->cancel();
        return result;
    }
    return failure("Unknown operation");
}
int ui_dispatch(void* user,const HailstoneArgs* args,HailstoneOut* out) noexcept {
    try {output(out,ui_call(*static_cast<Ui*>(user),args));return 1;}
    catch(const std::exception& ex){try{output(out,failure(ex.what()));}catch(...){}return 1;}
    catch(...){try{output(out,failure("Unexpected error"));}catch(...){}return 1;}
}
void command(void*,const char* line) noexcept {
    try {
        std::istringstream input(line?line:"");std::string op;input>>op;
        if(op.empty()||op=="show"){
            const auto result=set_window_open(true);
            if(!result.value("ok",false))say(result.value("error",std::string{}));
            return;
        }
        if(op=="list"){size_t i=0;for(const auto& a:engine.config["alerts"])say(std::to_string(++i)+". "+a.value("name",std::string{})+(a.value("enabled",true)?" [on]":" [off]"));return;}
        if(op=="test"){size_t n=0;input>>n;const auto& list=engine.config["alerts"];if(n&&n<=list.size()){
            const auto result=preview_rule(list[n-1]);if(!result.value("ok",false))say(result.value("error",std::string{}));return;}
            say("Use /sentinel test <alert number>. /sentinel list shows the numbers.");return;}
        if(op=="on"||op=="off"||op=="toggle"||op=="hide"){const auto result=action(op,"");say(result.value("ok",false)?status:result.value("error",std::string{}));return;}
        say("/sentinel opens the library. Commands: show, hide, on, off, toggle, list, test <number>.");
    }catch(...){log_error("Command");}
}
}
HAILSTONE_API void InitializePlugin() noexcept {
    try {
        if(!host||host->size<offsetof(PluginHostApi,character_config_dir)+sizeof(host->character_config_dir))return;
        if(initialized)return;
        initialized=true;engine=Engine();presentation_defaults(engine.config);folder.clear();snapshot={};
        timer_runs.clear(); ++timer_profile;
        history.clear();overlays.clear();error.clear();load_failed=false;last_poll=0;
        load_window_preferences();
#ifndef SENTINEL_NO_AUDIO
        audio=std::make_unique<sentinel::Audio>();
#endif
        host->add_command(host->host,"/sentinel",command,nullptr);
        static std::array kinds = {Ui::State, Ui::Action, Ui::Save, Ui::Preview, Ui::Export, Ui::Inspect, Ui::Import,
            Ui::Settings, Ui::TimerSave, Ui::TimerAction, Ui::TimerLayout};
        static_assert(kinds.size() == ui_names.size());
        for (size_t i = 0; i < kinds.size(); ++i) host->add_ui_function(host->host, ui_names[i], ui_dispatch, &kinds[i]);
        if(host->add_menu)host->add_menu(host->host,"Sentinel alerts","/sentinel");
        refresh_snapshot();say("Ready. Open Sentinel from the game menu or type /sentinel.");
    }catch(...){log_error("Initialize");}
}
HAILSTONE_API void ShutdownPlugin() noexcept {
    try {cancel_activity();audio.reset();if(host&&initialized){host->remove_command(host->host,"/sentinel");
        for (const auto* name : ui_names) host->remove_ui_function(host->host, name);}
        timer_runs.clear();
        initialized=false;folder.clear();snapshot={};history.clear();overlays.clear();}
    catch(...){log_error("Shutdown");}
}
HAILSTONE_API void OnPulse() noexcept {
    try {if(!initialized)return;const auto now=now_ms();if(now>=last_poll&&now-last_poll<100)return;last_poll=now;
        refresh_snapshot();sample_timers(now);for(const auto& f:engine.evaluate(snapshot,now))deliver(f);
        dismiss_finished_timers(now);}
    catch(...){log_error("Observation");}
}
HAILSTONE_API int OnIncomingChat(const char* line,uint32_t color) noexcept {
    try {if(!initialized||delivering||!line)return 0;const auto text=bounded_string(line,2048);
        if(text.rfind("[Sentinel]",0)==0)return 0;
        for(const auto& f:engine.chat(snapshot,text,color,now_ms()))deliver(f);}
    catch(...){log_error("Chat");}return 0;
}
HAILSTONE_API void OnBeginZone() noexcept {try{cancel_activity();snapshot.in_zone=false;}catch(...){log_error("Zone");}}
HAILSTONE_API void OnZoned() noexcept {try{last_poll=0;refresh_snapshot();}catch(...){log_error("Zone");}}
HAILSTONE_API void SetGameState(int state) noexcept {try{if(state!=GAMESTATE_INGAME){cancel_activity();snapshot={};}else last_poll=0;}catch(...){log_error("Game state");}}
#ifdef SENTINEL_TESTING
extern "C" void sentinel_test_time(uint64_t ms) {test_now=ms;}
#endif
