#include "AudioTestActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "AUDIO_TEST";

void putLE32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
void putLE16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}
}  // namespace

void AudioTestActivity::onEnter() {
  Activity::onEnter();
  state = IDLE;
  recorded = 0;
  peak = 0;
  if (!BoardConfig::hasAudio() || !audio.begin()) {
    fail(StrId::STR_AUDIO_INIT_FAILED);
    return;
  }
  requestUpdate();
}

void AudioTestActivity::onExit() {
  audio.end();
  if (wav) {
    heap_caps_free(wav);
    wav = nullptr;
  }
  Activity::onExit();
}

bool AudioTestActivity::allocate() {
  if (wav) return true;
  const size_t bytes = WAV_HEADER + SAMPLES * sizeof(int16_t);
  wav = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!wav) wav = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!wav) {
    LOG_ERR(TAG, "Could not allocate %u bytes for the take", (unsigned)bytes);
    return false;
  }
  return true;
}

void AudioTestActivity::fail(StrId why) {
  LOG_ERR(TAG, "Audio test failed: %s", I18N.get(why));
  audio.endCapture();
  audio.stop();
  failureId = why;
  state = FAILED;
  requestUpdate();
}

void AudioTestActivity::startTake() {
  if (!allocate()) {
    fail(StrId::STR_AUDIO_NO_MEMORY);
    return;
  }
  recorded = 0;
  peak = 0;
  usedMic = audio.captureAvailable();

  if (!usedMic) {
    // Speaker-only board: synthesize the take and go straight to playback.
    LOG_DBG(TAG, "No codec mic on this board, playing a test tone");
    fillTestTone();
    finishTake();
    return;
  }

  if (!audio.beginCapture(SAMPLE_RATE)) {
    fail(StrId::STR_AUDIO_CAPTURE_FAILED);
    return;
  }
  LOG_DBG(TAG, "Recording %u s at %u Hz", (unsigned)SECONDS, (unsigned)SAMPLE_RATE);
  state = RECORDING;
  requestUpdate();
}

// Called from loop() while RECORDING: drains the I2S RX DMA in small blocks so
// the UI loop keeps servicing input (Back aborts the take).
void AudioTestActivity::pumpCapture() {
  constexpr size_t BLOCK = 512;  // 32 ms at 16 kHz
  size_t want = SAMPLES - recorded;
  if (want > BLOCK) want = BLOCK;
  const int n = audio.readCapture(samples() + recorded, want, 50);
  if (n < 0) {
    fail(StrId::STR_AUDIO_CAPTURE_FAILED);
    return;
  }
  for (int i = 0; i < n; ++i) {
    const int16_t s = samples()[recorded + i];
    const int16_t a = s < 0 ? (s == INT16_MIN ? INT16_MAX : -s) : s;
    if (a > peak) peak = a;
  }
  recorded += n;
  if (recorded >= SAMPLES) {
    audio.endCapture();
    finishTake();
  }
}

void AudioTestActivity::finishTake() {
  writeWavHeader();
  LOG_DBG(TAG, "Take done: %u samples, peak %d", (unsigned)SAMPLES, (int)peak);
  if (!audio.playBuffer(wav, WAV_HEADER + SAMPLES * sizeof(int16_t), false)) {
    fail(StrId::STR_AUDIO_INIT_FAILED);
    return;
  }
  state = PLAYING;
  requestUpdate();
}

void AudioTestActivity::writeWavHeader() {
  const uint32_t dataBytes = SAMPLES * sizeof(int16_t);
  uint8_t* h = wav;
  memcpy(h, "RIFF", 4);
  putLE32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  putLE32(h + 16, 16);          // fmt chunk size
  putLE16(h + 20, 1);           // PCM
  putLE16(h + 22, 1);           // mono
  putLE32(h + 24, SAMPLE_RATE);
  putLE32(h + 28, SAMPLE_RATE * sizeof(int16_t));  // byte rate
  putLE16(h + 32, sizeof(int16_t));                // block align
  putLE16(h + 34, 16);                             // bits per sample
  memcpy(h + 36, "data", 4);
  putLE32(h + 40, dataBytes);
}

// 440 Hz at -12 dBFS with a short fade at both ends, so the speaker path can
// be checked on boards without a codec mic.
void AudioTestActivity::fillTestTone() {
  int16_t* s = samples();
  constexpr float amplitude = 8192.0f;
  constexpr size_t fade = SAMPLE_RATE / 20;  // 50 ms
  for (size_t i = 0; i < SAMPLES; ++i) {
    float env = 1.0f;
    if (i < fade) env = static_cast<float>(i) / fade;
    if (SAMPLES - i < fade) env = static_cast<float>(SAMPLES - i) / fade;
    s[i] = static_cast<int16_t>(amplitude * env * sinf(2.0f * static_cast<float>(M_PI) * 440.0f * i / SAMPLE_RATE));
  }
  peak = static_cast<int16_t>(amplitude);
}

void AudioTestActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    audio.endCapture();
    audio.stop();
    finish();
    return;
  }

  switch (state) {
    case IDLE:
    case DONE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) startTake();
      break;
    case RECORDING:
      pumpCapture();
      break;
    case PLAYING:
      if (!audio.isPlaying()) {
        state = DONE;
        requestUpdate();
      }
      break;
    case FAILED:
      break;
  }
}

void AudioTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AUDIO_TEST));

  const char* confirmLabel = "";
  switch (state) {
    case IDLE:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, tr(STR_AUDIO_TEST_HINT));
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, tr(STR_AUDIO_TEST_PRESS), true, EpdFontFamily::BOLD);
      confirmLabel = tr(STR_SELECT);
      break;
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_AUDIO_RECORDING), true, EpdFontFamily::BOLD);
      break;
    case PLAYING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_AUDIO_PLAYING), true, EpdFontFamily::BOLD);
      break;
    case DONE: {
      char line[96];
      if (usedMic) {
        // Peak as a percentage of full scale plus dBFS: a mic that returns
        // only noise sits well under 1 %, speech a few tens of percent.
        const float pct = 100.0f * peak / 32767.0f;
        const float dbfs = peak > 0 ? 20.0f * log10f(peak / 32767.0f) : -100.0f;
        snprintf(line, sizeof(line), "%s: %.1f %% (%.0f dBFS)", tr(STR_AUDIO_PEAK_LEVEL), pct, dbfs);
        renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, line);
        if (peak < 100) {
          renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, tr(STR_AUDIO_SILENT), true, EpdFontFamily::BOLD);
        }
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, tr(STR_AUDIO_TONE_PLAYED));
      }
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 30, tr(STR_AUDIO_AGAIN));
      confirmLabel = tr(STR_SELECT);
      break;
    }
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
