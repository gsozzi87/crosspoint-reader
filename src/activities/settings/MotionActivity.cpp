#include "MotionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/SevenSegment.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// El panel tarda medio segundo por refresco: los números se repintan como mucho
// dos veces por segundo, y sólo si cambiaron lo suficiente para verse.
constexpr unsigned long PAINT_MS = 500;
// Debajo de esto es el ruido del propio sensor apoyado en la mesa: repintar por
// eso sería un fantasma cada medio segundo sin que nadie haya movido nada.
constexpr float REPAINT_G = 0.020f;    // 20 mg
constexpr float REPAINT_DPS = 5.0f;

constexpr int SIDE = 24;   // único margen lateral
constexpr int GAP = 8;     // aire entre bloques (grilla de 8)

// Fondo de escala de los medidores. Con ±2000 mg la gravedad cae justo a media
// escala, que es la referencia que uno tiene en la cabeza mirando la pantalla.
constexpr float ACCEL_FS_MG = 2000.0f;
constexpr float GYRO_FS_DPS = 250.0f;

// Los umbrales que muestra la grilla de abajo salen de MotionInput.h, no de
// literales de acá: si se afinan allá y acá quedaran copiados, la pantalla
// mentiría justo cuando se la usa para decidir si afinarlos.

constexpr int METER_H = 18;       // 18 y no 8: a 8 px es un hilo
constexpr int METER_FRAME = 2;
constexpr int METER_LABEL_W = 40;  // "ax"
constexpr int METER_VALUE_W = 88;  // "+1006"
constexpr int METER_CAP = 3;       // punta maciza de la barra
// Aire entre medidores. Vale GAP a propósito: así el aire entre dos medidores y
// el aire entre el último medidor y la sección siguiente son el mismo, todo cae
// en la grilla de 8, y las 12 px que se ganan contra los 10 de antes son las que
// permiten que la fila de calibrar entre en dos renglones de UI_10 en ruso.
constexpr int METER_GAP = GAP;

void drawRightText(const GfxRenderer& r, const int fontId, const int right, const int y, const char* text,
                   const bool bold = false) {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  r.drawText(fontId, right - r.getTextWidth(fontId, text, style), y, text, true, style);
}

void drawCenteredIn(const GfxRenderer& r, const int fontId, const int centerX, const int y, const char* text,
                    const bool bold = false) {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  r.drawText(fontId, centerX - r.getTextWidth(fontId, text, style) / 2, y, text, true, style);
}

// Medidor bipolar: el cero en el medio y la barra crece hacia el signo del
// valor. La barra va tramada (nada macizo grande, que fantasmea) con una punta
// maciza de 3 px, que es lo que deja ver dónde termina.
void drawMeter(const GfxRenderer& r, const int x, const int y, const int w, const int h, const float value,
               const float fullScale, const bool valid) {
  r.drawRect(x, y, w, h, METER_FRAME, true);
  const int innerX = x + METER_FRAME;
  const int innerY = y + METER_FRAME;
  const int innerW = w - 2 * METER_FRAME;
  const int innerH = h - 2 * METER_FRAME;
  const int centerX = innerX + innerW / 2;

  if (valid && fullScale > 0.0f) {
    float f = value / fullScale;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    const int span = static_cast<int>(f * static_cast<float>(innerW / 2));
    if (span != 0) {
      const int barX = span > 0 ? centerX : centerX + span;
      const int barW = span > 0 ? span : -span;
      r.fillRectDither(barX, innerY, barW, innerH, Color::LightGray);
      const int capW = barW < METER_CAP ? barW : METER_CAP;
      r.fillRect(span > 0 ? barX + barW - capW : barX, innerY, capW, innerH, true);
    }
  }
  // Marca del cero, de arriba a abajo del marco.
  r.fillRect(centerX - 1, y, 2, h, true);
}

