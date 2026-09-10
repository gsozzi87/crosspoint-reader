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
  // Corta TODAS las grabaciones abiertas. Es el gesto de sacudir: "cancelá lo
  // que estés haciendo", que tiene que funcionar desde el loop de main.cpp sin
  // saber qué Activity abrió el micrófono. La Activity se entera porque
  // isRecording() pasa a false y su pump() la da por terminada.
  static void abortAll();
  // Closes the mic and finalises the WAV header.
  void stop();
  // Closes the mic and drops the take.
  void abort();
  // Frees the buffer (after the upload).
  void release();

  bool tooShort() const { return spokenSamples() < SAMPLE_RATE / 2; }  // < 0.5 s = accidental press
  // Short tones through the speaker when the mic opens (high) and closes (low),
  // so the user knows when to talk without looking. On by default.
  void setBlips(bool on) { blips = on; }
  size_t samples() const { return recorded; }
  float seconds() const { return recorded / static_cast<float>(SAMPLE_RATE); }
  // Samples of the take that are actually the user talking: everything from the
  // opening tone (and the DMA already in flight when it started) is dropped.
  size_t spokenSamples() const { return recorded > spokenStart ? recorded - spokenStart : 0; }
  float spokenSeconds() const { return spokenSamples() / static_cast<float>(SAMPLE_RATE); }
  // The WAV of the take WITHOUT the tone: the RIFF header is rewritten right in
  // front of the first spoken sample, so this is a plain pointer into the same
  // buffer (no copy, no second allocation).
  const uint8_t* wav() const;
  size_t wavBytes() const;
  // Same take as ADPCM (4x smaller): what actually gets uploaded. It is NOT
  // encoded in place: a second PSRAM buffer holds it, so the WAV take and the
  // ADPCM copy are alive at the same time (that peak is what has to fit).
  // Null if that allocation failed; release() frees both.
  const uint8_t* adpcm();
  // 0 until adpcm() has actually produced the buffer, so no caller can post a
  // size that goes with a null pointer.
  size_t adpcmBytes() const;

  // Cheap "was anything said?" test over the trimmed take, so a silent room
  // does not travel to the server to come back as an empty transcription.
  // 15 ms windows: at least three whose peak stands well above the noise floor
  // (4x the median window peak, and over an absolute floor), spanning at least
  // half a second.
  bool hasSpeech() const;

  // --- Playback of the take (voice-note review) -----------------------------
  // Reopens the codec and plays the trimmed take. The mic must be closed
  // (stop()) first: the I2S port is one. False if it could not start.
  bool playSpoken();
  bool isPlayingBack();
  void stopPlayback();

 private:
  AudioManager audio;
  uint8_t* buffer = nullptr;
  size_t maxSamples;
  size_t recorded = 0;
  bool recording = false;
  static int s_open;  // grabadoras con el micrófono abierto ahora mismo
  // Las grabadoras vivas, para poder cortarlas desde afuera (abortAll). Son una
  // o dos: cada pantalla crea la suya y la suelta al salir.
  static VoiceRecorder* s_live[4];
  void registerLive();
  void unregisterLive();
  bool blips = true;
  uint8_t* packed = nullptr;  // ADPCM view of the take
  uint8_t* tone = nullptr;    // opening tone, played while the mic is already open
  bool playingBack = false;
  // Samples recorded before the user could possibly be talking: the capture is
  // opened BEFORE the tone (that is the whole point — the first word used to be
  // lost while the tone played to completion), so the tone itself, the RX DMA
  // that was already in flight and a short tail all land at the head of the
  // buffer and are trimmed off here.
  size_t spokenStart = 0;

  // Plays the opening tone WITHOUT waiting for it: returns the millis() at which
  // playback was handed to the audio task, or 0 if there is no tone.
  unsigned long startBlip(uint16_t hz, uint16_t ms);
  void blip(uint16_t hz, uint16_t ms);
  void freeTone();
};
