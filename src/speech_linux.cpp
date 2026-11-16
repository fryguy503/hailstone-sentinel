// SPDX-License-Identifier: MIT
#include "speech_linux.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern char** environ;

namespace sentinel {
std::filesystem::path linux_speech_program() {
    for(const auto* name:{"/usr/bin/espeak-ng","/usr/local/bin/espeak-ng","/bin/espeak-ng",
        "/usr/bin/espeak","/usr/local/bin/espeak","/bin/espeak"}) {
        std::error_code ec;
        if(std::filesystem::is_regular_file(name,ec)&&access(name,X_OK)==0)return name;
    }
    return {};
}
bool linux_speak(const std::filesystem::path& program,const std::string& text,
    int voice,int rate,int volume,const std::function<bool()>& valid) {
    if(program.empty()||text.empty()||text.size()>512||text.find('\0')!=std::string::npos||!valid())return false;
    const auto words=static_cast<int>(std::lround(175*std::pow(2.0,std::clamp(rate,-10,10)/10.0)));
    std::vector<std::string> args={program.string(),"-b","1","-a",std::to_string(std::clamp(volume,0,100)),"-s",std::to_string(words)};
    // eSpeak variants need a base voice. en+f3 ships with both supported tools.
    if(voice==1){args.push_back("-v");args.push_back("en+f3");}
    args.push_back("--");args.push_back(text);
    std::vector<char*> argv;for(auto& s:args)argv.push_back(s.data());argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)!=0)return false;
    int rc=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if(rc==0)rc=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    if(rc==0)rc=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    pid_t pid=-1;
    if(rc==0)rc=posix_spawn(&pid,program.c_str(),&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(rc!=0)return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    int status=0;
    for(;;) {
        const auto state=waitpid(pid,&status,WNOHANG);
        if(state==pid)return WIFEXITED(status)&&WEXITSTATUS(status)==0;
        if(state<0&&errno!=EINTR)return false;
        if(!valid()||std::chrono::steady_clock::now()>=deadline)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Cancel only our child, never speech belonging to another application.
    kill(pid,SIGTERM);
    for(int i=0;i<20;++i) {
        const auto state=waitpid(pid,&status,WNOHANG);
        if(state==pid||(state<0&&errno!=EINTR))return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid,SIGKILL);
    while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    return false;
}
}
