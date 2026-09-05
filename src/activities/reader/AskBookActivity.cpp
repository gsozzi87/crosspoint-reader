#include "AskBookActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "ASK";
constexpr size_t MAX_QUESTION_LENGTH = 200;
// Claude with thinking can take a while on a long chapter; the default
// ServerClient timeout is sized for pings.
constexpr uint32_t ASK_TIMEOUT_MS = 90000;
}  // namespace

const StrId AskBookActivity::PRESETS[] = {StrId::STR_ASK_SUMMARIZE, StrId::STR_ASK_WHAT_HAPPENED,
                                          StrId::STR_ASK_CHARACTERS, StrId::STR_ASK_EXPLAIN_PAGE};
const int AskBookActivity::PRESET_COUNT = sizeof(AskBookActivity::PRESETS) / sizeof(AskBookActivity::PRESETS[0]);

void AskBookActivity::onEnter() {
  Activity::onEnter();
  if (contextText.empty()) {
    fail(StrId::STR_ASK_NO_TEXT);
    return;
  }
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  showQuestionPicker();
}

void AskBookActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void AskBookActivity::returnToReader() { activityManager.goToReader(epubPath); }

void AskBookActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void AskBookActivity::showQuestionPicker() {
  questionOptions.clear();
  for (int i = 0; i < PRESET_COUNT; ++i) questionOptions.emplace_back(I18N.get(PRESETS[i]));
  questionOptions.emplace_back(tr(STR_ASK_CUSTOM));
  state = PICK;
  questionPopup.show(StrId::STR_ASK_BOOK, questionOptions, 0, [this](int idx) { onQuestionPicked(idx); });
  requestUpdate();
}

void AskBookActivity::onQuestionPicked(int index) {
  if (index < 0 || index > PRESET_COUNT) {
    returnToReader();
    return;
  }
  if (index == PRESET_COUNT) {
    typeQuestion();
    return;
  }
  question = I18N.get(PRESETS[index]);
  connectThenAsk();
}

void AskBookActivity::typeQuestion() {
  state = TYPING;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ASK_QUESTION_TITLE), "",
                                              MAX_QUESTION_LENGTH),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          showQuestionPicker();
          return;
        }
        question = std::get<KeyboardResult>(result.data).text;
        if (question.empty()) {
          showQuestionPicker();
          return;
        }
        connectThenAsk();
      });
}

void AskBookActivity::connectThenAsk() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void AskBookActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  // The request runs from loop() so the "asking" screen paints first.
  state = ASKING;
  askPending = true;
  requestUpdate();
}

void AskBookActivity::performAsk() {
  askPending = false;
  WiFi.setSleep(false);

  std::string body;
  {
    JsonDocument doc;
    doc["book"] = bookTitle;
    doc["chapter"] = chapterTitle;
    doc["text"] = contextText;
    doc["page"] = pageText;
    doc["question"] = question;
    doc["lang"] = I18N.getLanguage() == Language::ES ? "es" : "en";
    serializeJson(doc, body);
  }
  LOG_DBG(TAG, "POST /api/ask: %u bytes, question \"%s\"", (unsigned)body.size(), question.c_str());

  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/ask", body, resp, ASK_TIMEOUT_MS);
  body.clear();
  body.shrink_to_fit();
  WiFi.setSleep(true);

  if (r != ServerClient::Result::Ok) {
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_ASK_FAILED, detail);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    fail(StrId::STR_ASK_FAILED, "bad json");
    return;
  }
  answer = doc["answer"] | "";
  if (answer.empty()) {
    fail(StrId::STR_ASK_FAILED, doc["error"] | "empty answer");
    return;
  }
  LOG_DBG(TAG, "answer: %u bytes (in %d / out %d tokens)", (unsigned)answer.size(), (int)(doc["usage"]["input"] | 0),
          (int)(doc["usage"]["output"] | 0));
  showAnswer();
}

void AskBookActivity::showAnswer() {
  state = ANSWER;
  // The paged viewer the dictionary uses: question in the header, answer
  // word-wrapped and paged with the nav buttons, Back returns here.
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, question, answer),
                         [this](const ActivityResult&) { showQuestionPicker(); });
}

void AskBookActivity::loop() {
  switch (state) {
    case PICK:
      if (questionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
        // Back dismisses the popup without a selection: leave to the reader.
        if (!questionPopup.isActive()) returnToReader();
      }
      break;
    case ASKING:
      if (askPending) performAsk();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        returnToReader();
      }
      break;
    case TYPING:
    case CONNECTING:
    case ANSWER:
      break;  // a sub-activity owns input
  }
}

void AskBookActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ASK_BOOK));

  const char* subtitle = !chapterTitle.empty() ? chapterTitle.c_str() : bookTitle.c_str();
  const std::string shortSubtitle = renderer.truncatedText(UI_10_FONT_ID, subtitle, pageWidth - 40);

  switch (state) {
    case PICK:
      renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 20, shortSubtitle.c_str());
      if (questionPopup.processRender(renderer, mappedInput)) return;
      break;
    case ASKING:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, shortSubtitle.c_str());
      renderer.drawCenteredText(UI_12_FONT_ID, mid, tr(STR_ASK_ASKING), true, EpdFontFamily::BOLD);
      break;
    case FAILED: {
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, failureDetail.c_str());
      break;
    }
    case TYPING:
    case CONNECTING:
    case ANSWER:
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
