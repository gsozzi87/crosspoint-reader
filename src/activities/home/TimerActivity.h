#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "voice/AlertBeep.h"
#include "voice/SpeechOut.h"

// The Time tile: countdown timer, stopwatch and Pomodoro (25/5), big 7-segment
// digits drawn with rectangles (no large font on board). Refreshes once a
// second while the seconds matter, with a clean refresh every so often so the
// panel does not ghost. Beeps at the end until OK. A voice "set 10 minutes"
// lands here with the seconds already chosen.
class TimerActivity final : public Activity {
 public:
  explicit TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int countdownSeconds = 0)
      : Activity("Timer", renderer, mappedInput), presetSeconds(countdownSeconds) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return mode != PICK && running; }

 private:
  enum Mode { PICK, COUNTDOWN, STOPWATCH, POMODORO };
  Mode mode = PICK;
  int presetSeconds;
  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  bool pickingDuration = false;

  bool running = false;
  bool finished = false;
  unsigned long startMs = 0;      // when the current run (segment) started
  unsigned long accumulatedMs = 0;  // paused time carried over
  long totalSeconds = 0;          // countdown / pomodoro segment length
  bool pomodoroBreak = false;
  int pomodoroRound = 1;
  long lastShownSeconds = -1;
  int partialCount = 0;
  AlertBeep beep;
  SpeechOut speech;
  bool spoken = false;

  long elapsedMs() const;
  long remainingSeconds() const;
  void showModePicker();
  void showDurationPicker();
  void startSegment(long seconds);
  void ring();
  void drawBigTime(long seconds, int centerY) const;
};
