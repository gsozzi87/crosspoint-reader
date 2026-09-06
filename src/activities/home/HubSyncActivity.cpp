#include "HubSyncActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "HUB_SYNC";
constexpr unsigned long DONE_SCREEN_MS = 1200;
constexpr long CLOCK_DRIFT_TOLERANCE_S = 120;
}  // namespace

void HubSyncActivity::onEnter() {
  Activity::onEnter();
  state = CONNECTING;
  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void HubSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();  // lands on the hub, which repaints from the fresh cache
  }
}

void HubSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_ERR(TAG, "WiFi connection failed");
    markAttempt(false);
    state = FAILED;
    doneAt = millis();
    requestUpdate();
    return;
  }
  state = SYNCING;  // the request runs from loop() so the screen paints first
  requestUpdate();
}

void HubSyncActivity::markAttempt(const bool ok) {
  time_t now = 0;
  const bool hasClock = halClock.getEpochUtc(now);
  // Without a clock, 1 still records "tried once" so a failing server does
  // not make every hub entry bring WiFi up again.
  HUB_STORE.lastAttemptAt = hasClock ? now : 1;
  if (ok) HUB_STORE.syncedAt = hasClock ? now : 1;
  HUB_STORE.saveToFile();
}

bool HubSyncActivity::fetchNow(ServerClient::Result* resultOut, int* statusOut) {
  WiFi.setSleep(false);
  ServerClient::Response resp;
  const ServerClient::Result result = SERVER_CLIENT.get(std::string("/api/hub?lang=") + uiLanguageCode(), resp, /*auth=*/true);
  if (resultOut) *resultOut = result;
  if (statusOut) *statusOut = resp.status;
  const int status = resp.status;
  bool ok = false;
  if (result == ServerClient::Result::Ok) {
    JsonDocument doc;
    if (deserializeJson(doc, resp.body) == DeserializationError::Ok && (doc["ok"] | false)) {
      // Server clock first, so syncedAt below is right even on a cold RTC.
      const int64_t serverNow = doc["now"] | (int64_t)0;
      if (serverNow > 0) {
        time_t local = 0;
        const bool have = halClock.getEpochUtc(local);
        if (!have || labs(static_cast<long>(local - serverNow)) > CLOCK_DRIFT_TOLERANCE_S) {
          halClock.setFromEpochUtc(static_cast<time_t>(serverNow));
        }
      }
      HUB_STORE.applyServer(doc.as<JsonVariantConst>());
      ok = true;
    } else {
      LOG_ERR(TAG, "bad /api/hub payload");
    }
  } else {
    LOG_ERR(TAG, "GET /api/hub: %s (%d)", ServerClient::resultName(result), status);
  }
  markAttempt(ok);
  WiFi.setSleep(true);
  LOG_INF(TAG, "sync %s: weather=\"%s\" events=%u messages=%u", ok ? "ok" : "failed", HUB_STORE.weatherLine.c_str(),
          (unsigned)HUB_STORE.events.size(), (unsigned)HUB_STORE.messages.size());
  return ok;
}

namespace {
constexpr const char* TTS_DIR = "/.crosspoint/tts";
constexpr int TTS_REMINDERS = 5;

bool fetchClip(const std::string& text, const std::string& path) {
  if (Storage.exists(path.c_str())) return true;
  ServerClient::Response resp;
  const ServerClient::Result r =
      SERVER_CLIENT.get(std::string("/api/tts?lang=") + uiLanguageCode() + "&text=" + urlEncode(text), resp);
  if (r != ServerClient::Result::Ok || resp.body.size() < 16) return false;
  HalFile f;
  if (!Storage.openFileForWrite("TTS", path, f)) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
  f.close();
  return written == resp.body.size();
}
}  // namespace

void HubSyncActivity::cacheSpokenNotices() {
  Storage.ensureDirectoryExists(TTS_DIR);
  int fetched = 0;
  int count = 0;
  for (const HubStore::Reminder& r : HUB_STORE.reminders) {
    if (count++ >= TTS_REMINDERS) break;
    const std::string path = std::string(TTS_DIR) + "/r" + std::to_string(r.id) + ".bin";
    if (!Storage.exists(path.c_str())) {
      if (fetchClip(std::string(tr(STR_HUB_REMINDERS)) + ": " + r.title, path)) fetched++;
    }
  }
  const std::string timerPath = std::string(TTS_DIR) + "/timer-" + uiLanguageCode() + ".bin";
  if (fetchClip(tr(STR_TIMER_DONE), timerPath)) fetched++;
  LOG_INF(TAG, "spoken notices: %d fetched", fetched);
}

void HubSyncActivity::runSync() {
  const bool ok = fetchNow(&result, &status);
  if (ok) {
    WiFi.setSleep(false);
    cacheSpokenNotices();
    WiFi.setSleep(true);
  }
  flushed = 0;
  if (ok && SERVER_CLIENT.queueSize() > 0) {
    WiFi.setSleep(false);
    flushed = SERVER_CLIENT.flushQueue();
    WiFi.setSleep(true);
  }
  state = ok ? DONE : FAILED;
  doneAt = millis();
  requestUpdate();
}

void HubSyncActivity::loop() {
  switch (state) {
    case SYNCING:
      runSync();
      break;
    case DONE:
      // Brief confirmation, then straight back (the restart repaints the hub).
      if (millis() - doneAt >= DONE_SCREEN_MS) finish();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        finish();
      }
      break;
    case CONNECTING:
      break;
  }
}

void HubSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_SYNC));
  switch (state) {
    case CONNECTING:
      break;
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_HUB_SYNCING), true, EpdFontFamily::BOLD);
      break;
    case DONE:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_HUB_SYNC_DONE), true, EpdFontFamily::BOLD);
      break;
    case FAILED: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_HUB_SYNC_FAILED), true, EpdFontFamily::BOLD);
      char detail[64];
      snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(result), status);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, detail);
      break;
    }
  }
  const auto labels = mappedInput.mapLabels(state == FAILED ? tr(STR_BACK) : "", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
