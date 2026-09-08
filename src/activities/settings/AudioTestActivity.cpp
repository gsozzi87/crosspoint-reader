#include "AudioTestActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WavHeader.h"

namespace {
constexpr const char* TAG = "AUDIO_TEST";

// Franja del pico a la que hay que apuntar hablándole al aparato de cerca: por
// debajo se desperdicia escala (y el dictado llega flojo), por arriba empieza a
// recortar, que para pasar voz a texto es peor todavía.
constexpr float TARGET_LOW_PCT = 50.0f;
constexpr float TARGET_HIGH_PCT = 70.0f;

}  // namespace

// --------------------------------------------------------- ganancia en la SD --

namespace micgain {

void loadAndApply() {
  HalFile file;
  if (!Storage.openFileForRead("MIC", FILE_PATH, file)) return;
  char buf[8] = {0};
  const int n = file.read(buf, sizeof(buf) - 1);
  file.close();
  if (n <= 0) return;
  buf[n] = '\0';
  const long value = strtol(buf, nullptr, 10);
  if (value < 0 || value > 100) return;
  AudioManager::setMicGain(static_cast<uint8_t>(value));
  LOG_INF("MIC", "Ganancia del micrófono: %ld %% (+%d dB)", value, AudioManager::micGainDb());
}

void save(const uint8_t percent) {
  HalFile file;
  if (!Storage.openFileForWrite("MIC", FILE_PATH, file)) {
    LOG_ERR("MIC", "No se pudo guardar la ganancia del micrófono");
    return;
  }
  char buf[8];
  const int n = snprintf(buf, sizeof(buf), "%u\n", static_cast<unsigned>(percent));
  file.write(buf, static_cast<size_t>(n));
  file.close();
}

}  // namespace micgain

// ------------------------------------------------------------------ actividad --

void AudioTestActivity::onEnter() {
  Activity::onEnter();
  state = IDLE;
  recorded = 0;
  peak = 0;
  clipped = 0;
  haveTake = false;
  gainDirty = false;
  if (!BoardConfig::hasAudio() || !audio.begin()) {
    fail(StrId::STR_AUDIO_INIT_FAILED);
    return;
  }
  requestUpdate();
}

void AudioTestActivity::onExit() {
  // Una escritura sola al salir, no una por cada toque de la palanca.
  if (gainDirty) micgain::save(AudioManager::micGain());
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

// Arriba sube y Abajo baja, de a GAIN_STEP. Se aplica en el acto (el códec la
// toma en la próxima grabación) y se guarda recién al salir.
void AudioTestActivity::nudgeGain(const int delta) {
  const int current = static_cast<int>(AudioManager::micGain());
  int next = current + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == current) return;
  AudioManager::setMicGain(static_cast<uint8_t>(next));
  gainDirty = true;
  LOG_DBG(TAG, "Ganancia del micrófono: %d %% (+%d dB)", next, AudioManager::micGainDb());
  requestUpdate();
}

void AudioTestActivity::startTake() {
  if (!allocate()) {
    fail(StrId::STR_AUDIO_NO_MEMORY);
    return;
  }
  recorded = 0;
  peak = 0;
  clipped = 0;
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
  LOG_DBG(TAG, "Recording %u s at %u Hz, gain %u %%", (unsigned)SECONDS, (unsigned)SAMPLE_RATE,
          (unsigned)AudioManager::micGain());
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
    if (a >= CLIP_LEVEL) ++clipped;
  }
  recorded += n;
  if (recorded >= SAMPLES) {
    audio.endCapture();
    finishTake();
  }
}

void AudioTestActivity::finishTake() {
  writeWavHeader();
  haveTake = true;
  LOG_DBG(TAG, "Take done: %u samples, peak %d, clipped %u", (unsigned)SAMPLES, (int)peak, (unsigned)clipped);
  if (!audio.playBuffer(wav, WAV_HEADER + SAMPLES * sizeof(int16_t), false)) {
    fail(StrId::STR_AUDIO_INIT_FAILED);
    return;
  }
  state = PLAYING;
  requestUpdate();
}

void AudioTestActivity::writeWavHeader() { wav::writeHeader(wav, SAMPLE_RATE, SAMPLES * sizeof(int16_t)); }

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
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        startTake();
        return;
      }
      // La palanca calibra la ganancia sin salir de la pantalla.
      if (audio.captureAvailable()) {
        if (mappedInput.wasPressed(MappedInputManager::Button::Up)) nudgeGain(GAIN_STEP);
        if (mappedInput.wasPressed(MappedInputManager::Button::Down)) nudgeGain(-GAIN_STEP);
      }
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

