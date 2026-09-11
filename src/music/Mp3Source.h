#pragma once

#include <AudioManager.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>

// One MP3 file on the SD, presented to the SDK's AudioManager as a WAV
// source: a synthesized 44-byte header followed by PCM decoded on demand
// (Helix, fixed point) inside the audio task's read() calls. ID3v2/v1 tags
// give title and artist; the first frame gives rate, channels and bitrate,
// and the duration is estimated from the file size (exact for CBR).
class Mp3Source {
 public:
  ~Mp3Source() { close(); }

  bool open(const std::string& path);
  void close();
  bool isOpen() const { return file_.isOpen(); }

  AudioManager::WavSource wavSource();

  const std::string& title() const { return title_; }
  const std::string& artist() const { return artist_; }
  int sampleRate() const { return sampleRate_; }
  int channels() const { return channels_; }
  int bitrateKbps() const { return bitrate_ / 1000; }
  int durationSeconds() const { return duration_; }
  // Seconds decoded so far (the playback position, give or take the DMA lag).
  int positionSeconds() const { return sampleRate_ > 0 ? static_cast<int>(samplesOut_ / sampleRate_) : 0; }
  bool finished() const { return eof_ && pcmAvail_ == 0; }

  // Historial de nivel para el analizador de la pantalla. NO es una FFT (no hay
  // presupuesto ni sentido: el panel se repinta cada varios segundos): es el
  // pico real de cada bloque decodificado, que dibujado como barras se lee
  // igual que el analizador de Winamp y encima dice la verdad sobre el audio.
  // Lo escribe la tarea de audio y lo lee la UI; son bytes sueltos, una lectura
  // a destiempo pinta una barra distinta y nada más.
  static constexpr int LEVELS = 24;
  uint8_t level(const int i) const { return levels_[(levelPos_ + i) % LEVELS]; }  // 0..15, del más viejo al más nuevo

 private:
  static constexpr size_t IN_BUF = 4096;
  static constexpr size_t PCM_BUF = 1152 * 2 * 2 * 2;  // two full stereo frames, bytes

  HalFile file_;
  void* decoder_ = nullptr;
  uint8_t* inBuf_ = nullptr;
  size_t inLen_ = 0;
  uint8_t* pcmBuf_ = nullptr;
  size_t pcmAvail_ = 0;
  size_t pcmPos_ = 0;
  size_t audioStart_ = 0;  // after the ID3v2 tag
  size_t fileSize_ = 0;
  bool eof_ = false;
  uint64_t samplesOut_ = 0;
  uint8_t levels_[LEVELS] = {0};
  int levelPos_ = 0;      // dónde entra el próximo (y, por eso, el más viejo)
  uint16_t levelPeak_ = 0;
  int levelFrames_ = 0;

  std::string title_;
  std::string artist_;
  int sampleRate_ = 0;
  int channels_ = 0;
  int bitrate_ = 0;
  int duration_ = 0;

  uint8_t header_[44];
  size_t headerPos_ = 0;
  bool inHeader_ = true;

  void parseId3v2();
  void parseId3v1();
  bool fillInput();
  bool decodeFrame();  // refills pcmBuf_; false at EOF / error
  int readPcm(uint8_t* dst, size_t len);
  void buildHeader();
};
