#pragma once

#include <AudioManager.h>

#include <cstddef>
#include <cstdint>

// Plays one spoken clip (ADPCM from the server, in memory or cached on the SD)
// through the speaker. Non-blocking: playback runs in the SDK's audio task,
// poll isPlaying(). The decoded WAV lives in PSRAM until stop().
class SpeechOut {
 public:
  ~SpeechOut() { stop(); }
  bool playAdpcm(const uint8_t* data, size_t len, uint8_t volume = 85);
  bool playFile(const char* path, uint8_t volume = 85);
  bool isPlaying() const { return started && audio.isPlaying(); }
  bool hasStarted() const { return started; }
  void stop();

 private:
  AudioManager audio;
  uint8_t* wav = nullptr;
  bool started = false;
};