// Número de `digits` cifras con los segmentos del temporizador, sin ceros a la
// izquierda. Devuelve el ancho que ocupó.
int drawSegNumber(const GfxRenderer& r, int value, const int digits, const int x, const int y, const int dw,
                  const int dh, const int t, const int gap) {
  if (value < 0) value = 0;
  int div = 1;
  for (int i = 1; i < digits; ++i) div *= 10;
  bool leading = true;
  int cx = x;
  for (int i = 0; i < digits; ++i, div /= 10) {
    const int d = (value / div) % 10;
    if (d != 0 || div == 1) leading = false;
    if (!leading) sevenseg::digit(r, d, cx, y, dw, dh, t);
    cx += dw + gap;
  }
  return cx - gap - x;
}

// Nombre traducido del gesto. MotionInput::name() devuelve una etiqueta fija
// para el log; lo que ve el usuario pasa por los yaml como todo lo demás.
StrId eventLabel(const MotionInput::Event e) {
  switch (e) {
    case MotionInput::Event::TiltLeft: return StrId::STR_MOTION_EV_TILT_LEFT;
    case MotionInput::Event::TiltRight: return StrId::STR_MOTION_EV_TILT_RIGHT;
    case MotionInput::Event::TiltForward: return StrId::STR_MOTION_EV_TILT_FWD;
    case MotionInput::Event::TiltBack: return StrId::STR_MOTION_EV_TILT_BACK;
    case MotionInput::Event::Shake: return StrId::STR_MOTION_EV_SHAKE;
    case MotionInput::Event::Rotate: return StrId::STR_MOTION_EV_ROTATE;
    case MotionInput::Event::Level: return StrId::STR_MOTION_EV_LEVEL;
    case MotionInput::Event::FaceDown: return StrId::STR_MOTION_EV_FACE_DOWN;
    case MotionInput::Event::FaceUp: return StrId::STR_MOTION_EV_FACE_UP;
    case MotionInput::Event::DoubleTap: return StrId::STR_MOTION_EV_TAP;
    case MotionInput::Event::None: break;
  }
  return StrId::STR_MOTION_TEST;
}
}  // namespace

void MotionActivity::onEnter() {
  Activity::onEnter();
  state = LIVE;
  calFailed = false;
  shown = MotionInput::Event::None;
  for (int& c : counters) c = 0;
  painted = MotionInput::Reading{};
  // El giróscopo se enciende solo mientras esta pantalla está abierta: acá se
  // quiere ver todo, incluido el giro, y son cinco veces más consumo.
  MOTION.setGyro(true);
  // Con los gestos apagados el chip no se sondea: acá se enciende igual, o esta
  // pantalla no puede diagnosticar nada.
  MOTION.setDiagnostics(true);
  requestUpdate();
}

void MotionActivity::onExit() {
  MOTION.setDiagnostics(false);
  MOTION.setGyro(false);
  if (HUB_STORE.imuMap.calibrated) HUB_STORE.saveToFile();
  Activity::onExit();
}

void MotionActivity::countEvent(const MotionInput::Event e) {
  switch (e) {
    case MotionInput::Event::TiltLeft:
    case MotionInput::Event::TiltRight:
    case MotionInput::Event::TiltForward:
    case MotionInput::Event::TiltBack: ++counters[CNT_TILT]; break;
    case MotionInput::Event::Shake: ++counters[CNT_SHAKE]; break;
    case MotionInput::Event::Rotate: ++counters[CNT_ROTATE]; break;
    case MotionInput::Event::Level: ++counters[CNT_LEVEL]; break;
    case MotionInput::Event::DoubleTap: ++counters[CNT_TAP]; break;
    // Boca abajo y boca arriba se ven en el renglón del último gesto: no
    // tienen contador propio porque la grilla es de cinco celdas.
    default: break;
  }
}

