#pragma once

#include <Arduino.h>

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
  explicit TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int countdownSeconds = 0,
                         bool resumeFired = false)
      : Activity("Timer", renderer, mappedInput), presetSeconds(countdownSeconds), resumeFired(resumeFired) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Solo mientras suena, y con tope: con el estado guardado, dormir es lo
  // correcto (el aparato se despierta con el timer de deep sleep y suena).
  // Antes devolvía true mientras corría y por eso nunca llegaba a ese wake.
  bool preventAutoSleep() override { return finished && millis() - finishedAt < RING_MAX_MS; }

 private:
  enum Mode { PICK, COUNTDOWN, STOPWATCH, POMODORO };
  Mode mode = PICK;
  int presetSeconds;
  const bool resumeFired = false;  // woken by the deep-sleep timer: ring right away
  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  bool pickingDuration = false;
  // Volver a mostrar un popup desde adentro de su propio callback destruiría el
  // std::function en ejecución: se anota acá y lo atiende loop().
  enum PendingPicker { NONE, MODE_PICKER, DURATION_PICKER };
  PendingPicker pendingPicker = NONE;

  static constexpr unsigned long RING_MAX_MS = 3 * 60 * 1000;  // si nadie atiende, dejarlo dormir

  bool running = false;
  bool finished = false;
  unsigned long finishedAt = 0;
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
  void persist();       // store endAt so the timer survives sleep
  bool resumeStored();  // pick a stored timer back up (entering or waking)
  void ring();
  void drawBigTime(long seconds, int centerY) const;
};
