#include "SimonActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN_X = 24;
constexpr int GAP = 20;

// Figura de cada casillero. Sin colores: la pantalla es blanco y negro.
enum PadFigure { FIG_CIRCLE = 0, FIG_SQUARE, FIG_TRIANGLE, FIG_DIAMOND };
}  // namespace

uint16_t SimonActivity::bestLevel = 0;

// ----------------------------------------------------------------- partida --

void SimonActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::READY;
  length = 0;
  score = 0;
  cursor = 0;
  pressed = -1;
  won = false;
  forceClean = true;
  requestUpdate();
}

void SimonActivity::startGame() {
  length = 0;
  score = 0;
  cursor = 0;
  pressed = -1;
  won = false;
  addStep();
  beginShow();
}

// Un paso nuevo. Se evita que la misma figura salga tres veces seguidas: en
// tinta electrónica esa repetición se lee como un solo destello.
void SimonActivity::addStep() {
  if (length >= MAX_LEN) return;
  int next = static_cast<int>(random(PAD_COUNT));
  if (length >= 2 && sequence[static_cast<size_t>(length - 1)] == sequence[static_cast<size_t>(length - 2)] &&
      sequence[static_cast<size_t>(length - 1)] == static_cast<uint8_t>(next)) {
    next = (next + 1 + static_cast<int>(random(PAD_COUNT - 1))) % PAD_COUNT;
  }
  sequence[static_cast<size_t>(length)] = static_cast<uint8_t>(next);
  ++length;
}

void SimonActivity::beginShow() {
  state = State::SHOW;
  showIndex = 0;
  padLit = false;  // arranca con la pausa de entrada, para dar tiempo a mirar
  stepAt = millis();
  forceClean = true;
  requestUpdate();
}

// La secuencia crece y los tiempos se acortan, pero nunca por debajo del piso
// que necesita el panel para que el destello se vea.
unsigned long SimonActivity::onMs() const {
  const unsigned long step = static_cast<unsigned long>(length) * 20UL;
  return ON_START_MS > ON_MIN_MS + step ? ON_START_MS - step : ON_MIN_MS;
}

unsigned long SimonActivity::offMs() const {
  const unsigned long step = static_cast<unsigned long>(length) * 10UL;
  return OFF_START_MS > OFF_MIN_MS + step ? OFF_START_MS - step : OFF_MIN_MS;
}

// Un paso del reproductor de la secuencia: encender, apagar, siguiente.
void SimonActivity::advanceShow() {
  stepAt = millis();
  if (padLit) {
    padLit = false;
    ++showIndex;
    if (showIndex >= length) {
      // Se terminó de mostrar: turno del jugador.
      state = State::PLAYING;
      inputIndex = 0;
      cursor = 0;
      pressed = -1;
      forceClean = true;
    }
  } else {
    padLit = true;
  }
  requestUpdate();
}

void SimonActivity::pressPad() {
  pressed = cursor;
  pressedOk = sequence[static_cast<size_t>(inputIndex)] == static_cast<uint8_t>(cursor);
  state = State::FLASH;
  stepAt = millis();
  requestUpdate();
}

// Terminado el destello de devolución se resuelve lo elegido.
void SimonActivity::resolveFlash() {
  pressed = -1;
  if (!pressedOk) {
    endGame(false);
    return;
  }
  ++score;
  ++inputIndex;
  if (inputIndex < length) {
    state = State::PLAYING;
    requestUpdate();
    return;
  }
  // Ronda entera acertada.
  if (length > static_cast<int>(bestLevel)) bestLevel = static_cast<uint16_t>(length);
  if (length >= MAX_LEN) {
    endGame(true);
    return;
  }
  state = State::ROUND;
  stepAt = millis();
  forceClean = true;
  requestUpdate();
}

void SimonActivity::endGame(const bool victory) {
  won = victory;
  state = State::OVER;
  pressed = -1;
  if (length > static_cast<int>(bestLevel)) bestLevel = static_cast<uint16_t>(length);
  if (!victory) {
    beeping = beep.start();
    beepSince = millis();
  }
  forceClean = true;
  requestUpdate();
}

// -------------------------------------------------------------------- loop --