bool MotionActivity::worthRepainting() const {
  const MotionInput::Reading& r = MOTION.reading();
  if (!r.valid) return false;
  if (fabsf(r.x - painted.x) > REPAINT_G || fabsf(r.y - painted.y) > REPAINT_G ||
      fabsf(r.n - painted.n) > REPAINT_G || fabsf(r.magnitude - painted.magnitude) > REPAINT_G) {
    return true;
  }
  if (MOTION.gyroOn() && (fabsf(r.gx - painted.gx) > REPAINT_DPS || fabsf(r.gy - painted.gy) > REPAINT_DPS ||
                          fabsf(r.gn - painted.gn) > REPAINT_DPS)) {
    return true;
  }
  return false;
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
        countEvent(shown);
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

  // Regla del panel: como mucho un repintado cada medio segundo, y sólo si algo
  // se movió más que el ruido del sensor. Quieto sobre la mesa no gasta tinta.
  // Vale para el instrumento y para los pasos de calibración, que también
  // muestran los ejes en vivo; en CAL_DONE ya no hay nada que se mueva.
  if (millis() - lastPaint < PAINT_MS || state == CAL_DONE) return;
  if (worthRepainting()) requestUpdate();
}

int MotionActivity::drawSection(const int y, const int width, const char* label, const char* right) const {
  const int hLabel = renderer.getTextHeight(UI_10_FONT_ID);
  const int hSmall = renderer.getTextHeight(SMALL_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, SIDE, y, label, true, EpdFontFamily::BOLD);
  if (right != nullptr && *right != '\0') {
    drawRightText(renderer, SMALL_FONT_ID, width - SIDE, y + hLabel - hSmall, right);
  }
  // Las superficies se separan con una regla de 1 px, no con marcos ni áreas
  // rellenas: no cuesta tinta y no fantasmea.
  renderer.fillRect(SIDE, y + hLabel + 4, width - 2 * SIDE, 1, true);
  return y + hLabel + 4 + GAP;
}

int MotionActivity::drawMeterRow(const int y, const int width, const char* label, const float value,
                                 const float fullScale, const bool valid) const {
  const int hValue = renderer.getTextHeight(UI_10_FONT_ID);
  const int textY = y + (METER_H - hValue) / 2;
  renderer.drawText(UI_10_FONT_ID, SIDE, textY, label);

  char buf[16];
  if (valid) {
    snprintf(buf, sizeof(buf), "%+d", static_cast<int>(lroundf(value)));
  } else {
    snprintf(buf, sizeof(buf), "%s", "--");
  }
  drawRightText(renderer, UI_10_FONT_ID, SIDE + METER_LABEL_W + METER_VALUE_W, textY, buf, true);

  const int meterX = SIDE + METER_LABEL_W + METER_VALUE_W + 12;
  drawMeter(renderer, meterX, y, width - SIDE - meterX, METER_H, value, fullScale, valid);
  return y + METER_H + METER_GAP;
}

