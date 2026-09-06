#include "TranslatorActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "TRANSL";
constexpr uint32_t TRANSLATE_TIMEOUT_MS = 60000;
struct LangInfo {
  const char* code;
  const char* name;
};
const LangInfo LANGS[] = {{"es", "Español"}, {"en", "English"}, {"fr", "Français"},
                          {"de", "Deutsch"}, {"pt", "Português"}, {"ru", "Русский"}};

// Word-wraps into at most maxLines lines for the two text panes.
void drawWrapped(const GfxRenderer& renderer, int font, int x, int y, int w, int lineH, int maxLines,
                 const std::string& text, bool bold = false) {
  std::string line;
  std::string word;
  int lines = 0;
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  auto flush = [&](bool last) {
    if (lines >= maxLines) return;
    std::string out = line;
    if (last || lines == maxLines - 1) out = renderer.truncatedText(font, line.c_str(), w, style);
    renderer.drawText(font, x, y + lines * lineH, out.c_str(), true, style);
    lines++;
    line.clear();
  };
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    if (c == ' ' || c == '\n') {
      if (word.empty()) continue;
      const std::string candidate = line.empty() ? word : line + " " + word;
      if (renderer.getTextWidth(font, candidate.c_str(), style) > w && !line.empty()) {
        flush(false);
        if (lines >= maxLines) return;
        line = word;
      } else {
        line = candidate;
      }
      word.clear();
      if (c == '\n') flush(false);
    } else {
      word += c;
    }
  }
  if (!line.empty()) flush(true);
}
}  // namespace

const char* TranslatorActivity::languageName(const std::string& code) {
  for (const LangInfo& l : LANGS) {
    if (code == l.code) return l.name;
  }
  return code.c_str();
}

void TranslatorActivity::onEnter() {
  Activity::onEnter();
  mine = uiLanguageCode();
  other = HUB_STORE.translatorLang;
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  if (other.empty() || other == mine) {
    showLanguagePicker();
  } else {
    state = IDLE;
    requestUpdate();
  }
}

void TranslatorActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  speech.stop();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void TranslatorActivity::leave() { activityManager.goHome(); }

void TranslatorActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void TranslatorActivity::showLanguagePicker() {
  state = PICK_LANG;
  pickerOptions.clear();
  pickerCodes.clear();
  for (const LangInfo& l : LANGS) {
    if (mine == l.code) continue;
    pickerOptions.emplace_back(l.name);
    pickerCodes.emplace_back(l.code);
  }
  int initial = 0;
  for (size_t i = 0; i < pickerCodes.size(); ++i) {
    if (pickerCodes[i] == other) initial = static_cast<int>(i);
  }
  picker.show(StrId::STR_TRANSLATOR_PICK, pickerOptions, initial, [this](int idx) {
    if (idx >= 0 && idx < static_cast<int>(pickerCodes.size())) {
      other = pickerCodes[idx];
      HUB_STORE.translatorLang = other;
      HUB_STORE.saveToFile();
    }
    state = IDLE;
    requestUpdate();
  });
  requestUpdate();
}

void TranslatorActivity::startRecording(const bool me) {
  speech.stop();
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  meSpeaking = me;
  state = RECORDING;
  requestUpdate();
}

void TranslatorActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    state = IDLE;
    requestUpdate();
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

void TranslatorActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = SENDING;
  requestPending = true;
  requestUpdate();
}

// POST /api/translate?from&to (audio/wav) -> [u32 json length][json][ADPCM in `to`]
void TranslatorActivity::performRequest() {
  requestPending = false;
  WiFi.setSleep(false);
  const std::string from = meSpeaking ? mine : other;
  const std::string to = meSpeaking ? other : mine;
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postBytes("/api/translate?from=" + from + "&to=" + to, "audio/wav",
                                                         recorder.wav(), recorder.wavBytes(), resp, TRANSLATE_TIMEOUT_MS);
  recorder.release();
  WiFi.setSleep(true);
  if (r != ServerClient::Result::Ok) {
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_ASK_FAILED, detail);
    return;
  }
  const std::string& raw = resp.body;
  size_t jsonLen = 0;
  if (raw.size() >= 4) {
    jsonLen = static_cast<uint8_t>(raw[0]) | (static_cast<uint8_t>(raw[1]) << 8) |
              (static_cast<uint8_t>(raw[2]) << 16) | (static_cast<size_t>(static_cast<uint8_t>(raw[3])) << 24);
  }
  const bool framed = jsonLen > 0 && 4 + jsonLen <= raw.size();
  JsonDocument doc;
  const DeserializationError err = framed ? deserializeJson(doc, raw.data() + 4, jsonLen) : deserializeJson(doc, raw);
  if (err != DeserializationError::Ok) {
    fail(StrId::STR_ASK_FAILED, "bad json");
    return;
  }
  original = doc["text"] | "";
  translation = doc["translation"] | "";
  if (translation.empty()) {
    fail(StrId::STR_ASK_FAILED, doc["error"] | "empty");
    return;
  }
  const size_t audioBytes = framed ? raw.size() - 4 - jsonLen : 0;
  if (audioBytes > 8) speech.playAdpcm(reinterpret_cast<const uint8_t*>(raw.data() + 4 + jsonLen), audioBytes);
  LOG_INF(TAG, "%s->%s \"%s\" -> \"%s\"", from.c_str(), to.c_str(), original.c_str(), translation.c_str());
  state = IDLE;
  requestUpdate();
}

