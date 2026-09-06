#pragma once

#include <string>

#include "activities/Activity.h"
#include "voice/AlertBeep.h"
#include "voice/SpeechOut.h"

// A reminder came due: title and time big on screen, a beep pattern through
// the speaker (looped, up to a minute), OK = done, Back = snooze 10 minutes.
// Reached from the hub while awake, or from the boot path after the deep-sleep
// timer wake that armReminderWake() set for it.
class ReminderAlertActivity final : public Activity {
 public:
  ReminderAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int reminderId, std::string title,
                        std::string when)
      : Activity("ReminderAlert", renderer, mappedInput),
        reminderId(reminderId),
        title(std::move(title)),
        when(std::move(when)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  static constexpr unsigned long BEEP_MS = 60000;
  static constexpr time_t SNOOZE_S = 10 * 60;

  int reminderId;
  std::string title;
  std::string when;
  AlertBeep beep;
  SpeechOut speech;
  bool spoken = false;  // the cached "Reminder: <title>" clip was played (or absent)
  unsigned long startedAt = 0;
  void done();
  void snooze();
  void leave();
};