void SimonActivity::loop() {
  // Atrás mantenido: abandona la partida y vuelve al inicio. Consume la suelta,
  // así que no dispara también la salida.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    if (beeping) {
      beep.stop();
      beeping = false;
    }
    if (state != State::READY) {
      state = State::READY;
      length = 0;
      score = 0;
      pressed = -1;
      forceClean = true;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (beeping) {
      beep.stop();
      beeping = false;
    }
    finish();
    return;
  }

  // El pitido del error se corta solo: nada de delay() ni de sonar para siempre.
  if (beeping && millis() - beepSince >= WRONG_BEEP_MS) {
    beep.stop();
    beeping = false;
  }

  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  switch (state) {
    case State::READY:
      if (confirm) startGame();
      break;

    case State::SHOW:
      // Mientras muestra no se acepta nada: hay que mirar.
      if (millis() - stepAt >= (padLit ? onMs() : (showIndex == 0 ? LEAD_IN_MS : offMs()))) advanceShow();
      break;

    case State::PLAYING:
      buttonNavigator.onNext([this] {
        cursor = ButtonNavigator::nextIndex(cursor, PAD_COUNT);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        cursor = ButtonNavigator::previousIndex(cursor, PAD_COUNT);
        requestUpdate();
      });
      if (confirm) pressPad();
      break;

    case State::FLASH:
      if (millis() - stepAt >= FLASH_MS) resolveFlash();
      break;

    case State::ROUND:
      if (millis() - stepAt >= ROUND_MS) {
        addStep();
        beginShow();
      }
      break;

    case State::OVER:
      if (confirm) {
        if (beeping) {
          beep.stop();
          beeping = false;
        }
        startGame();
      }
      break;
  }
}

// ------------------------------------------------------------------ dibujo --

void SimonActivity::fillCircle(const int cx, const int cy, const int r, const bool ink) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, ink);
  }
}

void SimonActivity::fillDiamond(const int cx, const int cy, const int r, const bool ink) const {
  for (int dy = -r; dy <= r; ++dy) {
    const int w = r - (dy < 0 ? -dy : dy);
    renderer.fillRect(cx - w, cy + dy, 2 * w + 1, 1, ink);
  }
}

// Cada figura entra en un cuadrado de lado 2r centrado en (cx, cy). `ink` es el
// color de la figura: negra sobre el casillero apagado, blanca sobre el encendido.
void SimonActivity::drawFigure(const int pad, const int cx, const int cy, const int r, const bool ink) const {
  switch (pad) {
    case FIG_CIRCLE:
      fillCircle(cx, cy, r, ink);
      break;

    case FIG_SQUARE:
      renderer.fillRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, ink);
      break;

    case FIG_TRIANGLE: {
      const int xs[3] = {cx, cx + r, cx - r};
      const int ys[3] = {cy - r, cy + r, cy + r};
      renderer.fillPolygon(xs, ys, 3, ink);
      break;
    }

    case FIG_DIAMOND:
    default:
      fillDiamond(cx, cy, r, ink);
      break;
  }
}

void SimonActivity::drawPad(const int idx, const int x, const int y, const int size, const bool lit,
                            const bool showCursor) const {
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  int r = size / 2 - 26;
  if (r < 10) r = 10;

  if (lit) {
    renderer.fillRoundedRect(x, y, size, size, 14, Color::Black);
    drawFigure(idx, cx, cy, r, false);
  } else {
    renderer.drawRoundedRect(x, y, size, size, 3, 14, true);
    drawFigure(idx, cx, cy, r, true);
  }

  // Cursor: un marco por fuera, así se distingue del casillero encendido.
  if (showCursor) renderer.drawRoundedRect(x - 6, y - 6, size + 12, size + 12, 3, 18, true);
}

void SimonActivity::drawInfoBar(const int y) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[48];

  if (length == 0) {
    if (bestLevel > 0) {
      snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestLevel));
      renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    }
    return;
  }

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_LEVEL), length);
  renderer.drawText(UI_10_FONT_ID, MARGIN_X, y, buf);

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);

  snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestLevel));
  renderer.drawText(UI_10_FONT_ID, pageWidth - MARGIN_X - renderer.getTextWidth(UI_10_FONT_ID, buf), y, buf);
}

