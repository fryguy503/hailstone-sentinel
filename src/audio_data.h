// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sentinel::audio_detail {
struct Pcm {
    unsigned rate=22050,channels=1;
    std::vector<int16_t> samples;
};
Pcm read_wav(const std::filesystem::path&);
Pcm make_tone(const std::string&);
void apply_volume(Pcm&,int volume);
}
