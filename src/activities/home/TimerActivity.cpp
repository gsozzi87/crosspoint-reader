#include "TimerActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr int DURATIONS_MIN[] = {1, 3, 5, 10, 15, 20, 25, 30, 45, 60};
constexpr int DURATION_COUNT = sizeof(DURATIONS_MIN) / sizeof(DURATIONS_MIN[0]);
constexpr long POMODORO_WORK_S = 25 * 60;
constexpr long POMODORO_BREAK_S = 5 * 60;
constexpr int PARTIALS_BEFORE_CLEAN = 40;

// 7-segment digit: segments a b c d e f g (top, top-right, bottom-right, bottom, bottom-left, top-left, middle)
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void drawDigit(const GfxRenderer& r, int digit, int x, int y, int w, int h, int t) {
  const uint8_t s = SEGMENTS[digit % 10];
  const int half = h / 2;
  if (s & 0x01) r.fillRect(x + t, y, w - 2 * t, t);                    // a
  if (s & 0x02) r.fillRect(x + w - t, y + t, t, half - t);              // b
  if (s & 0x04) r.fillRect(x + w - t, y + half, t, half - t);           // c
  if (s & 0x08) r.fillRect(x + t, y + h - t, w - 2 * t, t);             // d
  if (s & 0x10) r.fillRect(x, y + half, t, half - t);                   // e
  if (s & 0x20) r.fillRect(x, y + t, t, half - t);                      // f
  if (s & 0x40) r.fillRect(x + t, y + half - t / 2, w - 2 * t, t);      // g
}
}  // namespace

void TimerActivity::onEnter() {
  Activity::onEnter();
  if (presetSeconds > 0) {
    mode = COUNTDOWN;
    startSegment(presetSeconds);
  } else {
    showModePicker();
  }
}

void TimerActivity::onExit() {
  Activity::onExit();
  speech.stop();
  beep.stop();
}

void TimerActivity::showModePicker() {
  mode = PICK;
  running = false;
  finished = false;
  pickingDuration = false;
  pickerOptions = {tr(STR_TIMER_COUNTDOWN), tr(STR_TIMER_STOPWATCH), tr(STR_TIMER_POMODORO)};
  picker.show(StrId::STR_HUB_TIMER, pickerOptions, 0, [this](int idx) {
    if (idx == 0) {
      showDurationPicker();
    } else if (idx == 1) {
      mode = STOPWATCH;
      startSegment(0);
    } else if (idx == 2) {
      mode = POMODORO;
      pomodoroBreak = false;
      pomodoroRound = 1;
      startSegment(POMODORO_WORK_S);
    }
  });
  requestUpdate();
}

void TimerActivity::showDurationPicker() {
  pickingDuration = true;
  pickerOptions.clear();
  for (int i = 0; i < DURATION_COUNT; ++i) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d min", DURATIONS_MIN[i]);
    pickerOptions.emplace_back(buf);
  }
  picker.show(StrId::STR_TIMER_COUNTDOWN, pickerOptions, 3, [this](int idx) {
    if (idx < 0 || idx >= DURATION_COUNT) {
      showModePicker();
      return;
    }
    mode = COUNTDOWN;
    startSegment(DURATIONS_MIN[idx] * 60L);
  });
  requestUpdate();
}

void TimerActivity::startSegment(const long seconds) {
  totalSeconds = seconds;
  accumulatedMs = 0;
  startMs = millis();
  running = true;
  finished = false;
  lastShownSeconds = -1;
  partialCount = 0;
  speech.stop();
  beep.stop();
  requestUpdate();
}

long TimerActivity::elapsedMs() const { return accumulatedMs + (running ? millis() - startMs : 0); }

long TimerActivity::remainingSeconds() const {
  const long rem = totalSeconds - elapsedMs() / 1000;
  return rem < 0 ? 0 : rem;
}

void TimerActivity::ring() {
  running = false;
  finished = true;
  const std::string clip = std::string("/.crosspoint/tts/timer-") + uiLanguageCode() + ".bin";
  spoken = !speech.playFile(clip.c_str());
  if (spoken) beep.start();
  requestUpdate();
}

