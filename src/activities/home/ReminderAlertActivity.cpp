#include "ReminderAlertActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <esp_heap_caps.h>

#include <cmath>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "fontIds.h"
#include "util/WavHeader.h"

namespace {
constexpr const char* TAG = "REMIND";
constexpr uint32_t RATE = 16000;

void drawSdkIcon(const GfxRenderer& renderer, const freeink::Icon& icon, int x, int y) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) == 0) renderer.drawPixel(x + col, y + row, true);
    }
  }
}
}  // namespace

void ReminderAlertActivity::onEnter() {
  Activity::onEnter();
  startedAt = millis();
  startBeep();
  requestUpdate();
}

void ReminderAlertActivity::onExit() {
  Activity::onExit();
  stopBeep();
}

// Three 880 Hz beeps then a pause, as one looping WAV in PSRAM: ~1.6 s of
// 16 kHz mono 16-bit = 51 KB.
void ReminderAlertActivity::startBeep() {
  if (!BoardConfig::hasAudio()) return;
  const size_t samples = RATE * 16 / 10;
  beepBytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  beep = static_cast<uint8_t*>(heap_caps_malloc(beepBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!beep) return;
  int16_t* pcm = reinterpret_cast<int16_t*>(beep + wav::HEADER_BYTES);
  for (size_t i = 0; i < samples; ++i) {
    const float t = i / static_cast<float>(RATE);
    const float slot = std::fmod(t, 0.32f);  // 0.16 s on, 0.16 s off
    const bool on = t < 0.96f && slot < 0.16f;
    pcm[i] = on ? static_cast<int16_t>(12000 * std::sin(2 * M_PI * 880 * t)) : 0;
  }
  wav::writeHeader(beep, RATE, samples * sizeof(int16_t));
  if (!audio.begin()) return;
  audio.setVolume(85);
  beeping = audio.playBuffer(beep, beepBytes, true);
}

void ReminderAlertActivity::stopBeep() {
  if (beeping) {
    audio.stop();
    beeping = false;
  }
  audio.end();
  if (beep) {
    heap_caps_free(beep);
    beep = nullptr;
  }
}

void ReminderAlertActivity::done() {
  HUB_STORE.removeReminder(reminderId);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = reminderId;
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "done %d: %s", reminderId, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/done", body)));
  leave();
}

void ReminderAlertActivity::snooze() {
  time_t now = 0;
  halClock.getEpochUtc(now);
  HUB_STORE.snoozeReminder(reminderId, now + SNOOZE_S);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = reminderId;
    doc["snooze"] = static_cast<int>(SNOOZE_S);
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "snooze %d: %s", reminderId, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/done", body)));
  leave();
}

void ReminderAlertActivity::leave() {
  stopBeep();
  if (resultHandler) {
    finish();
  } else {
    activityManager.goHome();  // launched from the boot path after a timer wake
  }
}

void ReminderAlertActivity::loop() {
  if (beeping && millis() - startedAt > BEEP_MS) {
    audio.stop();
    beeping = false;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    done();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    snooze();
    return;
  }
}

void ReminderAlertActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_REMINDERS));
  drawSdkIcon(renderer, icon_hub_reminder_24, pageWidth / 2 - 12, pageHeight / 2 - 110);
  // Title wrapped by the paged-text helper is overkill: two truncated lines.
  const std::string line1 = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 60, line1.c_str(), true, EpdFontFamily::BOLD);
  if (!when.empty()) renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, when.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_REMINDER_SNOOZE), tr(STR_AGENDA_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
