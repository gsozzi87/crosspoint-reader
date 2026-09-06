#include "HubLocationActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "HubSyncActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "HUB_LOC";
constexpr int MAX_CANDIDATES = 5;
}  // namespace

void HubLocationActivity::onEnter() {
  Activity::onEnter();
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  startRecording();
}

void HubLocationActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  if (wifiActivated && !handoff) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void HubLocationActivity::leave() { finish(); }

void HubLocationActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void HubLocationActivity::startRecording() {
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  state = RECORDING;
  requestUpdate();
}

void HubLocationActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    startRecording();  // accidental press: listen again
    return;
  }
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void HubLocationActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = TRANSCRIBING;
  requestPending = true;
  requestUpdate();
}

void HubLocationActivity::performTranscribe() {
  requestPending = false;
  std::string detail;
  const bool ok = SpeechToText::transcribe(recorder, spoken, detail);
  recorder.release();
  if (!ok) {
    fail(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  state = SEARCHING;
  requestPending = true;
  requestUpdate();
}

// GET /api/hub/location/search?q=<spoken> -> {ok, results:[{name, label, lat, lon, timezone}]}
void HubLocationActivity::performSearch() {
  requestPending = false;
  ServerClient::Response resp;
  const ServerClient::Result r =
      SERVER_CLIENT.get("/api/hub/location/search?q=" + urlEncode(spoken) + "&lang=" + uiLanguageCode(), resp);
  WiFi.setSleep(true);
  if (r != ServerClient::Result::Ok) {
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_HUB_LOCATION_NONE, detail);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    fail(StrId::STR_HUB_LOCATION_NONE, "bad json");
    return;
  }
  candidates.clear();
  candidateLabels.clear();
  for (JsonVariantConst v : doc["results"].as<JsonArrayConst>()) {
    if (candidates.size() >= MAX_CANDIDATES) break;
    Candidate c;
    c.label = v["label"] | "";
    c.name = v["name"] | "";
    c.lat = v["lat"] | 0.0;
    c.lon = v["lon"] | 0.0;
    c.timezone = v["timezone"] | "";
    if (c.label.empty() || c.name.empty()) continue;
    candidateLabels.push_back(c.label);
    candidates.push_back(std::move(c));
  }
  if (candidates.empty()) {
    fail(StrId::STR_HUB_LOCATION_NONE, spoken);
    return;
  }
  state = PICK;
  picker.show(StrId::STR_HUB_LOCATION_PICK, candidateLabels, 0, [this](int idx) {
    chosen = idx;
    state = SAVING;
    requestPending = true;
    requestUpdate();
  });
  requestUpdate();
}

// PUT /api/hub/location {name, label, lat, lon, timezone}; then the regular
// sync fetches the weather for the new place and restarts into the hub.
void HubLocationActivity::performSave() {
  requestPending = false;
  if (chosen < 0 || chosen >= static_cast<int>(candidates.size())) {
    leave();
    return;
  }
  const Candidate& c = candidates[chosen];
  std::string body;
  {
    JsonDocument doc;
    doc["name"] = c.name;
    doc["label"] = c.label;
    doc["lat"] = c.lat;
    doc["lon"] = c.lon;
    doc["timezone"] = c.timezone;
    serializeJson(doc, body);
  }
  WiFi.setSleep(false);
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/hub/location", body, resp);
  WiFi.setSleep(true);
  if (r != ServerClient::Result::Ok) {
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_HUB_SYNC_FAILED, detail);
    return;
  }
  LOG_INF(TAG, "Weather place: %s (%.3f, %.3f)", c.label.c_str(), c.lat, c.lon);
  handoff = true;
  activityManager.replaceActivity(std::make_unique<HubSyncActivity>(renderer, mappedInput));
}

void HubLocationActivity::loop() {
  switch (state) {
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        leave();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        stopRecording();
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case TRANSCRIBING:
      if (requestPending) performTranscribe();
      break;
    case SEARCHING:
      if (requestPending) performSearch();
      break;
    case PICK:
      if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
        if (state == PICK && !picker.isActive()) leave();  // Back on the list
      }
      break;
    case SAVING:
      if (requestPending) performSave();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        leave();
      }
      break;
    case CONNECTING:
      break;
  }
}

void HubLocationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_LOCATION));
  const char* confirmLabel = "";
  switch (state) {
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_HUB_LOCATION_PROMPT), true, EpdFontFamily::BOLD);
      confirmLabel = tr(STR_SELECT);
      break;
    case TRANSCRIBING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_ASK_TRANSCRIBING), true, EpdFontFamily::BOLD);
      break;
    case SEARCHING:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, renderer.truncatedText(UI_10_FONT_ID, spoken.c_str(), pageWidth - 40).c_str());
      renderer.drawCenteredText(UI_12_FONT_ID, mid, tr(STR_HUB_LOCATION_SEARCHING), true, EpdFontFamily::BOLD);
      break;
    case PICK:
      renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 20,
                                renderer.truncatedText(UI_10_FONT_ID, spoken.c_str(), pageWidth - 40).c_str());
      if (picker.processRender(renderer, mappedInput)) return;
      break;
    case SAVING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_HUB_LOCATION_SAVING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
