// SPDX-License-Identifier: MIT
#include "speech_macos.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    bool available = false;
    std::atomic<bool> finished{false};
    std::exception_ptr problem;
    const char* opt_in = std::getenv("SENTINEL_TEST_SPEECH");
    const bool audible = opt_in && std::string(opt_in) == "1";
    std::thread worker([&] {
        try {
            available = sentinel::macos_speech_available();
            const auto start = std::chrono::steady_clock::now();
            sentinel::macos_speak("A cancelled alert must stay silent.", 0, 0, 100, [] { return false; });
            sentinel::macos_speak("", 1, 10, 100, [] { return true; });
            sentinel::macos_speak(std::string("\xff", 1), 1, -10, 100, [] { return true; });
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
                throw std::runtime_error("Silent cancellation/input guards did not return promptly.");
            if (audible && available) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
                sentinel::macos_speak("Sentinel speech is ready.", 0, 0, 65,
                    [&] { return std::chrono::steady_clock::now() < deadline; });
            }
        } catch (...) { problem = std::current_exception(); }
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(audible ? 25 : 15);
    while (!finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!finished) {
        std::cerr << "macOS speech smoke timed out.\n";
        std::_Exit(1); // Do not hang CI by joining a stuck system service.
    }
    worker.join();
    if (problem) {
        try { std::rethrow_exception(problem); }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        return 1;
    }
    std::cout << "macOS voice available: " << (available ? "yes" : "no")
              << "; silent guards passed; speech " << (audible && available ? "requested" : "not requested") << ".\n";
    return 0;
}
