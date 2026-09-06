#include "SpeechOut.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "Adpcm.h"

namespace {
constexpr const char* TAG = "SPEECH";
constexpr size_t MAX_FILE = 256 * 1024;
}  // namespace

bool SpeechOut::playAdpcm(const uint8_t* data, const size_t len, const uint8_t volume) {
  stop();
  size_t bytes = 0;
  wav = adpcm::decodeToWav(data, len, bytes);
  if (!wav) {
    LOG_ERR(TAG, "bad clip (%u bytes)", (unsigned)len);
    return false;
  }
  if (!audio.begin()) {
    stop();
    return false;
  }
  audio.setVolume(volume);
  started = audio.playBuffer(wav, bytes, false);
  LOG_DBG(TAG, "playing %u samples", (unsigned)adpcm::sampleCount(data, len));
  return started;
}

bool SpeechOut::playFile(const char* path, const uint8_t volume) {
  HalFile file;
  if (!Storage.openFileForRead("SPEECH", path, file)) return false;
  const size_t size = file.size();
  if (size < adpcm::HEADER_BYTES || size > MAX_FILE) {
    file.close();
    return false;
  }
  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    file.close();
    return false;
  }
  const int got = file.read(raw, size);
  file.close();
  const bool ok = got == static_cast<int>(size) && playAdpcm(raw, size, volume);
  heap_caps_free(raw);
  return ok;
}

void SpeechOut::stop() {
  if (started) {
    audio.stop();
    started = false;
  }
  audio.end();
  if (wav) {
    heap_caps_free(wav);
    wav = nullptr;
  }
}
