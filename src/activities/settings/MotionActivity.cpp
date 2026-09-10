#include "MotionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// El panel tarda medio segundo por refresco: los números se repintan como mucho
// dos veces por segundo, y solo si cambiaron lo suficiente para verse.
constexpr unsigned long PAINT_MS = 500;
constexpr int PARTIALS_BEFORE_CLEAN = 12;
}  // namespace

void MotionActivity::onEnter() {
  Activity::onEnter();
  state = LIVE;
  calFailed = false;
  // El giróscopo se enciende solo mientras esta pantalla está abierta: acá se
  // quiere ver todo, incluido el giro, y son cinco veces más consumo.
  MOTION.setGyro(true);
  requestUpdate();
}

void MotionActivity::onExit() {
  MOTION.setGyro(false);
  if (HUB_STORE.imuMap.calibrated) HUB_STORE.saveToFile();
  Activity::onExit();
}

void MotionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state == LIVE) {
      finish();
    } else {
      state = LIVE;
      calFailed = false;
      requestUpdate();
    }
    return;
  }

  switch (state) {
    case LIVE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = CAL_FLAT;
        calFailed = false;
        requestUpdate();
        return;
      }
      // Gestos: se muestran a medida que aparecen, para poder probarlos.
      if (MOTION.pending() != MotionInput::Event::None) {
        shown = MOTION.takeAny();
        shownAt = millis();
        requestUpdate();
        return;
      }
      break;
    case CAL_FLAT:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        calFailed = !MOTION.calibrateFlat();
        if (!calFailed) state = CAL_RIGHT;
        requestUpdate();
        return;
      }
      break;
    case CAL_RIGHT:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        calFailed = !MOTION.calibrateRight();
        if (!calFailed) state = CAL_TOWARD;
        requestUpdate();
        return;
      }
      break;
    case CAL_TOWARD:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        calFailed = !MOTION.calibrateToward();
        if (!calFailed) {
          state = CAL_DONE;
          HUB_STORE.saveToFile();
        }
        requestUpdate();
        return;
      }
      break;
    case CAL_DONE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = LIVE;
        requestUpdate();
        return;
      }
      break;
  }

  // Refresco periódico de los números en vivo.
  if (state == LIVE && millis() - lastPaint >= PAINT_MS) requestUpdate();
}

void MotionActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MOTION_TITLE));

  const int top = metrics.topPadding + metrics.headerHeight + 30;
  char line[128];

  if (!MOTION.available()) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_MOTION_NO_IMU), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == LIVE) {
    const MotionInput::Reading& r = MOTION.reading();
    int y = top;
    // Los tres ejes de pantalla, en milésimas de g, que es la unidad en la que
    // están escritos los umbrales.
    snprintf(line, sizeof(line), "X %+5d    Y %+5d    N %+5d", static_cast<int>(r.x * 1000),
             static_cast<int>(r.y * 1000), static_cast<int>(r.n * 1000));
    renderer.drawCenteredText(UI_12_FONT_ID, y, line, true, EpdFontFamily::BOLD);
    y += 36;
    snprintf(line, sizeof(line), "%d mg", static_cast<int>(r.magnitude * 1000));
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    y += 36;
    if (MOTION.gyroOn()) {
      snprintf(line, sizeof(line), "%+d  %+d  %+d dps", static_cast<int>(r.gx), static_cast<int>(r.gy),
               static_cast<int>(r.gn));
      renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    }
    y += 44;

    snprintf(line, sizeof(line), "%s: %s", tr(STR_MOTION_LAST),
             shown == MotionInput::Event::None ? tr(STR_MOTION_TEST) : MotionInput::name(shown));
    renderer.drawCenteredText(UI_12_FONT_ID, y, line, true, EpdFontFamily::BOLD);
    y += 40;
    renderer.drawCenteredText(UI_10_FONT_ID, y, MOTION.tapTrusted() ? tr(STR_MOTION_TAP_ON) : tr(STR_MOTION_TAP_OFF));
    y += 34;
    const auto& m = HUB_STORE.imuMap;
    snprintf(line, sizeof(line), "n=%c%c  x=%c%c  y=%c%c", m.normalSign < 0 ? '-' : '+',
             static_cast<char>('X' + m.normalAxis), m.xSign < 0 ? '-' : '+', static_cast<char>('X' + m.xAxis),
             m.ySign < 0 ? '-' : '+', static_cast<char>('X' + m.yAxis));
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);

    for (const std::string& l : renderer.wrappedText(UI_10_FONT_ID, tr(STR_MOTION_HELP), pageWidth - 60, 6)) {
      y += 34;
      renderer.drawCenteredText(UI_10_FONT_ID, y, l.c_str());
    }
  } else {
    // Calibración: una consigna grande por paso y nada más, que es lo que se
    // puede leer con el aparato inclinado en la mano.
    StrId what = StrId::STR_MOTION_CAL_FLAT;
    if (state == CAL_RIGHT) what = StrId::STR_MOTION_CAL_RIGHT;
    else if (state == CAL_TOWARD) what = StrId::STR_MOTION_CAL_TOWARD;
    else if (state == CAL_DONE) what = StrId::STR_MOTION_CAL_DONE;

    int y = pageHeight / 2 - 60;
    for (const std::string& l : renderer.wrappedText(UI_12_FONT_ID, I18N.get(what), pageWidth - 60, 6)) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, l.c_str(), true, EpdFontFamily::BOLD);
      y += 40;
    }
    if (calFailed) {
      y += 20;
      for (const std::string& l : renderer.wrappedText(UI_10_FONT_ID, tr(STR_MOTION_CAL_RETRY), pageWidth - 60, 6)) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, l.c_str());
        y += 30;
      }
    }
    if (state != CAL_DONE) {
      const MotionInput::Reading& r = MOTION.reading();
      snprintf(line, sizeof(line), "X %+5d    Y %+5d    Z %+5d", static_cast<int>(r.x * 1000),
               static_cast<int>(r.y * 1000), static_cast<int>(r.n * 1000));
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 200, line);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == LIVE ? tr(STR_MOTION_CALIBRATE) : tr(STR_SELECT),
                                            "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Los números cambian solos: cada tanto hay que limpiar el papel o quedan
  // fantasmas de los dígitos viejos.
  if (++partialCount >= PARTIALS_BEFORE_CLEAN) {
    partialCount = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
  lastPaint = millis();
}
