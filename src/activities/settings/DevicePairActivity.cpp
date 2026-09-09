#include "DevicePairActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/SevenSegment.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "PAIR";
constexpr unsigned long POLL_MS = 3000;
constexpr unsigned long CODE_LIFE_MS = 10UL * 60UL * 1000UL;  // lo que dura del lado del servidor
constexpr int PARTIALS_BEFORE_CLEAN = 10;
}  // namespace

void DevicePairActivity::onEnter() {
  Activity::onEnter();
  if (SERVER_STORE.getBaseUrl().empty()) {
    fail(StrId::STR_NO_SERVER_URL);
    return;
  }
  // El token se genera solo la primera vez; de acá en más el aparato ya tiene
  // identidad aunque nadie haya tocado nada.
  SERVER_STORE.ensureToken();
  wifiWasUp = WiFi.status() == WL_CONNECTED;
  beginConnect();
}

void DevicePairActivity::onExit() {
  Activity::onExit();
  if (!wifiWasUp && WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void DevicePairActivity::fail(const StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failure = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void DevicePairActivity::beginConnect() {
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) {
    pumpConnect();
    return;
  }
  requestUpdate();
}

void DevicePairActivity::pumpConnect() {
  if (wifiPicker) return;
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase phase = wifi.pump();
  if (phase == FriendlyWifi::Phase::Connected) {
    onWifiReady(true);
    return;
  }
  if (phase == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             onWifiReady(!result.isCancelled);
                           });
    return;
  }
  if (wifi.revision() != rev) requestUpdate();
}

void DevicePairActivity::onWifiReady(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  requestCode();
}

// Pide el código. Va SIN token: el servidor todavía no conoce este aparato, y
// mandarle un Bearer desconocido daría 401 antes de llegar al handler.
void DevicePairActivity::requestCode() {
  std::string body;
  {
    JsonDocument doc;
    doc["deviceId"] = ServerCredentialStore::deviceId();
    doc["token"] = SERVER_STORE.getToken();
    serializeJson(doc, body);
  }
  ServerClient::Response res;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/pair/start", body, res, 20000, /*auth=*/false);
  if (r != ServerClient::Result::Ok) {
    fail(StrId::STR_PAIR_FAILED, ServerClient::resultName(r));
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, res.body) != DeserializationError::Ok || !(doc["ok"] | false)) {
    fail(StrId::STR_PAIR_FAILED, doc["error"] | "");
    return;
  }
  code = doc["code"] | "";
  if (code.empty()) {
    fail(StrId::STR_PAIR_FAILED);
    return;
  }
  LOG_INF(TAG, "codigo listo para %s", ServerCredentialStore::deviceId().c_str());
  codeStartedAt = millis();
  lastPoll = millis();
  state = WAITING;
  requestUpdate();
}

// Pregunta cada tres segundos si del otro lado ya escribieron el código.
void DevicePairActivity::poll() {
  ServerClient::Response res;
  if (SERVER_CLIENT.get("/api/pair/status", res) != ServerClient::Result::Ok) return;
  JsonDocument doc;
  if (deserializeJson(doc, res.body) != DeserializationError::Ok) return;
  if (!(doc["paired"] | false)) return;
  account = doc["account"] | "";
  state = PAIRED;
  requestUpdate();
}

void DevicePairActivity::loop() {
  if (state == CONNECTING) {
    pumpConnect();
    return;
  }
  if (state == WAITING) {
    if (millis() - codeStartedAt >= CODE_LIFE_MS) {
      requestCode();  // se venció: uno nuevo, sin hacer nada raro
      return;
    }
    if (millis() - lastPoll >= POLL_MS) {
      lastPoll = millis();
      poll();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (state != WAITING && state != CONNECTING && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    finish();
  }
}

void DevicePairActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;
  const int side = 24;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PAIR_TITLE));

  switch (state) {
    case CONNECTING:
      FriendlyWifi::drawStatus(renderer, wifi, mid);
      break;
    case WAITING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 150, tr(STR_PAIR_STEP1), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 120, tr(STR_PAIR_STEP2));

      // El código, en dígitos de segmentos: se lee de lejos y se copia sin
      // confundir un cero con una o.
      const int dw = 46;
      const int dh = 78;
      const int gap = 10;
      const int total = 6 * dw + 5 * gap;
      int x = (pageWidth - total) / 2;
      const int y = mid - 60;
      renderer.drawRoundedRect(x - 20, y - 18, total + 40, dh + 36, 2, 10, true);
      for (char c : code) {
        if (c < '0' || c > '9') continue;
        sevenseg::digit(renderer, c - '0', x, y, dw, dh, 9);
        x += dw + gap;
      }

      renderer.drawCenteredText(UI_10_FONT_ID, mid + 70, tr(STR_PAIR_WAITING), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 100,
                                renderer.truncatedText(SMALL_FONT_ID, tr(STR_PAIR_EXPIRES), pageWidth - 2 * side).c_str());
      // El identificador del aparato, por si hay más de uno a la vista.
      const std::string id = std::string(tr(STR_PAIR_DEVICE_ID)) + " " + ServerCredentialStore::deviceId();
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 130,
                                renderer.truncatedText(SMALL_FONT_ID, id.c_str(), pageWidth - 2 * side).c_str());
      break;
    }
    case PAIRED:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, tr(STR_PAIR_DONE), true, EpdFontFamily::BOLD);
      if (!account.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid,
                                  renderer.truncatedText(UI_10_FONT_ID, account.c_str(), pageWidth - 2 * side).c_str());
      }
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 40, tr(STR_PAIR_DONE_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, I18N.get(failure), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(SMALL_FONT_ID, mid + 6,
                                  renderer.truncatedText(SMALL_FONT_ID, failureDetail.c_str(), pageWidth - 2 * side).c_str());
      }
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == WAITING || state == CONNECTING ? "" : tr(STR_DONE),
                                            "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool clean = ++partials >= PARTIALS_BEFORE_CLEAN || state != WAITING;
  if (clean) partials = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
