#pragma once

#include <AudioManager.h>

#include <string>

#include "activities/Activity.h"

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
  AudioManager audio;
  uint8_t* beep = nullptr;
  size_t beepBytes = 0;
  unsigned long startedAt = 0;
  bool beeping = false;

  void startBeep();
  void stopBeep();
  void done();
  void snooze();
  void leave();
};
