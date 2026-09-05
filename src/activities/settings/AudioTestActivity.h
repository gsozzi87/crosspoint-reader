#pragma once

#include <AudioManager.h>
#include <I18n.h>

#include <cstdint>

#include "activities/Activity.h"

// Audio diagnostic: records a few seconds from the board's microphone (the
// codec ADC path, e.g. the ws397's ES8311 MIC1) into PSRAM and plays the take
// back through the speaker, reporting the peak level so a dead mic is
// distinguishable from a dead speaker. Boards with a speaker but no codec mic
// play a test tone instead, so the output path still gets exercised.
class AudioTestActivity final : public Activity {
 public:
  explicit AudioTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AudioTest", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING || state == PLAYING; }
  bool preventAutoSleep() override { return state == RECORDING || state == PLAYING; }

 private:
  enum State { IDLE, RECORDING, PLAYING, DONE, FAILED };

  static constexpr uint32_t SAMPLE_RATE = 16000;
  static constexpr uint32_t SECONDS = 3;
  static constexpr size_t SAMPLES = SAMPLE_RATE * SECONDS;
  static constexpr size_t WAV_HEADER = 44;

  State state = IDLE;
  StrId failureId = StrId::STR_AUDIO_INIT_FAILED;

  AudioManager audio;
  uint8_t* wav = nullptr;  // WAV_HEADER + SAMPLES * 2 bytes, PSRAM when available
  size_t recorded = 0;     // samples captured so far
  int16_t peak = 0;
  bool usedMic = false;

  int16_t* samples() { return reinterpret_cast<int16_t*>(wav + WAV_HEADER); }
  bool allocate();
  void startTake();
  void pumpCapture();
  void finishTake();
  void writeWavHeader();
  void fillTestTone();
  void fail(StrId why);
};
