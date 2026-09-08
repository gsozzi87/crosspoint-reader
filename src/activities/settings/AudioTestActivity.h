#pragma once

#include <AudioManager.h>
#include <I18n.h>

#include <cstdint>

#include "activities/Activity.h"

// Ganancia del micrófono guardada en la SD. El aparato no tiene teclado y el
// ajuste no vive en la lista de Ajustes: se calibra acá, en la prueba de audio,
// mirando el nivel pico. Vale para todo el aparato (el dictado incluido) porque
// AudioManager la guarda estática, y main.cpp la aplica al arrancar.
namespace micgain {
constexpr char FILE_PATH[] = "/.crosspoint/micgain.txt";
void loadAndApply();          // desde main.cpp, apenas monta la SD
void save(uint8_t percent);   // desde la prueba de audio, al salir
}  // namespace micgain

// Audio diagnostic: records a few seconds from the board's microphone (the
// codec ADC path, e.g. the ws397's ES8311 MIC1) into PSRAM and plays the take
// back through the speaker, reporting the peak level so a dead mic is
// distinguishable from a dead speaker. Boards with a speaker but no codec mic
// play a test tone instead, so the output path still gets exercised.
//
// En las placas con micrófono por el códec también es el calibrador de la
// ganancia: Arriba y Abajo la mueven de a 10 %, la pantalla muestra el pico de
// la última toma con su barra y dice si está bajo, bien o saturando. La idea es
// grabar hablando a 15-20 cm y dejar la ganancia donde el pico caiga entre el
// 50 y el 70 %.
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
  static constexpr uint8_t GAIN_STEP = 10;  // lo que mueve cada toque de la palanca
  // Una muestra a partir de acá ya está pegada al tope de la escala.
  static constexpr int16_t CLIP_LEVEL = 31000;

  State state = IDLE;
  StrId failureId = StrId::STR_AUDIO_INIT_FAILED;

  AudioManager audio;
  uint8_t* wav = nullptr;  // WAV_HEADER + SAMPLES * 2 bytes, PSRAM when available
  size_t recorded = 0;     // samples captured so far
  int16_t peak = 0;
  size_t clipped = 0;  // muestras contra el tope: eso es saturación, no volumen
  bool usedMic = false;
  bool haveTake = false;    // ya se grabó algo en esta visita
  bool gainDirty = false;   // hay que guardar la ganancia al salir

  int16_t* samples() { return reinterpret_cast<int16_t*>(wav + WAV_HEADER); }
  bool allocate();
  void startTake();
  void pumpCapture();
  void finishTake();
  void writeWavHeader();
  void fillTestTone();
  void fail(StrId why);
  void nudgeGain(int delta);
  void drawGainRow(int y, int width) const;
  void drawLevelRow(int y, int width) const;
};
