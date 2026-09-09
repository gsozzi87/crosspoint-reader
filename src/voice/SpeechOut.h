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
  // volume 0 = el volumen del aparato (HubStore::musicVolume).
  bool playAdpcm(const uint8_t* data, size_t len, uint8_t volume = 0);
  bool playFile(const char* path, uint8_t volume = 0);
  bool isPlaying() const { return started && audio.isPlaying(); }
  bool hasStarted() const { return started; }
  void stop();

  // Pausa de verdad. El SDK no tiene pause: parar y volver a reproducir
  // arrancaba el clip de cero, así que al leer una noticia "Seguir" te hacía
  // escuchar el párrafo entero otra vez. Como en la música, la pausa baja el
  // volumen a cero y deja el stream corriendo.
  void pause();
  void resume();
  bool isPaused() const { return paused; }

 private:
  AudioManager audio;
  uint8_t* wav = nullptr;
  bool started = false;
  bool paused = false;
};