void MotionActivity::renderLive(const int width, const int bottom) const {
  const MotionInput::Reading& r = MOTION.reading();
  const bool gestures = HUB_STORE.motionGestures;
  const int hUi14 = renderer.getTextHeight(UI_14_FONT_ID);
  const int hUi10 = renderer.getTextHeight(UI_10_FONT_ID);
  const int hSmall = renderer.getTextHeight(SMALL_FONT_ID);
  const int right = width - SIDE;
  char buf[96];

  int y = SIDE + hUi14 + GAP + 1 + GAP;  // debajo de la regla del encabezado

  // --- Visor: magnitud y estado -------------------------------------------
  constexpr int VISOR_H = 100;
  renderer.drawRoundedRect(SIDE, y, width - 2 * SIDE, VISOR_H, 2, 10, true);
  const int inLeft = SIDE + 16;
  const int inRight = right - 16;
  renderer.drawText(SMALL_FONT_ID, inLeft, y + 12, tr(STR_MOTION_MAGNITUDE), true, EpdFontFamily::BOLD);
  int mg = r.valid ? static_cast<int>(lroundf(r.magnitude * 1000.0f)) : 0;
  if (mg < 0) mg = 0;
  if (mg > 9999) mg = 9999;
  const int segY = y + 36;
  const int segW = drawSegNumber(renderer, mg, 4, inLeft, segY, 30, 48, 5, 7);
  const int mgLabelX = inLeft + segW + 8;
  renderer.drawText(SMALL_FONT_ID, mgLabelX, segY + 48 - hSmall, "mg", true, EpdFontFamily::BOLD);

  // El estado va a la derecha, a la altura de los dígitos: lo que sobra después
  // del visor es todo el lugar que hay, y en alemán o en ruso estas frases son
  // bastante más largas que en español.
  const int stateW = inRight - (mgLabelX + renderer.getTextWidth(SMALL_FONT_ID, "mg", EpdFontFamily::BOLD) + 12);
  drawRightText(renderer, SMALL_FONT_ID, inRight, y + 12, tr(STR_MOTION_GESTURES), true);
  drawRightText(renderer, UI_14_FONT_ID, inRight, y + 34,
                renderer
                    .truncatedText(UI_14_FONT_ID,
                                   I18N.get(gestures ? StrId::STR_MOTION_ON : StrId::STR_MOTION_OFF), stateW,
                                   EpdFontFamily::BOLD)
                    .c_str(),
                true);
  drawRightText(
      renderer, SMALL_FONT_ID, inRight, y + 70,
      renderer
          .truncatedText(SMALL_FONT_ID,
                         I18N.get(MOTION.tapTrusted() ? StrId::STR_MOTION_TAP_ON : StrId::STR_MOTION_TAP_OFF),
                         stateW)
          .c_str());
  y += VISOR_H + GAP;

  // --- Acelerómetro --------------------------------------------------------
  snprintf(buf, sizeof(buf), "±%d mg", static_cast<int>(ACCEL_FS_MG));
  y = drawSection(y, width, tr(STR_MOTION_ACCEL), buf);
  y = drawMeterRow(y, width, "ax", r.x * 1000.0f, ACCEL_FS_MG, r.valid);
  y = drawMeterRow(y, width, "ay", r.y * 1000.0f, ACCEL_FS_MG, r.valid);
  y = drawMeterRow(y, width, "an", r.n * 1000.0f, ACCEL_FS_MG, r.valid);
  // drawMeterRow ya dejó METER_GAP == GAP de aire: entre bloques no hace falta más.

  // --- Giroscopio ----------------------------------------------------------
  snprintf(buf, sizeof(buf), "±%d dps", static_cast<int>(GYRO_FS_DPS));
  y = drawSection(y, width, tr(STR_MOTION_GYRO), buf);
  const bool gyro = r.valid && MOTION.gyroOn();
  y = drawMeterRow(y, width, "gx", r.gx, GYRO_FS_DPS, gyro);
  y = drawMeterRow(y, width, "gy", r.gy, GYRO_FS_DPS, gyro);
  y = drawMeterRow(y, width, "gn", r.gn, GYRO_FS_DPS, gyro);

  // --- Evento y contadores -------------------------------------------------
  y = drawSection(y, width, tr(STR_MOTION_EVENT), nullptr);
  if (shown != MotionInput::Event::None) {
    // El gesto se muestra aunque los gestos estén apagados en Ajustes: acá el
    // sensor se lee igual y el motor de eventos corre, así que esta pantalla
    // sirve para probarlos ANTES de encenderlos.
    renderer.drawText(UI_14_FONT_ID, SIDE,
                      y, renderer.truncatedText(UI_14_FONT_ID, I18N.get(eventLabel(shown)), width - 2 * SIDE,
                                                EpdFontFamily::BOLD)
                             .c_str(),
                      true, EpdFontFamily::BOLD);
  } else {
    const char* note = gestures ? tr(STR_MOTION_TEST) : tr(STR_MOTION_OFF_NOTE);
    renderer.drawText(UI_10_FONT_ID, SIDE, y + (hUi14 - hUi10) / 2,
                      renderer.truncatedText(UI_10_FONT_ID, note, width - 2 * SIDE).c_str());
  }
  y += hUi14 + GAP;

  {
    // Grilla de contadores: cinco celdas separadas por reglas de 1 px, la
    // etiqueta arriba en SMALL y el número grande abajo.
    //
    // Las etiquetas tienen claves propias y ABREVIADAS: la celda da 86 px y las
    // largas de la tabla de umbrales no entran ("Horizontal" son 79 px y salía
    // "Horizon…" ya en español; en alemán "Waagerecht" son 93). Justo la celda
    // que hay que mirar para saber si el gesto de horizontal se reconoce era la
    // que quedaba cortada.
    static const StrId CNT_LABELS[CNT_COUNT] = {StrId::STR_MOTION_CNT_TILT, StrId::STR_MOTION_CNT_SHAKE,
                                                StrId::STR_MOTION_CNT_ROTATE, StrId::STR_MOTION_CNT_LEVEL,
                                                StrId::STR_MOTION_CNT_TAP};
    const int cellW = (width - 2 * SIDE) / CNT_COUNT;
    const int numY = y + hSmall + 4;
    for (int i = 0; i < CNT_COUNT; ++i) {
      const int cellX = SIDE + i * cellW;
      const int centerX = cellX + cellW / 2;
      if (i > 0) renderer.fillRect(cellX, y - 2, 1, hSmall + 4 + hUi14 + 4, true);
      drawCenteredIn(renderer, SMALL_FONT_ID, centerX,
                     y, renderer.truncatedText(SMALL_FONT_ID, I18N.get(CNT_LABELS[i]), cellW - 4).c_str());
      snprintf(buf, sizeof(buf), "%d", counters[i]);
      drawCenteredIn(renderer, UI_14_FONT_ID, centerX, numY, buf, true);
    }
    y = numY + hUi14 + GAP;
  }

  // --- Montaje -------------------------------------------------------------
  const auto& m = HUB_STORE.imuMap;
  snprintf(buf, sizeof(buf), "x=%c%c · y=%c%c", m.xSign < 0 ? '-' : '+', static_cast<char>('X' + m.xAxis),
           m.ySign < 0 ? '-' : '+', static_cast<char>('X' + m.yAxis));
  y = drawSection(y, width, tr(STR_MOTION_MOUNT), buf);
  snprintf(buf, sizeof(buf), "%c%c · %s", m.normalSign < 0 ? '-' : '+', static_cast<char>('X' + m.normalAxis),
           m.calibrated ? tr(STR_MOTION_CALIBRATED) : tr(STR_MOTION_FACTORY));
  // El valor manda: en alemán y en ruso la etiqueta ocupa media pantalla, así
  // que se recorta contra lo que deja libre el "-Z · calibrado" y no al revés.
  const int valueW = renderer.getTextWidth(UI_10_FONT_ID, buf, EpdFontFamily::BOLD);
  renderer.drawText(
      UI_10_FONT_ID, SIDE, y,
      renderer.truncatedText(UI_10_FONT_ID, tr(STR_MOTION_NORMAL_AXIS), width - 2 * SIDE - valueW - 12).c_str());
  drawRightText(renderer, UI_10_FONT_ID, right, y, buf, true);
  y += hUi10 + 4;
  // La fila de calibrar es la instrucción de la pantalla y no puede salir
  // cortada: en ruso no entra en un renglón ni en UI_10 (548 px) ni en SMALL
  // (448 contra los 432 útiles), y el recorte se comía justo el "presiona OK".
  // Se prueba cuerpo y cantidad de renglones de mayor a menor y se usa el
  // primero que entre ENTERO, dentro de lo que sobra después de reservar la
  // grilla de umbrales: así el renglón nunca la empuja fuera del panel (la
  // grilla se corta sola con su `break` y la última fila desaparecía sin aviso).
  const int textW = width - 2 * SIDE;
  const int rowStep = hUi10 + 4;  // 24: cae en la grilla de 8 y deja los 4 px del bajo
  const int thBlockH = hUi10 + 4 + GAP + 2 * rowStep + hUi10;  // encabezado + tres filas
  const int calBudget = bottom - y - thBlockH - GAP;
  const char* calRow = tr(STR_MOTION_CAL_ROW);
  const int CAL_FONTS[2] = {UI_10_FONT_ID, SMALL_FONT_ID};
  int calFont = SMALL_FONT_ID;
  int calStep = renderer.getTextHeight(SMALL_FONT_ID) + 4;
  std::vector<std::string> calLines;
  for (const int f : CAL_FONTS) {
    const int h = renderer.getTextHeight(f);
    for (int n = 1; n <= 2; ++n) {
      if ((n - 1) * (h + 4) + h > calBudget) continue;
      std::vector<std::string> lines = renderer.wrappedText(f, calRow, textW, n);
      if (static_cast<int>(lines.size()) > n) continue;
      bool cut = false;
      for (const std::string& l : lines) {
        if (l.find("\xe2\x80\xa6") != std::string::npos) cut = true;  // U+2026
      }
      if (cut) continue;
      calFont = f;
      calStep = h + 4;
      calLines = std::move(lines);
      break;
    }
    if (!calLines.empty()) break;
  }
  // Red de seguridad: si ni dos renglones de SMALL entran (una traducción
  // desmedida), vale más recortar que pisar los umbrales.
  if (calLines.empty()) calLines.push_back(renderer.truncatedText(SMALL_FONT_ID, calRow, textW));
  for (const std::string& l : calLines) {
    renderer.drawText(calFont, SIDE, y, l.c_str());
    y += calStep;
  }
  y += GAP - 4;  // el último renglón ya sumó su propio interlineado

  // --- Umbrales ------------------------------------------------------------
  y = drawSection(y, width, tr(STR_MOTION_THRESHOLDS), nullptr);
  struct Threshold {
    StrId label;
    int value;
    const char* unit;
  };
  const Threshold TH[6] = {
      {StrId::STR_MOTION_TH_TILT, MotionInput::TH_TILT_MG, "mg"},     {StrId::STR_MOTION_TH_LEVEL, MotionInput::TH_LEVEL_MG, "mg"},
      {StrId::STR_MOTION_TH_SHAKE, MotionInput::TH_SHAKE_MG, "mg"},   {StrId::STR_MOTION_TH_STILL, MotionInput::TH_STILL_MG, "mg"},
      {StrId::STR_MOTION_TH_ROTATE, MotionInput::TH_ROTATE_DPS, "dps"}, {StrId::STR_MOTION_TH_DEBOUNCE, MotionInput::TH_DEBOUNCE_MS, "ms"},
  };
  const int colW = (width - 2 * SIDE - 24) / 2;
  const int gridTop = y;
  for (int i = 0; i < 6; ++i) {
    const int col = i % 2;
    const int rowY = y + (i / 2) * rowStep;
    if (rowY + hUi10 > bottom) break;
    const int colX = SIDE + col * (colW + 24);
    renderer.drawText(UI_10_FONT_ID, colX,
                      rowY, renderer.truncatedText(UI_10_FONT_ID, I18N.get(TH[i].label), colW - 70).c_str());
    snprintf(buf, sizeof(buf), "%d %s", TH[i].value, TH[i].unit);
    drawRightText(renderer, UI_10_FONT_ID, colX + colW, rowY, buf, true);
  }
  // Regla vertical entre las dos columnas, del alto de la grilla.
  const int gridBottom = gridTop + 3 * rowStep - 6;
  if (gridBottom <= bottom) renderer.fillRect(SIDE + colW + 12, gridTop, 1, gridBottom - gridTop, true);
}

