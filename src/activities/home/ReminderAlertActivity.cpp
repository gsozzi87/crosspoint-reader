#include "ReminderAlertActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include "HubStore.h"
#include "input/MotionInput.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "fontIds.h"
#include "voice/SpeechCache.h"

namespace {
constexpr const char* TAG = "REMIND";

}  // namespace

void ReminderAlertActivity::onEnter() {
  Activity::onEnter();
  startedAt = millis();
  // "Reminder: <title>" from the SD (cached at sync), then the beeps.
  const std::string clip = speechcache::clipPath(std::string(tr(STR_HUB_REMINDERS)) + ": " + title);
  spoken = !speech.playFile(clip.c_str());
  if (spoken) {
    speech.stop();  // el I2S es uno solo: soltarlo antes de que lo abra el pitido
    beep.start();
  }
  requestUpdate();
}

void ReminderAlertActivity::onExit() {
  Activity::onExit();
  speech.stop();
  beep.stop();
}

void ReminderAlertActivity::done() {
  // Si repite, la cache se queda con la proxima ocurrencia: sin WiFi, borrarlo
  // dejaba al aparato sin nada que armar y el diario no volvia a sonar nunca.
  time_t now = 0;
  halClock.getEpochUtc(now);
  HUB_STORE.completeReminder(reminderId, now);
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
  speech.stop();
  beep.stop();
  if (resultHandler) {
    finish();
  } else {
    activityManager.goHome();  // launched from the boot path after a timer wake
  }
}

void ReminderAlertActivity::loop() {
  if (!spoken && !speech.isPlaying()) {
    spoken = true;
    speech.stop();
    beep.start();
  }
  if (beep.isPlaying() && millis() - startedAt > BEEP_MS) beep.stop();
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    done();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    snooze();
    return;
  }
  // Darlo vuelta es posponer sin buscar ningún botón: es el gesto de tapar el
  // despertador. Sacudirlo también lo pospone (es "pará"), que es lo primero
  // que hace cualquiera con un aparato que suena en la mano.
  if (MOTION.take(MotionInput::Event::FaceDown) || MOTION.take(MotionInput::Event::Shake)) {
    LOG_INF(TAG, "gesto: se pospone el recordatorio");
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
  BaseTheme::drawIconBitmap(renderer, icon_hub_reminder_24, pageWidth / 2 - 12, pageHeight / 2 - 110);
  // Title wrapped by the paged-text helper is overkill: two truncated lines.
  const std::string line1 = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 60, line1.c_str(), true, EpdFontFamily::BOLD);
  if (!when.empty()) renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, when.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_REMINDER_SNOOZE), tr(STR_AGENDA_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
