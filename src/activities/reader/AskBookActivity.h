#pragma once

#include <AudioManager.h>
#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// "Ask the book": the reader hands over the chapter text read so far (up to
// MAX_CONTEXT_BYTES, most recent pages kept) and the current page; the user
// picks a preset question or asks one BY VOICE (the device has no keyboard,
// ever): the mic take goes to the server for transcription, then text plus
// question go to the device's own server (POST /api/ask, answered by Claude
// there) and the answer is shown in a paged viewer. Runs in place of the
// reader, like KOReader sync: WiFi + TLS need the heap the open book would
// otherwise hold, and the reader is re-entered through silentRestartToReader().
class AskBookActivity final : public Activity {
 public:
  static constexpr size_t MAX_CONTEXT_BYTES = 24 * 1024;

  explicit AskBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                           std::string bookTitle, std::string chapterTitle, std::string contextText,
                           std::string pageText)
      : Activity("AskBook", renderer, mappedInput),
        epubPath(std::move(epubPath)),
        bookTitle(std::move(bookTitle)),
        chapterTitle(std::move(chapterTitle)),
        contextText(std::move(contextText)),
        pageText(std::move(pageText)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override {
    return state == RECORDING || state == CONNECTING || state == TRANSCRIBING || state == ASKING;
  }

 private:
  enum State { PICK, RECORDING, CONNECTING, TRANSCRIBING, ASKING, ANSWER, FAILED };
  State state = PICK;

  // Voice take: 16 kHz mono 16-bit, capped so the upload stays small.
  static constexpr uint32_t SAMPLE_RATE = 16000;
  static constexpr uint32_t MAX_SECONDS = 10;
  static constexpr size_t MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS;
  static constexpr size_t MIN_SAMPLES = SAMPLE_RATE / 2;  // shorter than 0.5 s = accidental press

  std::string epubPath;
  std::string bookTitle;
  std::string chapterTitle;
  std::string contextText;
  std::string pageText;

  OptionPopup questionPopup;
  std::vector<std::string> questionOptions;
  std::string question;
  std::string answer;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  bool wifiActivated = false;
  bool requestPending = false;

  AudioManager audio;
  uint8_t* wavBuffer = nullptr;  // WAV header + samples, PSRAM
  size_t recorded = 0;           // samples captured
  bool voiceQuestion = false;

  static const StrId PRESETS[];
  static const int PRESET_COUNT;

  void showQuestionPicker();
  void onQuestionPicked(int index);
  void startRecording();
  void pumpRecording();
  void stopRecording();
  void connectThenSend();
  void onWifiSelectionComplete(bool connected);
  void performTranscribe();
  void performAsk();
  void showAnswer();
  void fail(StrId why, std::string detail = "");
  void releaseTake();
  void returnToReader();
};