void SimonActivity::drawBoard(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[64];

  drawInfoBar(top + 6);

  const int areaTop = top + 40;
  const int areaBottom = bottom - 54;  // la línea de estado va abajo del tablero
  const int byWidth = (pageWidth - 2 * MARGIN_X - GAP) / 2;
  const int byHeight = (areaBottom - areaTop - GAP) / 2;
  int size = byWidth < byHeight ? byWidth : byHeight;
  if (size < 40) size = 40;

  const int gridW = 2 * size + GAP;
  const int gridH = 2 * size + GAP;
  const int left = (pageWidth - gridW) / 2;
  const int gridTop = areaTop + (areaBottom - areaTop - gridH) / 2;

  // Qué casillero está encendido en este momento.
  int litPad = -1;
  if (state == State::SHOW && padLit && showIndex < length) litPad = sequence[static_cast<size_t>(showIndex)];
  if (state == State::FLASH) litPad = pressed;

  for (int i = 0; i < PAD_COUNT; ++i) {
    const int x = left + (i % 2) * (size + GAP);
    const int y = gridTop + (i / 2) * (size + GAP);
    drawPad(i, x, y, size, i == litPad, state == State::PLAYING && i == cursor);
  }

  // Línea de estado: qué toca hacer y por dónde va la secuencia.
  const int statusY = bottom - 40;
  switch (state) {
    case State::READY:
      renderer.drawCenteredText(UI_12_FONT_ID, statusY, I18N.get(StrId::STR_GAME_NEW), true, EpdFontFamily::BOLD);
      break;
    case State::SHOW:
      snprintf(buf, sizeof(buf), "%s  %d/%d", I18N.get(StrId::STR_GAME_WATCH), showIndex + (padLit ? 1 : 0), length);
      renderer.drawCenteredText(UI_12_FONT_ID, statusY, buf, true, EpdFontFamily::BOLD);
      break;
    case State::ROUND:
      renderer.drawCenteredText(UI_12_FONT_ID, statusY, I18N.get(StrId::STR_GAME_CORRECT), true, EpdFontFamily::BOLD);
      break;
    default:
      snprintf(buf, sizeof(buf), "%s  %d/%d", I18N.get(StrId::STR_GAME_REPEAT),
               inputIndex + (state == State::FLASH ? 1 : 0), length);
      renderer.drawCenteredText(UI_12_FONT_ID, statusY, buf, true, EpdFontFamily::BOLD);
      break;
  }
}

void SimonActivity::drawSummary(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[64];
  int y = top + (bottom - top) / 2 - 110;

  renderer.fillRoundedRect(MARGIN_X, y - 12, pageWidth - 2 * MARGIN_X, 58, 12, Color::Black);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 6, I18N.get(won ? StrId::STR_GAME_WON : StrId::STR_GAME_WRONG), false,
                            EpdFontFamily::BOLD);
  y += 76;

  renderer.drawCenteredText(UI_10_FONT_ID, y, I18N.get(StrId::STR_GAME_OVER));
  y += 40;

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_LEVEL), length);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += 34;

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += 34;

  snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestLevel));
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 40;

  // La secuencia que había que repetir, para ver dónde se cortó.
  const int shown = length < 12 ? length : 12;
  const int step = 34;
  int x = (pageWidth - (shown > 0 ? shown * step - (step - 26) : 0)) / 2;
  for (int i = 0; i < shown; ++i) {
    drawFigure(sequence[static_cast<size_t>(i)], x + 13, y + 13, 11, true);
    x += step;
  }
}

void SimonActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(StrId::STR_GAME_SIMON));

  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  if (state == State::OVER) {
    drawSummary(top, bottom);
  } else {
    drawBoard(top, bottom);
  }

  switch (state) {
    case State::READY: {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::OVER: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PLAYING:
    case State::FLASH: {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    default: {
      // Mientras muestra la secuencia no hay nada que tocar.
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  // Regla del panel: parcial rápido para cada destello y uno limpio cada tantos,
  // más los cambios de pantalla, que si no fantasmea.
  ++partialCount;
  bool clean = forceClean || partialCount >= PARTIALS_BEFORE_CLEAN;
  // Pero nunca un refresco limpio justo sobre un casillero encendido: el
  // parpadeo del panel se confundiría con el destello. Espera al hueco.
  if (state == State::SHOW && padLit && !forceClean) clean = false;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
