#include "TimerActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <HalClock.h>
#include <Logging.h>

#include "HubStore.h"
#include "input/MotionInput.h"
#include "MappedInputManager.h"
#include "components/SevenSegment.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"
#include "voice/SpeechCache.h"

namespace {
constexpr int DURATIONS_MIN[] = {1, 3, 5, 10, 15, 20, 25, 30, 45, 60};
constexpr int DURATION_COUNT = sizeof(DURATIONS_MIN) / sizeof(DURATIONS_MIN[0]);
constexpr long POMODORO_WORK_S = 25 * 60;
constexpr long POMODORO_BREAK_S = 5 * 60;
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
constexpr unsigned long CANCEL_HOLD_MS = 1000;  // Atrás mantenido: cancelar

// Los dígitos de 7 segmentos viven en components/SevenSegment.h: los usa también
// el reproductor de música.
void drawDigit(const GfxRenderer& r, int digit, int x, int y, int w, int h, int t) {
  sevenseg::digit(r, digit, x, y, w, h, t);
}
}  // namespace

void TimerActivity::onEnter() {
  Activity::onEnter();
  if (resumeFired) {
    // Woken because the countdown ran out: show it finished and ring.
    mode = HUB_STORE.timerMode == 0 ? COUNTDOWN : POMODORO;
    pomodoroBreak = HUB_STORE.timerMode == 2;
    pomodoroRound = HUB_STORE.timerRound > 0 ? HUB_STORE.timerRound : 1;
    totalSeconds = HUB_STORE.timerTotal;
    ring();
    return;
  }
  if (presetSeconds > 0) {
    mode = COUNTDOWN;
    startSegment(presetSeconds);
  } else if (resumeStored()) {
    // Something was already running (the device slept in between).
  } else {
    showModePicker();
  }
}

// El estado vive en el store como tiempo absoluto: dormir, despertar o salir de
// la pantalla no lo pierde. Salir con Atrás deja todo corriendo; solo Atrás
// largo (o el final) lo borra.
void TimerActivity::persist() {
  time_t now = 0;
  const bool haveClock = halClock.getEpochUtc(now);
  if (!haveClock) LOG_ERR("TIMER", "sin hora del RTC: no puede sonar dormido (sincronizá el hub)");
  const long elapsedS = elapsedMs() / 1000;
  if (mode == STOPWATCH) {
    HUB_STORE.clearTimer();
    if (finished || (!running && elapsedS <= 0)) {
      HUB_STORE.clearStopwatch();
    } else if (running && haveClock) {
      HUB_STORE.stopwatchStartAt = now - elapsedS;  // el hub calcula lo corrido desde acá
      HUB_STORE.stopwatchAccumS = 0;
    } else {
      HUB_STORE.stopwatchStartAt = 0;
      HUB_STORE.stopwatchAccumS = static_cast<int>(elapsedS);
    }
  } else {
    HUB_STORE.clearStopwatch();
    const long left = remainingSeconds();
    if (finished || left <= 0) {
      HUB_STORE.clearTimer();
    } else {
      HUB_STORE.timerTotal = static_cast<int>(totalSeconds);
      HUB_STORE.timerMode = mode == POMODORO ? (pomodoroBreak ? 2 : 1) : 0;
      HUB_STORE.timerRound = static_cast<uint8_t>(pomodoroRound);
      // Sin reloj no se puede guardar un fin absoluto: se guarda como pausado
      // para no perderlo (no va a sonar dormido, pero al volver sigue ahí).
      if (running && haveClock) {
        HUB_STORE.timerEndAt = now + left;
        HUB_STORE.timerPausedLeft = 0;
      } else {
        HUB_STORE.timerEndAt = 0;
        HUB_STORE.timerPausedLeft = static_cast<int>(left);
      }
    }
  }
  HUB_STORE.saveToFile();
}

// Retoma lo que haya quedado corriendo o pausado (al entrar de nuevo a Tiempo o
// al arrancar después de dormir).
bool TimerActivity::resumeStored() {
  time_t now = 0;
  const bool haveClock = halClock.getEpochUtc(now);
  if (HUB_STORE.stopwatchActive()) {
    mode = STOPWATCH;
    totalSeconds = 0;
    const long el = haveClock ? HUB_STORE.stopwatchElapsed(now) : HUB_STORE.stopwatchAccumS;
    accumulatedMs = el > 0 ? el * 1000L : 0;
    running = HUB_STORE.stopwatchStartAt > 0 && haveClock;
    startMs = millis();
    finished = false;
    lastShownSeconds = -1;
    partialCount = 0;
    requestUpdate();
    return true;
  }
  const bool countdown = HUB_STORE.timerRunning() || HUB_STORE.timerPaused();
  if (!countdown) return false;
  mode = HUB_STORE.timerMode == 0 ? COUNTDOWN : POMODORO;
  pomodoroBreak = HUB_STORE.timerMode == 2;
  pomodoroRound = HUB_STORE.timerRound > 0 ? HUB_STORE.timerRound : 1;
  finished = false;
  lastShownSeconds = -1;
  partialCount = 0;
  if (HUB_STORE.timerRunning()) {
    if (!haveClock) return false;  // no se puede saber cuánto queda
    const long left = static_cast<long>(HUB_STORE.timerEndAt - now);
    if (left <= 0) {
      totalSeconds = HUB_STORE.timerTotal;
      ring();
      return true;
    }
    totalSeconds = HUB_STORE.timerTotal > left ? HUB_STORE.timerTotal : left;
    accumulatedMs = (totalSeconds - left) * 1000L;
    startMs = millis();
    running = true;
  } else {
    const long left = HUB_STORE.timerPausedLeft;
    totalSeconds = HUB_STORE.timerTotal > left ? HUB_STORE.timerTotal : left;
    accumulatedMs = (totalSeconds - left) * 1000L;
    running = false;
  }
  requestUpdate();
  return true;
}

