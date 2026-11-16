// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <functional>
#include <string>
namespace sentinel {
std::filesystem::path linux_speech_program();
bool linux_speak(const std::filesystem::path& program,const std::string& text,
    int voice,int rate,int volume,const std::function<bool()>& valid);
}