void MotionActivity::renderCalibration(const int width, const int height, const int bottom) const {
  // Calibración: una consigna grande por paso y nada más, que es lo que se
  // puede leer con el aparato inclinado en la mano.
  StrId what = StrId::STR_MOTION_CAL_FLAT;
  if (state == CAL_RIGHT) what = StrId::STR_MOTION_CAL_RIGHT;
  else if (state == CAL_TOWARD) what = StrId::STR_MOTION_CAL_TOWARD;
  else if (state == CAL_DONE) what = StrId::STR_MOTION_CAL_DONE;

  const int hUi14 = renderer.getTextHeight(UI_14_FONT_ID);
  const int hUi10 = renderer.getTextHeight(UI_10_FONT_ID);
  const int step14 = hUi14 + 12;
  const auto lines = renderer.wrappedText(UI_14_FONT_ID, I18N.get(what), width - 2 * SIDE, 4, EpdFontFamily::BOLD);
  int y = height / 2 - static_cast<int>(lines.size()) * step14 / 2 - 40;
  for (const std::string& l : lines) {
    renderer.drawCenteredText(UI_14_FONT_ID, y, l.c_str(), true, EpdFontFamily::BOLD);
    y += step14;
  }
  if (calFailed) {
    y += 12;
    for (const std::string& l : renderer.wrappedText(UI_10_FONT_ID, tr(STR_MOTION_CAL_RETRY), width - 2 * SIDE, 3)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, l.c_str());
      y += hUi10 + 8;
    }
  }
  if (state != CAL_DONE) {
    // Los ejes en vivo mientras se apunta, con las mismas letras que los
    // medidores: es lo que dice si la posición que pide la consigna ya está
    // tomada, sin tener que adivinar cuándo apretar OK.
    const MotionInput::Reading& r = MOTION.reading();
    char buf[64];
    snprintf(buf, sizeof(buf), "x %+5d   y %+5d   n %+5d", static_cast<int>(lroundf(r.x * 1000.0f)),
             static_cast<int>(lroundf(r.y * 1000.0f)), static_cast<int>(lroundf(r.n * 1000.0f)));
    // Pegado a la barra de botones, no a un número mágico: la altura de los
    // hints la fija el tema y acá el renglón tiene que quedar arriba de ella.
    const int axisY = bottom - hUi10 - GAP;
    renderer.fillRect(SIDE, axisY - 16, width - 2 * SIDE, 1, true);
    renderer.drawCenteredText(UI_10_FONT_ID, axisY, buf);
  }
}

void MotionActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int bottom = height - metrics.buttonHintsHeight - GAP;

  renderer.clearScreen();

  // Encabezado propio y compacto: el título en UI_14 y una regla de 1 px. El
  // encabezado del tema son 89 px con la batería, y acá cada píxel de alto es
  // un medidor que se ve o no se ve.
  const int hUi14 = renderer.getTextHeight(UI_14_FONT_ID);
  const int hSmall = renderer.getTextHeight(SMALL_FONT_ID);
  renderer.drawText(UI_14_FONT_ID, SIDE, SIDE, tr(STR_MOTION_TITLE), true, EpdFontFamily::BOLD);
  char sub[32];
  snprintf(sub, sizeof(sub), "QMI8658 · %lu ms", MotionInput::POLL_MS);
  drawRightText(renderer, SMALL_FONT_ID, width - SIDE, SIDE + hUi14 - hSmall, sub);
  renderer.fillRect(SIDE, SIDE + hUi14 + GAP, width - 2 * SIDE, 1, true);

  if (!MOTION.available()) {
    renderer.drawCenteredText(UI_14_FONT_ID, height / 2, tr(STR_MOTION_NO_IMU), true, EpdFontFamily::BOLD);
  } else if (state == LIVE) {
    renderLive(width, bottom);
  } else {
    renderCalibration(width, height, bottom);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), state == LIVE ? tr(STR_MOTION_CALIBRATE) : tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de limpieza del panel la lleva el coordinador de refresco, que
  // además se saltea el refresco entero si el frame salió idéntico al anterior
  // (el aparato quieto sobre la mesa no gasta tinta).
  renderer.displayBuffer();
  lastPaint = millis();
  painted = MOTION.reading();
}
