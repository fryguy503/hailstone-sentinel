// SPDX-License-Identifier: MIT
#include "speech_macos.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace sentinel {
namespace {
using Clock = std::chrono::steady_clock;

bool live(const std::function<bool()>& valid) {
    // A stopped Audio owner must never be referenced from an Apple callback.
    // This predicate is called here, on the owning worker, and is never retained.
    try { return valid && valid(); } catch (...) { return false; }
}

NSString* language_key(NSString* language) {
    return [[language stringByReplacingOccurrencesOfString:@"_" withString:@"-"] lowercaseString];
}

AVSpeechSynthesisVoice* selected_voice(int choice) {
    NSString* language = [AVSpeechSynthesisVoice currentLanguageCode];
    AVSpeechSynthesisVoice* system = [AVSpeechSynthesisVoice voiceWithLanguage:language];
    // Leaving an utterance's voice unset preserves the system's own selection if
    // its current-language voice cannot be resolved explicitly.
    if (choice <= 0) return system;
    NSString* key = language_key(system ? system.language : language);
    if (key.length == 0) return system;
    NSString* base = [key componentsSeparatedByString:@"-"].firstObject;
    AVSpeechSynthesisVoice* same_language = nil;
    for (AVSpeechSynthesisVoice* candidate in [AVSpeechSynthesisVoice speechVoices]) {
        if (system && [candidate.identifier isEqualToString:system.identifier]) continue;
        NSString* candidate_key = language_key(candidate.language);
        if ([candidate_key isEqualToString:key]) return candidate;
        if (!same_language && [[candidate_key componentsSeparatedByString:@"-"].firstObject isEqualToString:base])
            same_language = candidate;
    }
    return same_language ? same_language : system;
}

float speech_rate(int rate) {
    const float setting = static_cast<float>(std::clamp(rate, -10, 10)) / 10.0f;
    const float normal = AVSpeechUtteranceDefaultSpeechRate;
    return setting < 0
        ? normal + setting * (normal - AVSpeechUtteranceMinimumSpeechRate)
        : normal + setting * (AVSpeechUtteranceMaximumSpeechRate - normal);
}

void pump_slice() {
    // NSRunLoop belongs to the calling worker. If no source is installed it may
    // return immediately, so sleep the unused portion instead of busy-spinning.
    const auto until = Clock::now() + std::chrono::milliseconds(20);
    @autoreleasepool {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.020]];
    }
    std::this_thread::sleep_until(until);
}
}

bool macos_speech_available() {
    @autoreleasepool {
        @try {
            return [AVSpeechSynthesisVoice speechVoices].count > 0;
        } @catch (NSException* exception) {
            (void)exception;
            return false;
        }
    }
}

void macos_speak(const std::string& text, int voice, int rate, int volume,
                 const std::function<bool()>& valid) {
    if (text.empty() || volume <= 0 || !live(valid)) return;
    @autoreleasepool {
        @try {
            NSString* words = [[NSString alloc] initWithBytes:text.data()
                                                     length:text.size()
                                                   encoding:NSUTF8StringEncoding];
            if (!words || words.length == 0 || !live(valid)) return;
            AVSpeechSynthesizer* synth = [[AVSpeechSynthesizer alloc] init];
            AVSpeechUtterance* utterance = [[AVSpeechUtterance alloc] initWithString:words];
            if (!synth || !utterance) return;
            utterance.voice = selected_voice(std::clamp(voice, 0, 1));
            utterance.rate = speech_rate(rate);
            utterance.volume = static_cast<float>(std::clamp(volume, 0, 100)) / 100.0f;
            if (!live(valid)) return;
            // AVSpeechSynthesizer's documented lifetime requires our ownership:
            // developer.apple.com/documentation/avfaudio/avspeechsynthesizer
            // Keep it strongly
            // owned by this ARC scope until completion or explicit cancellation.
            // No main-queue blocks, delegates, or plugin callbacks survive it.
            [synth speakUtterance:utterance];
            const auto begin = Clock::now();
            const auto startup_limit = begin + std::chrono::seconds(5);
            const auto finish_limit = begin + std::chrono::seconds(120);
            bool started = false;
            while (live(valid) && Clock::now() < finish_limit) {
                if (synth.isSpeaking) started = true;
                else if (started || Clock::now() >= startup_limit) break;
                pump_slice();
            }
            // Also clear an utterance that never started (missing/cold voice or
            // unavailable output), so it cannot unexpectedly play at a later time.
            [synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        } @catch (NSException* exception) {
            // Voice service/device failures are isolated to this notification.
            (void)exception;
        }
    }
}
}
