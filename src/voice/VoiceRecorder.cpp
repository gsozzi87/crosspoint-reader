#include "VoiceRecorder.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cmath>

#include "util/WavHeader.h"

namespace {
constexpr const char* TAG = "VOICE";
constexpr size_t BLOCK = 512;  // 32 ms at 16 kHz
}  // namespace

bool VoiceRecorder::start(StrId& why) {
  if (!BoardConfig::hasCodecMic()) {
    why = StrId::STR_ASK_NO_MIC;
    return false;
  }
  release();
  const size_t bytes = wav::HEADER_BYTES + maxSamples * sizeof(int16_t);
  buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!buffer) {
    why = StrId::STR_AUDIO_NO_MEMORY;
    return false;
  }
  recorded = 0;
  if (!audio.begin()) {
    why = StrId::STR_AUDIO_CAPTURE_FAILED;
    return false;
  }
  blip(1200, 90);
  if (!audio.beginCapture(SAMPLE_RATE)) {
    audio.end();
    why = StrId::STR_AUDIO_CAPTURE_FAILED;
    return false;
  }
  recording = true;
  LOG_DBG(TAG, "Recording (max %u s)", (unsigned)(maxSamples / SAMPLE_RATE));
  return true;
}

bool VoiceRecorder::pump() {
  if (!recording) return true;
  int16_t* samples = reinterpret_cast<int16_t*>(buffer + wav::HEADER_BYTES);
  size_t want = maxSamples - recorded;
  if (want > BLOCK) want = BLOCK;
  const int n = audio.readCapture(samples + recorded, want, 50);
  if (n < 0) {
    abort();
    return false;
  }
  recorded += n;
  if (recorded >= maxSamples) stop();
  return true;
}

void VoiceRecorder::stop() {
  if (!recording) return;
  recording = false;
  audio.endCapture();
  blip(700, 110);
  audio.end();  // release I2S + codec before WiFi/TLS need the heap
  wav::writeHeader(buffer, SAMPLE_RATE, recorded * sizeof(int16_t));
  LOG_DBG(TAG, "Take: %u samples (%.1f s)", (unsigned)recorded, seconds());
}

void VoiceRecorder::abort() {
  if (recording) {
    recording = false;
    audio.endCapture();
    audio.end();
  }
  release();
}

void VoiceRecorder::release() {
  if (buffer) {
    heap_caps_free(buffer);
    buffer = nullptr;
  }
  recorded = 0;
}

size_t VoiceRecorder::wavBytes() const { return buffer ? wav::HEADER_BYTES + recorded * sizeof(int16_t) : 0; }

// One sine tone, played to completion (short, so a blocking wait is fine).
void VoiceRecorder::blip(const uint16_t hz, const uint16_t ms) {
  if (!blips) return;
  const size_t samples = SAMPLE_RATE * ms / 1000;
  const size_t bytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  uint8_t* tone = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!tone) return;
  int16_t* pcm = reinterpret_cast<int16_t*>(tone + wav::HEADER_BYTES);
  for (size_t i = 0; i < samples; ++i) {
    // Fade in/out over 5 ms so the tone does not click.
    const size_t edge = SAMPLE_RATE / 200;
    float env = 1.0f;
    if (i < edge) env = i / static_cast<float>(edge);
    else if (samples - i < edge) env = (samples - i) / static_cast<float>(edge);
    pcm[i] = static_cast<int16_t>(9000 * env * std::sin(2 * M_PI * hz * i / SAMPLE_RATE));
  }
  wav::writeHeader(tone, SAMPLE_RATE, samples * sizeof(int16_t));
  audio.setVolume(80);
  if (audio.playBuffer(tone, bytes, false)) {
    const unsigned long until = millis() + ms + 150;
    while (audio.isPlaying() && millis() < until) delay(5);
    audio.stop();
  }
  heap_caps_free(tone);
}
