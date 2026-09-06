#pragma once

#include <AudioManager.h>
#include <I18n.h>

#include <cstddef>
#include <cstdint>

// One voice take from the codec mic into a PSRAM WAV buffer (16 kHz mono
// 16-bit), pumped from an Activity's loop() so the buttons stay responsive.
// The device has no keyboard: this is how every free-text input gets in.
class VoiceRecorder {
 public:
  static constexpr uint32_t SAMPLE_RATE = 16000;

  explicit VoiceRecorder(uint32_t maxSeconds = 10) : maxSamples(SAMPLE_RATE * maxSeconds) {}
  ~VoiceRecorder() { abort(); }

  // Allocates the buffer and opens the mic. On failure `why` names the reason.
  bool start(StrId& why);
  // Drains one block from I2S. False on a capture error. Stops by itself when full.
  bool pump();
  bool isRecording() const { return recording; }
  // Closes the mic and finalises the WAV header.
  void stop();
  // Closes the mic and drops the take.
  void abort();
  // Frees the buffer (after the upload).
  void release();

  bool tooShort() const { return recorded < SAMPLE_RATE / 2; }  // < 0.5 s = accidental press
  // Short tones through the speaker when the mic opens (high) and closes (low),
  // so the user knows when to talk without looking. On by default.
  void setBlips(bool on) { blips = on; }
  size_t samples() const { return recorded; }
  float seconds() const { return recorded / static_cast<float>(SAMPLE_RATE); }
  const uint8_t* wav() const { return buffer; }
  size_t wavBytes() const;

 private:
  AudioManager audio;
  uint8_t* buffer = nullptr;
  size_t maxSamples;
  size_t recorded = 0;
  bool recording = false;
  bool blips = true;

  void blip(uint16_t hz, uint16_t ms);
};