void TimerActivity::loop() {
  if (mode == PICK) {
    if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (mode == PICK && !picker.isActive()) {
        if (pickingDuration) showModePicker();
        else finish();  // pushed from the hub: back to it; after a voice handoff the empty stack goes home
      }
    }
    return;
  }

  if (finished) {
    if (!spoken && !speech.isPlaying()) {
      spoken = true;
      speech.stop();
      beep.start();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      speech.stop();
      beep.stop();
      if (mode == POMODORO) {
        pomodoroBreak = !pomodoroBreak;
        if (!pomodoroBreak) pomodoroRound++;
        startSegment(pomodoroBreak ? POMODORO_BREAK_S : POMODORO_WORK_S);
      } else {
        showModePicker();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Pause / resume
    if (running) {
      accumulatedMs += millis() - startMs;
      running = false;
    } else {
      startMs = millis();
      running = true;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mode == STOPWATCH && !running && elapsedMs() > 0) {
      accumulatedMs = 0;  // reset a paused stopwatch first, leave on the next Back
      requestUpdate();
      return;
    }
    showModePicker();
    return;
  }

  if (!running) return;
  const long shown = mode == STOPWATCH ? elapsedMs() / 1000 : remainingSeconds();
  if (mode != STOPWATCH && shown == 0) {
    ring();
    return;
  }
  if (shown != lastShownSeconds) {
    // Seconds always for the stopwatch; a countdown ticks every 10 s until the last minute.
    if (mode == STOPWATCH || shown <= 60 || shown % 10 == 0) requestUpdate();
  }
}

void TimerActivity::drawBigTime(const long seconds, const int centerY) const {
  const int pageWidth = renderer.getScreenWidth();
  const long mm = seconds / 60;
  const long ss = seconds % 60;
  const int digitW = 70, digitH = 120, thick = 14, gap = 16, colonW = 30;
  const bool hours = mm >= 100;
  const int digits = hours ? 6 : 4;
  const int totalW = digits * digitW + (digits - 1) * gap + (hours ? 2 : 1) * colonW;
  int x = (pageWidth - totalW) / 2;
  const int y = centerY - digitH / 2;
  auto colon = [&] {
    renderer.fillRect(x + colonW / 2 - thick / 2, y + digitH / 3 - thick / 2, thick, thick);
    renderer.fillRect(x + colonW / 2 - thick / 2, y + 2 * digitH / 3 - thick / 2, thick, thick);
    x += colonW + gap;
  };
  if (hours) {
    const long hh = mm / 60;
    drawDigit(renderer, (hh / 10) % 10, x, y, digitW, digitH, thick); x += digitW + gap;
    drawDigit(renderer, hh % 10, x, y, digitW, digitH, thick); x += digitW + gap;
    colon();
  }
  const long m = hours ? mm % 60 : mm;
  drawDigit(renderer, (m / 10) % 10, x, y, digitW, digitH, thick); x += digitW + gap;
  drawDigit(renderer, m % 10, x, y, digitW, digitH, thick); x += digitW + gap;
  colon();
  drawDigit(renderer, ss / 10, x, y, digitW, digitH, thick); x += digitW + gap;
  drawDigit(renderer, ss % 10, x, y, digitW, digitH, thick);
}

void TimerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const char* title = mode == STOPWATCH ? tr(STR_TIMER_STOPWATCH) : mode == POMODORO ? tr(STR_TIMER_POMODORO) : tr(STR_TIMER_COUNTDOWN);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, mode == PICK ? tr(STR_HUB_TIMER) : title);

  if (mode == PICK) {
    if (picker.processRender(renderer, mappedInput)) return;
    renderer.displayBuffer();
    return;
  }

  const long shown = mode == STOPWATCH ? elapsedMs() / 1000 : remainingSeconds();
  lastShownSeconds = shown;
  drawBigTime(shown, pageHeight / 2 - 40);

  char sub[64] = "";
  if (finished) {
    snprintf(sub, sizeof(sub), "%s", mode == POMODORO && !pomodoroBreak ? tr(STR_TIMER_BREAK_TIME) : tr(STR_TIMER_DONE));
  } else if (mode == POMODORO) {
    snprintf(sub, sizeof(sub), "%s %d · %s", tr(STR_TIMER_ROUND), pomodoroRound, pomodoroBreak ? tr(STR_TIMER_BREAK) : tr(STR_TIMER_WORK));
  } else if (!running) {
    snprintf(sub, sizeof(sub), "%s", tr(STR_TIMER_PAUSED));
  }
  if (sub[0]) renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + 60, sub, true, EpdFontFamily::BOLD);

  const char* confirmLabel = finished ? tr(STR_AGENDA_DONE) : running ? tr(STR_TIMER_PAUSE) : tr(STR_TIMER_RESUME);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Mostly partial refreshes; a clean one now and then keeps the digits crisp.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN || finished;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
