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

  // ¿Hay ALGÚN micrófono abierto en el aparato? Cada pantalla tiene su propia
  // grabadora, pero el códec es uno solo: main.cpp lo consulta para no abrir un
  // recordatorio (ni el atajo de voz) encima de una grabación en curso, que
  // dejaba el micrófono colgado y la toma perdida.
  static bool anyRecording() { return s_open > 0; }
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
  // Same take as ADPCM (4x smaller): what actually gets uploaded. It is NOT
  // encoded in place: a second PSRAM buffer holds it, so the WAV take and the
  // ADPCM copy are alive at the same time (that peak is what has to fit).
  // Null if that allocation failed; release() frees both.
  const uint8_t* adpcm();
  // 0 until adpcm() has actually produced the buffer, so no caller can post a
  // size that goes with a null pointer.
  size_t adpcmBytes() const;

 private:
  AudioManager audio;
  uint8_t* buffer = nullptr;
  size_t maxSamples;
  size_t recorded = 0;
  bool recording = false;
  static int s_open;  // grabadoras con el micrófono abierto ahora mismo
  bool blips = true;
  uint8_t* packed = nullptr;  // ADPCM view of the take

  void blip(uint16_t hz, uint16_t ms);
};
