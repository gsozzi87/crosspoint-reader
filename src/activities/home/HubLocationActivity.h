#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "voice/VoiceRecorder.h"

// Weather place, set by voice (no keyboard on this device): say the city, the
// server transcribes and geocodes it, pick the match, the server keeps it and
// the hub re-syncs with the new weather. Settings -> Weather location.
class HubLocationActivity final : public Activity {
 public:
  explicit HubLocationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HubLocation", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != PICK && state != FAILED; }

 private:
  enum State { RECORDING, CONNECTING, TRANSCRIBING, SEARCHING, PICK, SAVING, FAILED };
  State state = RECORDING;

  struct Candidate {
    std::string label;  // "Rosario, Santa Fe, Argentina"
    std::string name;
    double lat = 0;
    double lon = 0;
    std::string timezone;
  };

  VoiceRecorder recorder{6};
  std::string spoken;
  std::vector<Candidate> candidates;
  std::vector<std::string> candidateLabels;
  OptionPopup picker;
  StrId failureId = StrId::STR_HUB_LOCATION_NONE;
  std::string failureDetail;
  bool wifiActivated = false;
  bool handoff = false;  // leaving for HubSyncActivity: keep WiFi up
  bool requestPending = false;
  int chosen = -1;

  void startRecording();
  void stopRecording();
  void onWifiSelectionComplete(bool connected);
  void performTranscribe();
  void performSearch();
  void performSave();
  void fail(StrId why, std::string detail = "");
  void leave();
};
