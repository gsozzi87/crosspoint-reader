#pragma once

#include <AudioManager.h>

#include <cstddef>
#include <cstdint>

// The device's attention sound: three 880 Hz beeps and a pause, looped through
// the speaker until stop(). Shared by reminders, the timer and the alarm.
class AlertBeep {
 public:
  ~AlertBeep() { stop(); }
  bool start(uint8_t volume = 85);
  void stop();
  bool isPlaying() const { return playing; }

 private:
  AudioManager audio;
  uint8_t* wav = nullptr;
  bool playing = false;
};