// Ganancia actual: número grande, decibeles al lado y una barra, para que se
// vea de un saque para dónde está el ajuste.
void AudioTestActivity::drawGainRow(const int y, const int width) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  char buf[64];

  renderer.drawText(SMALL_FONT_ID, x, y, tr(STR_AUDIO_MIC_GAIN));
  snprintf(buf, sizeof(buf), "%u %%   (+%d dB)", (unsigned)AudioManager::micGain(), AudioManager::micGainDb());
  renderer.drawText(UI_12_FONT_ID, x, y + 24, buf, true, EpdFontFamily::BOLD);

  const int barY = y + 56;
  constexpr int barH = 14;
  renderer.drawRect(x, barY, width, barH, 2, true);
  const int filled = width * AudioManager::micGain() / 100;
  if (filled > 4) renderer.fillRect(x + 2, barY + 2, filled - 4, barH - 4, true);
}

// Pico de la última toma con su barra y la franja a la que hay que apuntar
// marcada con dos rayitas, más el veredicto en una línea.
void AudioTestActivity::drawLevelRow(const int y, const int width) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  char buf[80];

  const float pct = 100.0f * peak / 32767.0f;
  const float dbfs = peak > 0 ? 20.0f * log10f(peak / 32767.0f) : -100.0f;
  snprintf(buf, sizeof(buf), "%s: %.1f %% (%.0f dBFS)", tr(STR_AUDIO_PEAK_LEVEL), pct, dbfs);
  renderer.drawText(UI_10_FONT_ID, x, y, buf, true, EpdFontFamily::BOLD);

  const int barY = y + 30;
  constexpr int barH = 20;
  renderer.drawRect(x, barY, width, barH, 2, true);
  int filled = static_cast<int>(width * pct / 100.0f);
  if (filled > width - 4) filled = width - 4;
  if (filled > 4) renderer.fillRect(x + 2, barY + 2, filled - 4, barH - 4, true);
  // Las dos rayitas de la franja buena, un poco por debajo de la barra.
  const float marks[2] = {TARGET_LOW_PCT, TARGET_HIGH_PCT};
  for (const float mark : marks) {
    const int mx = x + static_cast<int>(width * mark / 100.0f);
    renderer.fillRect(mx - 1, barY + barH + 2, 3, 8, true);
  }

  StrId verdict;
  if (clipped > SAMPLES / 500) {
    verdict = StrId::STR_AUDIO_LEVEL_CLIP;  // más del 0,2 % de la toma contra el tope
  } else if (pct < TARGET_LOW_PCT) {
    verdict = peak < 100 ? StrId::STR_AUDIO_SILENT : StrId::STR_AUDIO_LEVEL_LOW;
  } else if (pct > TARGET_HIGH_PCT) {
    verdict = StrId::STR_AUDIO_LEVEL_CLIP;
  } else {
    verdict = StrId::STR_AUDIO_LEVEL_OK;
  }
  renderer.drawText(UI_10_FONT_ID, x, barY + barH + 22, I18N.get(verdict), true, EpdFontFamily::BOLD);
}

void AudioTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int x = metrics.contentSidePadding;
  const int width = pageWidth - 2 * metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AUDIO_TEST));

  const char* confirmLabel = "";
  const char* upLabel = "";
  const char* downLabel = "";
  switch (state) {
    case IDLE:
    case DONE: {
      confirmLabel = tr(STR_SELECT);
      int y = top;
      if (state == DONE && usedMic) {
        drawLevelRow(y, width);
        y += 120;
      } else if (state == DONE) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_AUDIO_TONE_PLAYED));
        y += 60;
      } else {
        renderer.drawText(SMALL_FONT_ID, x, y, tr(STR_AUDIO_TEST_HINT));
        y += 46;
      }

      if (audio.captureAvailable()) {
        drawGainRow(y, width);
        y += 96;
        renderer.drawText(SMALL_FONT_ID, x, y, tr(STR_AUDIO_GAIN_TARGET));
        y += 34;
        upLabel = "+";
        downLabel = "-";
      }
      renderer.drawText(SMALL_FONT_ID, x, y + 10,
                        I18N.get(haveTake ? StrId::STR_AUDIO_AGAIN : StrId::STR_AUDIO_TEST_PRESS), true,
                        EpdFontFamily::BOLD);
      break;
    }
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_AUDIO_RECORDING), true, EpdFontFamily::BOLD);
      break;
    case PLAYING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_AUDIO_PLAYING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, upLabel, downLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
