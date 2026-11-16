// SPDX-License-Identifier: MIT
// This executable is its own fake eSpeak child. No shell, installed TTS, or sound device is used.
#include "speech_linux.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
int checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
struct Environment {
    std::string name;
    std::optional<std::string> previous;
    Environment(std::string key,const std::string& value):name(std::move(key)) {
        if(const auto* old=std::getenv(name.c_str()))previous=old;
        if(setenv(name.c_str(),value.c_str(),1)!=0)throw std::runtime_error("Could not set fixture environment");
    }
    ~Environment() {if(previous)setenv(name.c_str(),previous->c_str(),1);else unsetenv(name.c_str());}
};
int child_main(int argc,char** argv) {
    const char* location=std::getenv("SENTINEL_FAKE_SPEECH_LOG");
    const char* mode_value=std::getenv("SENTINEL_FAKE_SPEECH_MODE");
    if(!location)return 9;
    const std::string mode=mode_value?mode_value:"ok";
    if(mode=="sleep")std::signal(SIGTERM,SIG_IGN); // Exercise the bounded SIGKILL fallback too.
    {
        std::ofstream out(location,std::ios::binary|std::ios::trunc);
        out<<getpid()<<'\n'<<argc-1<<'\n';
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];out<<arg.size()<<'\n';out.write(arg.data(),static_cast<std::streamsize>(arg.size()));out.put('\n');
        }
        out.flush();if(!out)return 8;
    }
    {std::ofstream ready(std::string(location)+".ready");ready<<"ready";}
    if(mode=="sleep")for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return mode=="failure"?7:0;
}
struct Record {pid_t pid=-1;std::vector<std::string> args;};
struct Fixture {
    fs::path root,program,log,ready;
    explicit Fixture(const fs::path& executable) {
        const auto serial=std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root=fs::temp_directory_path()/("sentinel-speech-linux-test-"+std::to_string(serial));fs::create_directories(root);
        program=root/"fake speech with spaces";log=root/"arguments.bin";ready=root/"arguments.bin.ready";
        fs::copy_file(executable,program);fs::permissions(program,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec);
    }
    ~Fixture() {std::error_code ec;fs::remove_all(root,ec);}
    void clear() {std::error_code ec;fs::remove(log,ec);fs::remove(ready,ec);}
    Record record() const {
        std::ifstream in(log,std::ios::binary);check(static_cast<bool>(in),"Fake child recorded its argv");
        std::string line;Record record;
        std::getline(in,line);record.pid=static_cast<pid_t>(std::stol(line));
        std::getline(in,line);const auto count=std::stoul(line);check(count<20,"Fake child argv count is bounded");
        for(unsigned long i=0;i<count;++i) {
            std::getline(in,line);const auto size=std::stoul(line);check(size<4096,"Recorded argument is bounded");
            std::string argument(size,'\0');in.read(argument.data(),static_cast<std::streamsize>(size));
            check(in.get()=='\n',"Recorded argument framing is intact");record.args.push_back(std::move(argument));
        }
        return record;
    }
};
std::string option(const Record& record,const std::string& name) {
    const auto found=std::find(record.args.begin(),record.args.end(),name);
    if(found==record.args.end()||found+1==record.args.end())throw std::runtime_error("Missing expected speech option");
    return *(found+1);
}
void literal_arguments_and_limits(const fs::path& executable) {
    Fixture f(executable);Environment location("SENTINEL_FAKE_SPEECH_LOG",f.log.string());Environment mode("SENTINEL_FAKE_SPEECH_MODE","ok");
    const std::string text="-v 'single quotes' \"double quotes\" ${Me.Name} $(literal) ;\na second line";
    check(sentinel::linux_speak(f.program,text,1,100,500,[]{return true;}),"Fake speech child can complete successfully");
    const auto high=f.record();
    check(high.args.size()>=2&&high.args[high.args.size()-2]=="--"&&high.args.back()==text,"Text beginning with a dash and shell syntax stays one literal argument after --");
    check(option(high,"-b")=="1","UTF-8 text mode is requested");
    check(option(high,"-a")=="100","Volume clamps to one hundred");
    check(option(high,"-s")=="350","Speech rate clamps to its upper bound");
    check(option(high,"-v")=="en+f3","Second voice uses an existing eSpeak base voice and variant");
    f.clear();
    check(sentinel::linux_speak(f.program,"quiet",0,-100,-20,[]{return true;}),"Low limit request completes");
    const auto low=f.record();
    check(option(low,"-a")=="0"&&option(low,"-s")=="88","Negative volume and rate clamp to supported lower bounds");
    check(std::find(low.args.begin(),low.args.end(),"-v")==low.args.end(),"Default voice leaves system voice selection alone");
}
void failures_and_pre_spawn_cancellation(const fs::path& executable) {
    Fixture f(executable);Environment location("SENTINEL_FAKE_SPEECH_LOG",f.log.string());
    {
        Environment mode("SENTINEL_FAKE_SPEECH_MODE","failure");
        check(!sentinel::linux_speak(f.program,"failed",0,0,50,[]{return true;}),"Nonzero child exit is reported as failure");
        check(fs::exists(f.ready),"Failure result came from the launched child");
    }
    f.clear();
    check(!sentinel::linux_speak(f.root/"missing-program","test",0,0,50,[]{return true;}),"Missing speech executable fails without invoking a shell");
    check(!fs::exists(f.log),"Missing program did not run a fallback process");
    check(!sentinel::linux_speak(f.program,"cancelled",0,0,50,[]{return false;}),"Already cancelled request is refused before spawn");
    check(!fs::exists(f.log),"Immediate cancellation never launches a child");
    for(const auto& text:std::vector<std::string>{"",std::string(513,'x'),std::string("before\0after",12)}) {
        check(!sentinel::linux_speak(f.program,text,0,0,50,[]{return true;}),"Invalid speech text is rejected before spawn");
        check(!fs::exists(f.log),"Invalid text never launches a child");
    }
    const auto installed=sentinel::linux_speech_program();
    check(installed.empty()||(installed.is_absolute()&&fs::is_regular_file(installed)&&access(installed.c_str(),X_OK)==0),"Optional speech discovery returns an absolute executable or unavailable");
}
struct UnrelatedChild {
    pid_t pid=-1;
    UnrelatedChild() {
        pid=fork();if(pid<0)throw std::runtime_error("Could not create unrelated fixture child");
        if(pid==0)for(;;)pause();
    }
    ~UnrelatedChild() {if(pid>0){kill(pid,SIGKILL);int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}
};
void cancellation_owns_and_reaps_its_child(const fs::path& executable) {
    Fixture f(executable);Environment location("SENTINEL_FAKE_SPEECH_LOG",f.log.string());Environment mode("SENTINEL_FAKE_SPEECH_MODE","sleep");
    UnrelatedChild other;
    const auto started=std::chrono::steady_clock::now();
    const bool result=sentinel::linux_speak(f.program,"cancel this owned process",0,0,50,[&]{
        return !fs::exists(f.ready)&&std::chrono::steady_clock::now()-started<std::chrono::milliseconds(500);
    });
    const auto elapsed=std::chrono::steady_clock::now()-started;
    check(!result,"Cancellation reports an interrupted speech request");
    check(elapsed<std::chrono::seconds(2),"Cancelling even a SIGTERM-resistant child is bounded below two seconds");
    const auto child=f.record();int status=0;errno=0;
    check(waitpid(child.pid,&status,WNOHANG)==-1&&errno==ECHILD,"Cancelled speech child has already been reaped");
    check(kill(other.pid,0)==0,"Cancellation leaves another application's process untouched");
}
}
int main(int argc,char** argv) {
    if(argc>1)return child_main(argc,argv);
    try {
        const fs::path executable=fs::canonical(argv[0]);
        literal_arguments_and_limits(executable);failures_and_pre_spawn_cancellation(executable);cancellation_owns_and_reaps_its_child(executable);
        std::cout<<"Linux speech process checks passed: "<<checks<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"Linux speech process test failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
