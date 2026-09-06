#pragma once

#include <string>

#include "activities/Activity.h"
#include "voice/SpeechOut.h"
#include "voice/VoiceRecorder.h"

// The hub's Talk tile (push-to-talk): one take, POST /api/voice, and the
// server decides what it was — a question it answers, a reminder / task /
// shopping item / note / message it saves — then the hub cache is refreshed so
// the widgets show the result, and the reply is shown in the paged viewer.
// Back returns to the hub through a silent restart (WiFi heap).
class VoiceActivity final : public Activity {
 public:
  explicit VoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Voice", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != REPLY && state != FAILED; }

 private:
  enum State { RECORDING, CONNECTING, SENDING, REPLY, FAILED };
  State state = RECORDING;

  VoiceRecorder recorder{12};
  std::string heard;   // what the server understood
  std::string intent;  // question | reminder | task | ...
  std::string reply;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  bool wifiActivated = false;
  bool requestPending = false;
  int timerSeconds = 0;  // timer/alarm intent: hand off to TimerActivity
  SpeechOut speech;      // the reply, spoken by the server's Piper, played with the text

  void startRecording();
  void stopRecording();
  void onWifiSelectionComplete(bool connected);
  void performRequest();
  void showReply();
  void fail(StrId why, std::string detail = "");
  void leave();
  const char* intentTitle() const;
};
