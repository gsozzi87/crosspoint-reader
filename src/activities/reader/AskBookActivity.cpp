#include "AskBookActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "ASK";
// Claude with a long chapter takes longer than the ServerClient default that
// is sized for pings.
constexpr uint32_t ASK_TIMEOUT_MS = 90000;
}  // namespace

const StrId AskBookActivity::PRESETS[] = {StrId::STR_ASK_SUMMARIZE, StrId::STR_ASK_WHAT_HAPPENED,
                                          StrId::STR_ASK_CHARACTERS, StrId::STR_ASK_EXPLAIN_PAGE};
const int AskBookActivity::PRESET_COUNT = sizeof(AskBookActivity::PRESETS) / sizeof(AskBookActivity::PRESETS[0]);

void AskBookActivity::onEnter() {
  Activity::onEnter();
  if (!generalMode && contextText.empty()) {
    fail(StrId::STR_ASK_NO_TEXT);
    return;
  }
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  if (generalMode) {
    startRecording();
  } else {
    showQuestionPicker();
  }
}

void AskBookActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    if (generalMode) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
  }
}

void AskBookActivity::returnToReader() {
  if (generalMode) {
    activityManager.goHome();
  } else {
    activityManager.goToReader(epubPath);
  }
}

void AskBookActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void AskBookActivity::showQuestionPicker() {
  recorder.release();
  // No list without a book: after an answer, a too-short take or Back while
  // recording, general mode just goes back to the hub.
  if (generalMode) {
    returnToReader();
    return;
  }
  questionOptions.clear();
  // Voice first: it is the way to ask anything not on the list.
  questionOptions.emplace_back(tr(STR_ASK_VOICE));
  for (int i = 0; i < PRESET_COUNT; ++i) questionOptions.emplace_back(I18N.get(PRESETS[i]));
  state = PICK;
  questionPopup.show(StrId::STR_ASK_BOOK, questionOptions, 0, [this](int idx) { onQuestionPicked(idx); });
  requestUpdate();
}

void AskBookActivity::onQuestionPicked(int index) {
  if (index < 0 || index > PRESET_COUNT) {
    returnToReader();
    return;
  }
  if (index == 0) {
    startRecording();
    return;
  }
  voiceQuestion = false;
  question = I18N.get(PRESETS[index - 1]);
  connectThenSend();
}

void AskBookActivity::startRecording() {
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  voiceQuestion = true;
  state = RECORDING;
  requestUpdate();
}

void AskBookActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    LOG_DBG(TAG, "Take too short, back to the picker");
    showQuestionPicker();
    return;
  }
  connectThenSend();
}

void AskBookActivity::connectThenSend() {
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
  // The requests run from loop() so the status screen paints first.
  state = voiceQuestion ? TRANSCRIBING : ASKING;
  requestPending = true;
  requestUpdate();
}

void AskBookActivity::performTranscribe() {
  requestPending = false;
  std::string detail;
  const bool ok = SpeechToText::transcribe(recorder, question, detail);
  recorder.release();
  if (!ok) {
    fail(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  state = ASKING;
  requestPending = true;
  requestUpdate();
}

void AskBookActivity::performAsk() {
  requestPending = false;
  WiFi.setSleep(false);

  std::string body;
  {
    JsonDocument doc;
    if (!generalMode) {
      doc["book"] = bookTitle;
      doc["chapter"] = chapterTitle;
      doc["text"] = contextText;
      doc["page"] = pageText;
    }
    doc["question"] = question;
    doc["lang"] = I18N.getLanguage() == Language::ES ? "es" : "en";
    serializeJson(doc, body);
  }
  LOG_DBG(TAG, "POST /api/ask: %u bytes", (unsigned)body.size());

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
  // The paged viewer the dictionary uses: the question (as understood) in the
  // header, the answer word-wrapped and paged with the nav buttons, Back
  // returns here.
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, question, answer),
                         [this](const ActivityResult&) { showQuestionPicker(); });
}

void AskBookActivity::loop() {
  switch (state) {
    case PICK:
      if (questionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
        // The popup goes inactive both on a selection (which already moved us
        // to another state through onQuestionPicked) and on Back. Only the
        // latter, still in PICK, means "leave to the reader".
        if (state == PICK && !questionPopup.isActive()) returnToReader();
      }
      break;
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        showQuestionPicker();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        stopRecording();  // OK, or the 10 s cap reached inside pump()
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case TRANSCRIBING:
      if (requestPending) performTranscribe();
      break;
    case ASKING:
      if (requestPending) performAsk();
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        returnToReader();
      }
      break;
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
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 generalMode ? tr(STR_HUB_ASK) : tr(STR_ASK_BOOK));

  const char* subtitle = !chapterTitle.empty() ? chapterTitle.c_str() : bookTitle.c_str();
  const std::string shortSubtitle = renderer.truncatedText(UI_10_FONT_ID, subtitle, pageWidth - 40);
  const char* confirmLabel = "";

  switch (state) {
    case PICK:
      if (!generalMode) {
        renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 20, shortSubtitle.c_str());
      }
      if (questionPopup.processRender(renderer, mappedInput)) return;
      break;
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_ASK_RECORDING), true, EpdFontFamily::BOLD);
      confirmLabel = tr(STR_SELECT);
      break;
    case TRANSCRIBING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_ASK_TRANSCRIBING), true, EpdFontFamily::BOLD);
      break;
    case ASKING: {
      const std::string shortQuestion = renderer.truncatedText(UI_10_FONT_ID, question.c_str(), pageWidth - 40);
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, shortQuestion.c_str());
      renderer.drawCenteredText(UI_12_FONT_ID, mid, tr(STR_ASK_ASKING), true, EpdFontFamily::BOLD);
      break;
    }
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, failureDetail.c_str());
      break;
    case CONNECTING:
    case ANSWER:
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
