#include "AlertBeep.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <cmath>

#include "util/WavHeader.h"

namespace {
constexpr uint32_t RATE = 16000;
constexpr float PATTERN_S = 1.6f;  // 3 x (0.16 s on + 0.16 s off) + pause
}  // namespace

bool AlertBeep::start(const uint8_t volume) {
  if (playing) return true;
  if (!BoardConfig::hasAudio()) return false;
  const size_t samples = static_cast<size_t>(RATE * PATTERN_S);
  const size_t bytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  if (!wav) wav = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!wav) return false;
  int16_t* pcm = reinterpret_cast<int16_t*>(wav + wav::HEADER_BYTES);
  for (size_t i = 0; i < samples; ++i) {
    const float t = i / static_cast<float>(RATE);
    const float slot = std::fmod(t, 0.32f);
    const bool on = t < 0.96f && slot < 0.16f;
    pcm[i] = on ? static_cast<int16_t>(12000 * std::sin(2 * M_PI * 880 * t)) : 0;
  }
  wav::writeHeader(wav, RATE, samples * sizeof(int16_t));
  if (!audio.begin()) return false;
  audio.setVolume(volume);
  playing = audio.playBuffer(wav, bytes, true);
  return playing;
}

void AlertBeep::stop() {
  if (playing) {
    audio.stop();
    playing = false;
  }
  audio.end();
  if (wav) {
    heap_caps_free(wav);
    wav = nullptr;
  }
}