void TimerActivity::onExit() {
  Activity::onExit();
  speech.stop();
  beep.stop();
}

void TimerActivity::showModePicker() {
  mode = PICK;
  pendingPicker = NONE;
  running = false;
  finished = false;
  pickingDuration = false;
  pickerOptions = {tr(STR_TIMER_COUNTDOWN), tr(STR_TIMER_STOPWATCH), tr(STR_TIMER_POMODORO)};
  picker.show(StrId::STR_HUB_TIMER, pickerOptions, 0, [this](int idx) {
    if (idx == 0) {
      pendingPicker = DURATION_PICKER;
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
  pendingPicker = NONE;
  pickingDuration = true;
  pickerOptions.clear();
  for (int i = 0; i < DURATION_COUNT; ++i) {
    char buf[24];
    // Se reusa el "%u min" que ya está traducido en los siete idiomas.
    snprintf(buf, sizeof(buf), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned>(DURATIONS_MIN[i]));
    pickerOptions.emplace_back(buf);
  }
  picker.show(StrId::STR_TIMER_COUNTDOWN, pickerOptions, 3, [this](int idx) {
    if (idx < 0 || idx >= DURATION_COUNT) {
      pendingPicker = MODE_PICKER;
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
  persist();
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
  finishedAt = millis();
  // The alarm screen replaces the ticking digits: one clean refresh so no
  // countdown ghost stays under "Done" (the coordinator honors and counts it).
  renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
  HUB_STORE.clearTimer();
  HUB_STORE.saveToFile();
  spoken = !speech.playFile(speechcache::clipPath(tr(STR_TIMER_DONE)).c_str());
  if (spoken) {
    speech.stop();  // el I2S es uno solo: soltarlo antes de que lo abra el pitido
    beep.start();
  }
  requestUpdate();
}

void TimerActivity::loop() {
  if (pendingPicker != NONE) {
    const PendingPicker next = pendingPicker;
    pendingPicker = NONE;
    if (next == DURATION_PICKER) showDurationPicker();
    else showModePicker();
    return;
  }
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
    // Nadie atendió: callar y dejar que el aparato se duerma.
    if (millis() - finishedAt >= RING_MAX_MS) {
      speech.stop();
      beep.stop();
    }
    // Darlo vuelta o sacudirlo lo calla, igual que apretar un botón: cuando el
    // aparato está sonando en la mesa eso es lo que sale solo.
    const bool gesture = MOTION.take(MotionInput::Event::FaceDown) || MOTION.take(MotionInput::Event::Shake);
    if (gesture || mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
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
    persist();
    requestUpdate();
    return;
  }
  // Atrás largo cancela lo que esté corriendo; Atrás corto sale al hub y lo deja
  // corriendo (era lo que faltaba: antes salir lo borraba y nunca sonaba).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, CANCEL_HOLD_MS)) {
    HUB_STORE.clearTimer();
    HUB_STORE.clearStopwatch();
    HUB_STORE.saveToFile();
    showModePicker();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    persist();
    finish();
    return;
  }

  if (!running) return;
  const long shown = mode == STOPWATCH ? elapsedMs() / 1000 : remainingSeconds();
  if (mode != STOPWATCH && shown == 0) {
    ring();
    return;
  }
  if (shown != lastShownSeconds) {
    // Cronómetro: cada segundo. Cuenta regresiva: cada 5 s y al segundo en los
    // últimos 10, para no saturar el panel de parciales.
    if (mode == STOPWATCH || shown <= 10 || shown % 5 == 0) requestUpdate();
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

  if (!finished) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 88, tr(STR_TIMER_CANCEL_HINT));
  }

  const char* confirmLabel = finished ? tr(STR_AGENDA_DONE) : running ? tr(STR_TIMER_PAUSE) : tr(STR_TIMER_RESUME);
  const auto labels = mappedInput.mapLabels(finished ? tr(STR_BACK) : tr(STR_TIMER_LEAVE), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Mostly partial refreshes; a clean one now and then keeps the digits crisp.
  // The finished screen gets its clean via promoteNextRefresh() in ring(), not
  // on every repaint while ringing.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
