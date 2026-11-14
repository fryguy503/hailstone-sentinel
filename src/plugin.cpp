// SPDX-License-Identifier: MIT
#include "hailstone_plugin.h"
#include "audio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

HAILSTONE_PLUGIN
namespace {
using namespace sentinel;
namespace fs = std::filesystem;
constexpr size_t max_alerts=128, max_bytes=128*1024, max_config_bytes=1024*1024;
Engine engine;
Snapshot snapshot;
std::unique_ptr<Audio> audio;
fs::path folder;
bool open=true, delivering=false, initialized=false, load_failed=false;
uint64_t revision=0, last_poll=0, next_id=0;
std::string status="Choose a starter preset or create your first alert.", error;
Json history=Json::array(), overlays=Json::array();
std::vector<std::string> sounds;
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
}
Json config_normalized(const Json& raw) {
    if(raw.contains("version")&&raw["version"]!=1)throw std::runtime_error("Unsupported settings version");
    auto c=normalize_config(raw);
    for(const auto* key:{"overlay_x","overlay_y","overlay_locked","quiet"})if(raw.contains(key))c[key]=raw[key];
    presentation_defaults(c);return c;
}
bool persist() {
    if(folder.empty()){error="Enter the world before saving character alerts.";return false;}
    if(load_failed){error="Settings could not be read. Restore or rename config.json before saving; the original has been preserved.";return false;}
    try {
        fs::create_directories(folder);
        const auto path=folder/"config.json", temp=folder/"config.json.tmp";
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);
         if(!out)throw std::runtime_error("Cannot create settings file");
         const auto text=engine.config.dump(2);
         if(text.size()>max_config_bytes)throw std::runtime_error("Settings exceed 1 MiB");
         out.write(text.data(),static_cast<std::streamsize>(text.size()));out.flush();
         if(!out)throw std::runtime_error("Cannot write settings file");}
#ifdef _WIN32
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace settings file");
#else
        fs::rename(temp,path);
