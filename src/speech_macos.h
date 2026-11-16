// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>

namespace sentinel {
// Call on Audio's worker. Availability means an installed system voice exists;
// it does not prove that the current Mac has an audible output device.
bool macos_speech_available();

// Plain UTF-8 text, not SSML or a command. The worker owns the synthesizer and
// pumps its own run loop; valid is an atomic-only cancellation predicate.
// Voice 0 uses the current language's default; 1 prefers an installed alternate
// in the same language and otherwise keeps that default.
void macos_speak(const std::string& text, int voice, int rate, int volume,
                 const std::function<bool()>& valid);
}
