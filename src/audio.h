// SPDX-License-Identifier: MIT
#pragma once
#include "engine.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace sentinel {
// Owns all platform audio. The worker never calls the Hailstone host.
class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    void enqueue(const Fired&, const std::filesystem::path& sounds);
    void cancel();
    bool sound_available() const;
    bool speech_available() const;
    static bool safe_filename(const std::string&);
    static std::vector<std::string> files(const std::filesystem::path&);
private:
    struct Request { Fired fired; std::filesystem::path sounds; uint64_t generation; };
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> queue_;
    std::atomic<bool> stopping_{false}, sound_{false}, speech_{false};
    std::atomic<uint64_t> generation_{0};
    std::thread worker_;
    void run();
};
}