void TranslatorActivity::loop() {
  switch (state) {
    case PICK_LANG:
      if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
        if (state == PICK_LANG && !picker.isActive()) leave();
      }
      break;
    case IDLE:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startRecording(true);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
        startRecording(false);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
        showLanguagePicker();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        leave();
      }
      break;
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        state = IDLE;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
          mappedInput.wasPressed(MappedInputManager::Button::NavPrevious) || !recorder.isRecording()) {
        stopRecording();
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case SENDING:
      if (requestPending) performRequest();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        leave();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = IDLE;
        requestUpdate();
      }
      break;
    case CONNECTING:
      break;
  }
}

void TranslatorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = 20;

  renderer.clearScreen();
  char title[64];
  snprintf(title, sizeof(title), "%s  %s - %s", tr(STR_HUB_TRANSLATOR), mine.c_str(), other.c_str());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  if (state == PICK_LANG) {
    if (picker.processRender(renderer, mappedInput)) return;
    renderer.displayBuffer();
    return;
  }

  const int top = metrics.topPadding + metrics.headerHeight + 16;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int paneH = (hintsTop - top - 60) / 2;
  const int w = pageWidth - 2 * side;

  // Pane 1: what was said (source language), pane 2: the translation (target).
  const std::string srcLang = meSpeaking ? mine : other;
  const std::string dstLang = meSpeaking ? other : mine;
  renderer.drawRoundedRect(side, top, w, paneH, 1, 10, true);
  renderer.drawText(UI_10_FONT_ID, side + 12, top + 8, languageName(srcLang));
  drawWrapped(renderer, UI_12_FONT_ID, side + 12, top + 34, w - 24, 26, (paneH - 44) / 26, original);

  const int top2 = top + paneH + 12;
  renderer.fillRoundedRect(side, top2, w, paneH, 10, Color::Black);
  renderer.drawText(UI_10_FONT_ID, side + 12, top2 + 8, languageName(dstLang), false);
  // White text on black: draw with the same wrap helper but inverted via drawText's black flag.
  {
    std::string line, word;
    int lines = 0;
    const int maxLines = (paneH - 44) / 26;
    const int tw = w - 24;
    auto flushLine = [&](const std::string& l) {
      if (lines >= maxLines) return;
      renderer.drawText(UI_12_FONT_ID, side + 12, top2 + 34 + lines * 26,
                        renderer.truncatedText(UI_12_FONT_ID, l.c_str(), tw, EpdFontFamily::BOLD).c_str(), false,
                        EpdFontFamily::BOLD);
      lines++;
    };
    for (size_t i = 0; i <= translation.size(); ++i) {
      const char c = i < translation.size() ? translation[i] : ' ';
      if (c == ' ' || c == '\n') {
        if (word.empty()) continue;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (renderer.getTextWidth(UI_12_FONT_ID, cand.c_str(), EpdFontFamily::BOLD) > tw && !line.empty()) {
          flushLine(line);
          line = word;
        } else {
          line = cand;
        }
        word.clear();
      } else {
        word += c;
      }
    }
    if (!line.empty()) flushLine(line);
  }

  const int statusY = top2 + paneH + 14;
  char status[96] = "";
  switch (state) {
    case RECORDING:
      snprintf(status, sizeof(status), tr(STR_TRANSLATOR_LISTENING), languageName(srcLang));
      break;
    case SENDING:
      snprintf(status, sizeof(status), "%s", tr(STR_TRANSLATOR_TRANSLATING));
      break;
    case FAILED:
      snprintf(status, sizeof(status), "%s %s", I18N.get(failureId), failureDetail.c_str());
      break;
    default:
      snprintf(status, sizeof(status), "%s", tr(STR_TRANSLATOR_HINT));
      break;
  }
  renderer.drawCenteredText(UI_10_FONT_ID, statusY, renderer.truncatedText(UI_10_FONT_ID, status, w).c_str(), true,
                            state == RECORDING ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == RECORDING ? tr(STR_SELECT) : tr(STR_TRANSLATOR_ME),
                                            tr(STR_TRANSLATOR_OTHER), tr(STR_TRANSLATOR_LANG));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
