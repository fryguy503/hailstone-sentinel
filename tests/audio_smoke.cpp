// SPDX-License-Identifier: MIT
// Exercise real backend initialization/teardown without playing or speaking.
#include "audio.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

int main() {
    std::atomic<bool> finished{false};
    std::exception_ptr failure;
    std::thread test([&]{
        try {
            sentinel::Audio audio;
            audio.cancel();
        }catch(...){failure=std::current_exception();}
        finished=true;
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!finished&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if(!finished){std::cerr<<"Audio backend teardown timed out.\n";std::_Exit(1);}
    test.join();
    if(failure){std::cerr<<"Audio backend initialization threw an exception.\n";return 1;}
    std::cout<<"Audio backend initialization/cancel/teardown passed; no playback requested.\n";
}
