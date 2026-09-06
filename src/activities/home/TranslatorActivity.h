#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "voice/SpeechOut.h"
#include "voice/VoiceRecorder.h"

// Conversation translator (its own tile, apart from Talk's "translate ..."):
// pick the other language once (remembered), then OK = I speak (UI language
// -> other), Up = the other person speaks (other -> mine). Each take shows
// both texts and is read aloud in the target language by the server's Piper.
// WiFi stays up for the whole conversation; Back leaves to the hub.
class TranslatorActivity final : public Activity {
 public:
  explicit TranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Translator", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != IDLE && state != FAILED; }

 private:
  enum State { PICK_LANG, IDLE, RECORDING, CONNECTING, SENDING, FAILED };
  State state = PICK_LANG;

  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  std::vector<std::string> pickerCodes;
  std::string mine;   // UI language code
  std::string other;  // the other side
  bool meSpeaking = true;

  VoiceRecorder recorder{12};
  SpeechOut speech;
  std::string original;
  std::string translation;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  bool wifiActivated = false;
  bool requestPending = false;

  void showLanguagePicker();
  void startRecording(bool me);
  void stopRecording();
  void onWifiSelectionComplete(bool connected);
  void performRequest();
  void fail(StrId why, std::string detail = "");
  void leave();
  static const char* languageName(const std::string& code);
};