#endif
        ++revision;error.clear();return true;
    } catch(const std::exception& ex){error=ex.what();return false;}
}
template<typename Fn> Json change(Fn fn) {
    const auto previous=engine.config;
    try{fn();if(!persist()){engine.config=previous;return failure(error);}return success();}
    catch(const std::exception& ex){engine.config=previous;return failure(ex.what());}
}
void cancel_activity() {engine.reset();overlays.clear();if(audio)audio->cancel();}
void load_for(const fs::path& next) {
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
    if(host->get_buff)for(uint32_t i=0;i<std::min(player.buff_slots,512u);++i) {
        HailstoneBuff b{};b.size=sizeof b;
        if(!host->get_buff(host->host,i,&b)||!b.spell_id)continue;
        Buff buff;buff.id=b.spell_id;
        HailstoneSpell spell{};spell.size=sizeof spell;
        if(host->get_spell&&host->get_spell(host->host,b.spell_id,&spell))buff.name=spell.name;
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
            folder.clear();engine.config=defaults();presentation_defaults(engine.config);cancel_activity();
            history.clear();sounds.clear();status="Waiting for the character settings folder.";
        }
    }
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
void validate_rule(const Json& a) {
    const auto event=a.value("event",std::string{}),subject=a.value("subject",std::string{});
    const bool required=event=="player_health_below"||event=="player_mana_below"||event=="buff_fading"||
        event=="buff_missing"||event=="spawn_appears"||event=="player_appears"||event=="chat_text_match";
    if(required&&(subject.empty()||subject.find_first_not_of(" \t\r\n")==std::string::npos))
        throw std::runtime_error("Enter a name or text pattern for this event.");
    if(!a.value("chat",false)&&!a.value("overlay",false)&&!a.value("sound",false)&&!a.value("speech",false))
        throw std::runtime_error("Choose at least one notification: banner, chat, sound, or speech.");
}
enum class Ui {State,Action,Save,Preview,Export,Inspect,Import,Settings};
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
        "speech_text","voice","rate","color","size","duration","overlay_x","overlay_y","overlay_locked","quiet"}) {
        HailstoneValue field{};if(host->arg_field(host->host,args,n,key,&field))object[key]=value(field);
    }return object;
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
    result["open"]=open;result["in_zone"]=snapshot.in_zone;result["character"]=snapshot.character;
    result["zone_id"]=snapshot.zone_id;
    char zone[128]{};if(host->zone_name)host->zone_name(host->host,snapshot.zone_id,zone,sizeof zone);
    static const auto catalog=events(), starter_presets=presets();
    result["zone_name"]=zone;result["catalog"]=catalog;result["presets"]=starter_presets;result["history"]=history;
    result["overlays"]=showing;result["status"]=status;result["error"]=error;result["revision"]=revision;
    result["audio_available"]=audio&&audio->sound_available();result["speech_available"]=audio&&audio->speech_available();
    const auto path=(folder/"config.json").u8string();
    result["sounds"]=sounds;result["config_path"]=std::string(path.begin(),path.end());
    result["hp"]=snapshot.hp?Json(*snapshot.hp):Json();result["mana"]=snapshot.mana?Json(*snapshot.mana):Json();
    result["endurance"]=snapshot.endurance?Json(*snapshot.endurance):Json();return result;
}
Json action(const std::string& op,const std::string& id) {
    if(op=="show"){open=true;return success();}if(op=="hide"){open=false;return success();}
    if(op=="clear_history"){history.clear();return success();}
    if(op=="rescan_sounds"){sounds=Audio::files(folder/"Sounds");return success();}
    if(op=="test") {const auto* a=find(id);if(!a)return failure("Alert not found");deliver(engine.preview(*a,snapshot,now_ms()));return success();}
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
    if(kind==Ui::Action) {const auto op=arg(args,0),id=arg(args,1);return action(op.is_string()?op.get<std::string>():"",id.is_string()?id.get<std::string>():"");}
    if(kind==Ui::Save||kind==Ui::Preview) {
        const auto raw=arg(args,0);if(!raw.is_object())return failure("Expected an alert");
        auto a=normalize_alert(raw);
        if(kind==Ui::Preview){deliver(engine.preview(a,snapshot,now_ms()));return success();}
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
        for(auto& a:list)a["enabled"]=false;
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
        if(op.empty()||op=="show"){open=true;return;}
        if(op=="list"){size_t i=0;for(const auto& a:engine.config["alerts"])say(std::to_string(++i)+". "+a.value("name",std::string{})+(a.value("enabled",true)?" [on]":" [off]"));return;}
        if(op=="test"){size_t n=0;input>>n;const auto& list=engine.config["alerts"];if(n&&n<=list.size()){deliver(engine.preview(list[n-1],snapshot,now_ms()));return;}say("Use /sentinel test <alert number>. /sentinel list shows the numbers.");return;}
        if(op=="on"||op=="off"||op=="toggle"||op=="hide"){const auto result=action(op,"");say(result.value("ok",false)?status:result.value("error",std::string{}));return;}
        say("/sentinel opens the library. Commands: show, hide, on, off, toggle, list, test <number>.");
    }catch(...){log_error("Command");}
}
}
HAILSTONE_API void InitializePlugin() noexcept {
    try {
        if(!host||host->size<offsetof(PluginHostApi,character_config_dir)+sizeof(host->character_config_dir))return;
        if(initialized)return;
        initialized=true;engine=Engine();presentation_defaults(engine.config);open=true;folder.clear();snapshot={};
        history.clear();overlays.clear();error.clear();load_failed=false;last_poll=0;
#ifndef SENTINEL_NO_AUDIO
        audio=std::make_unique<sentinel::Audio>();
#endif
        host->add_command(host->host,"/sentinel",command,nullptr);
        static Ui kinds[]={Ui::State,Ui::Action,Ui::Save,Ui::Preview,Ui::Export,Ui::Inspect,Ui::Import,Ui::Settings};
        const char* names[]={"state","action","save","preview","export","inspect","import","settings"};
        for(size_t i=0;i<8;++i)host->add_ui_function(host->host,names[i],ui_dispatch,&kinds[i]);
        if(host->add_menu)host->add_menu(host->host,"Sentinel alerts","/sentinel");
        refresh_snapshot();say("Ready. Open Sentinel from the game menu or type /sentinel.");
    }catch(...){log_error("Initialize");}
}
HAILSTONE_API void ShutdownPlugin() noexcept {
    try {cancel_activity();audio.reset();if(host&&initialized){host->remove_command(host->host,"/sentinel");
        for(const auto* name:{"state","action","save","preview","export","inspect","import","settings"})host->remove_ui_function(host->host,name);}
        initialized=false;folder.clear();snapshot={};history.clear();overlays.clear();}
    catch(...){log_error("Shutdown");}
}
HAILSTONE_API void OnPulse() noexcept {
    try {if(!initialized)return;const auto now=now_ms();if(now>=last_poll&&now-last_poll<100)return;last_poll=now;
        refresh_snapshot();for(const auto& f:engine.evaluate(snapshot,now))deliver(f);}
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
