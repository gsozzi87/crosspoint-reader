#include "VoiceActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "HubSyncActivity.h"
#include "TimerActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "VOICE_ACT";
constexpr uint32_t VOICE_TIMEOUT_MS = 90000;  // Whisper + Claude on one request
}  // namespace

void VoiceActivity::onEnter() {
  Activity::onEnter();
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  startRecording();
}

void VoiceActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  speech.stop();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    if (timerSeconds > 0) return;  // TimerActivity takes over; a restart would kill it
    silentRestart();
  }
}

void VoiceActivity::leave() { activityManager.goHome(); }

void VoiceActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void VoiceActivity::startRecording() {
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  state = RECORDING;
  requestUpdate();
}

void VoiceActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    leave();  // accidental press
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

void VoiceActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = SENDING;  // the request runs from loop() so the screen paints first
  requestPending = true;
  requestUpdate();
}

// POST /api/voice (audio/wav) -> {ok, text, intent, reply, saved[]}
void VoiceActivity::performRequest() {
  requestPending = false;
  WiFi.setSleep(false);
  LOG_DBG(TAG, "POST /api/voice: %u bytes", (unsigned)recorder.wavBytes());
  ServerClient::Response resp;
  const ServerClient::Result r =
      SERVER_CLIENT.postBytes(std::string("/api/voice?lang=") + uiLanguageCode(), "audio/wav", recorder.wav(),
                              recorder.wavBytes(), resp, VOICE_TIMEOUT_MS);
  recorder.release();
  if (r != ServerClient::Result::Ok) {
    WiFi.setSleep(true);
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_ASK_FAILED, detail);
    return;
  }
  // Framed body: [u32 LE json length][json][ADPCM speech, optional].
  const std::string& raw = resp.body;
  size_t jsonLen = 0;
  if (raw.size() >= 4) {
    jsonLen = static_cast<uint8_t>(raw[0]) | (static_cast<uint8_t>(raw[1]) << 8) |
              (static_cast<uint8_t>(raw[2]) << 16) | (static_cast<size_t>(static_cast<uint8_t>(raw[3])) << 24);
  }
  const bool framed = jsonLen > 0 && 4 + jsonLen <= raw.size();
  JsonDocument doc;
  const DeserializationError jsonErr =
      framed ? deserializeJson(doc, raw.data() + 4, jsonLen) : deserializeJson(doc, raw);
  if (jsonErr != DeserializationError::Ok) {
    WiFi.setSleep(true);
    fail(StrId::STR_ASK_FAILED, "bad json");
    return;
  }
  heard = doc["text"] | "";
  intent = doc["intent"] | "";
  reply = doc["reply"] | "";
  timerSeconds = doc["timerSeconds"] | 0;
  const size_t audioBytes = framed ? raw.size() - 4 - jsonLen : 0;
  if (audioBytes > 8) {
    // Start the voice right away, while the widgets refresh and the text paints.
    speech.playAdpcm(reinterpret_cast<const uint8_t*>(raw.data() + 4 + jsonLen), audioBytes);
  }
  if (reply.empty()) {
    WiFi.setSleep(true);
    fail(StrId::STR_ASK_FAILED, doc["error"] | "empty reply");
    return;
  }
  LOG_INF(TAG, "\"%s\" -> %s", heard.c_str(), intent.c_str());

  // Widgets: whatever was just saved shows up on the hub right away.
  if (intent != "question" && intent != "translate") HubSyncActivity::fetchNow();
  WiFi.setSleep(true);
  if ((intent == "timer" || intent == "alarm") && timerSeconds > 0) {
    LOG_INF(TAG, "timer %d s", timerSeconds);
    activityManager.replaceActivity(std::make_unique<TimerActivity>(renderer, mappedInput, timerSeconds));
    return;
  }
  timerSeconds = 0;
  showReply();
}

const char* VoiceActivity::intentTitle() const {
  if (intent == "reminder") return tr(STR_VOICE_SAVED_REMINDER);
  if (intent == "task") return tr(STR_VOICE_SAVED_TASK);
  if (intent == "shopping") return tr(STR_VOICE_SAVED_SHOPPING);
  if (intent == "note") return tr(STR_VOICE_SAVED_NOTE);
  if (intent == "message") return tr(STR_VOICE_SAVED_MESSAGE);
  if (intent == "translate") return tr(STR_VOICE_TRANSLATION);
  return heard.c_str();  // question: what was asked, as the header
}

void VoiceActivity::showReply() {
  state = REPLY;
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, intentTitle(), reply),
                         [this](const ActivityResult&) { leave(); });
}

void VoiceActivity::loop() {
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
    case SENDING:
      if (requestPending) performRequest();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        leave();
      }
      break;
    case CONNECTING:
    case REPLY:
      break;
  }
}

void VoiceActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_TALK));
  const char* confirmLabel = "";
  switch (state) {
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_VOICE_PROMPT), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, tr(STR_VOICE_HINT));
      confirmLabel = tr(STR_SELECT);
      break;
    case SENDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_VOICE_THINKING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 10,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
    case REPLY:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
