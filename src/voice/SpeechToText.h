#pragma once

#include <string>

class VoiceRecorder;

// POST /api/transcribe on the device's server: WAV in, recognised text out.
// WiFi must be up. Blocks for the round trip (up to 60 s), so call it from a
// state the Activity paints first.
namespace SpeechToText {
// True with `text` filled on success; otherwise `detail` says what failed
// (short, for the screen) and text is empty.
bool transcribe(const VoiceRecorder& take, std::string& text, std::string& detail);
}  // namespace SpeechToText
