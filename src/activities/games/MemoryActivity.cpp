#include "MemoryActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Nivel -> forma de la grilla. 8, 10 y 15 parejas.
constexpr int COLS[] = {4, 4, 5};
constexpr int ROWS[] = {4, 5, 6};

enum Figure {
  FIG_CIRCLE = 0,
  FIG_SQUARE,
  FIG_TRIANGLE,
  FIG_PLUS,
  FIG_DIAMOND,
  FIG_STAR,
  FIG_MOON,
  FIG_HOURGLASS,
  FIG_CHECKER,
  FIG_SPIRAL,
  FIG_RING,
  FIG_XCROSS,
  FIG_ARROW,
  FIG_SUN,
  FIG_BOXES,
  FIG_HASH
};

// Estrella de cinco puntas: puntas y valles alternados, en milésimas del radio.
constexpr int STAR_X[10] = {0, 225, 951, 363, 588, 0, -588, -363, -951, -225};
constexpr int STAR_Y[10] = {-1000, -309, -309, 118, 809, 382, 809, 118, -309, -309};

// Ocho rayos del sol, en milésimas.
constexpr int RAY_X[8] = {1000, 707, 0, -707, -1000, -707, 0, 707};
constexpr int RAY_Y[8] = {0, 707, 1000, 707, 0, -707, -1000, -707};

// Espiral cuadrada: derecha, abajo, izquierda, arriba.
constexpr int SPIRAL_DX[4] = {1, 0, -1, 0};
constexpr int SPIRAL_DY[4] = {0, 1, 0, -1};
}  // namespace

std::array<uint16_t, 3> MemoryActivity::bestMoves{};
std::array<uint16_t, 3> MemoryActivity::bestSeconds{};

// ----------------------------------------------------------------- partida --

void MemoryActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::LEVEL;
  forceClean = true;
  requestUpdate();
}

void MemoryActivity::startGame() {
  const int cards = COLS[level] * ROWS[level];
  pairsTotal = cards / 2;

  // Se eligen `pairsTotal` figuras distintas barajando el catálogo entero.
  std::array<uint8_t, FIGURE_COUNT> pool{};
  for (int i = 0; i < FIGURE_COUNT; ++i) pool[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
  for (int i = FIGURE_COUNT - 1; i > 0; --i)
    std::swap(pool[static_cast<size_t>(i)], pool[static_cast<size_t>(random(i + 1))]);

  figures.assign(static_cast<size_t>(cards), 0);
  for (int p = 0; p < pairsTotal; ++p) {
    figures[static_cast<size_t>(2 * p)] = pool[static_cast<size_t>(p)];
    figures[static_cast<size_t>(2 * p + 1)] = pool[static_cast<size_t>(p)];
  }
  for (int i = cards - 1; i > 0; --i)
    std::swap(figures[static_cast<size_t>(i)], figures[static_cast<size_t>(random(i + 1))]);

  status.assign(static_cast<size_t>(cards), 0);
  cursor = 0;
  firstPick = -1;
  secondPick = -1;
  moves = 0;
  pairsFound = 0;
  finalSeconds = 0;
  startedAt = millis();
  state = State::PLAY;
  forceClean = true;
  requestUpdate();
}

bool MemoryActivity::selectable(const int idx) const {
  return idx >= 0 && idx < static_cast<int>(status.size()) && status[static_cast<size_t>(idx)] == 0;
}

// La palanca recorre solo las cartas tapadas, en orden de lectura y en círculo.
void MemoryActivity::moveCursor(const int delta) {
  const int total = static_cast<int>(status.size());
  if (total == 0) return;
  for (int step = 1; step <= total; ++step) {
    int idx = (cursor + delta * step) % total;
    if (idx < 0) idx += total;
    if (selectable(idx)) {
      cursor = idx;
      requestUpdate();
      return;
    }
  }
}

// Después de tapar o emparejar, el cursor puede haber quedado sobre una carta
// que ya no se puede elegir.
void MemoryActivity::normalizeCursor() {
  if (selectable(cursor)) return;
  const int total = static_cast<int>(status.size());
  for (int step = 1; step <= total; ++step) {
    const int idx = (cursor + step) % total;
    if (selectable(idx)) {
      cursor = idx;
      return;
    }
  }
}

void MemoryActivity::flipAtCursor() {
  if (!selectable(cursor)) return;
  status[static_cast<size_t>(cursor)] = 1;

  if (firstPick < 0) {
    firstPick = cursor;
    normalizeCursor();
    requestUpdate();
    return;
  }

  secondPick = cursor;
  ++moves;
  if (figures[static_cast<size_t>(firstPick)] == figures[static_cast<size_t>(secondPick)]) {
    status[static_cast<size_t>(firstPick)] = 2;
    status[static_cast<size_t>(secondPick)] = 2;
    firstPick = -1;
    secondPick = -1;
    ++pairsFound;
    if (pairsFound >= pairsTotal) {
      finalSeconds = elapsedSeconds();
      const size_t lv = static_cast<size_t>(level);
      const bool better = bestMoves[lv] == 0 || moves < bestMoves[lv] ||
                          (moves == static_cast<int>(bestMoves[lv]) && finalSeconds < bestSeconds[lv]);
      if (better) {
        bestMoves[lv] = static_cast<uint16_t>(moves);
        bestSeconds[lv] = static_cast<uint16_t>(finalSeconds > 9999 ? 9999 : finalSeconds);
      }
      state = State::WON;
      forceClean = true;
    } else {
      normalizeCursor();
    }
    requestUpdate();
    return;
  }

  // No coinciden: quedan a la vista un momento y se tapan solas.
  state = State::PEEK;
  peekSince = millis();
  requestUpdate();
}

void MemoryActivity::resolvePeek() {
  if (firstPick >= 0) status[static_cast<size_t>(firstPick)] = 0;
  if (secondPick >= 0) status[static_cast<size_t>(secondPick)] = 0;
  cursor = firstPick >= 0 ? firstPick : cursor;
  firstPick = -1;
  secondPick = -1;
  state = State::PLAY;
  normalizeCursor();
  requestUpdate();
}

unsigned long MemoryActivity::elapsedSeconds() const {
  if (state == State::WON) return finalSeconds;
  if (startedAt == 0) return 0;
  return (millis() - startedAt) / 1000UL;
}

// -------------------------------------------------------------------- loop --

void MemoryActivity::loop() {
  // Atrás mantenido: abandona la partida y vuelve a elegir nivel. Consume la
  // suelta, así que no dispara también la salida.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    if (state != State::LEVEL) {
      state = State::LEVEL;
      forceClean = true;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::PEEK && millis() - peekSince >= PEEK_MS) {
    resolvePeek();
    return;
  }

  if (state == State::LEVEL) {
    buttonNavigator.onNext([this] {
      level = ButtonNavigator::nextIndex(level, LEVEL_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      level = ButtonNavigator::previousIndex(level, LEVEL_COUNT);
      requestUpdate();
    });
  } else if (state == State::PLAY) {
    buttonNavigator.onNext([this] { moveCursor(1); });
    buttonNavigator.onPrevious([this] { moveCursor(-1); });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::LEVEL: startGame(); break;
      case State::PLAY: flipAtCursor(); break;
      case State::PEEK: resolvePeek(); break;  // no hace falta esperar
      case State::WON:
        state = State::LEVEL;
        forceClean = true;
        requestUpdate();
        break;
    }
  }
}

// ------------------------------------------------------------------ dibujo --

void MemoryActivity::fillCircle(const int cx, const int cy, const int r, const bool ink) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, ink);
  }
}

void MemoryActivity::fillDiamond(const int cx, const int cy, const int r, const bool ink) const {
  for (int dy = -r; dy <= r; ++dy) {
    const int w = r - (dy < 0 ? -dy : dy);
    renderer.fillRect(cx - w, cy + dy, 2 * w + 1, 1, ink);
  }
}

void MemoryActivity::fillTriangle(const int x1, const int y1, const int x2, const int y2, const int x3, const int y3,
                                  const bool ink) const {
  const int xs[3] = {x1, x2, x3};
  const int ys[3] = {y1, y2, y3};
  renderer.fillPolygon(xs, ys, 3, ink);
}

// Cada figura entra en un cuadrado de lado 2r centrado en (cx, cy). `ink` es el
// color de la figura: negro sobre carta blanca, blanco sobre carta emparejada.
void MemoryActivity::drawFigure(const int figure, const int cx, const int cy, const int r, const bool ink) const {
  switch (figure) {
    case FIG_CIRCLE: fillCircle(cx, cy, r, ink); break;

    case FIG_SQUARE: renderer.fillRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, ink); break;

    case FIG_TRIANGLE: fillTriangle(cx, cy - r, cx + r, cy + r, cx - r, cy + r, ink); break;

    case FIG_PLUS: {
      const int t = r / 2 > 4 ? r / 2 : 4;
      renderer.fillRect(cx - t / 2, cy - r, t, 2 * r + 1, ink);
      renderer.fillRect(cx - r, cy - t / 2, 2 * r + 1, t, ink);
      break;
    }

    case FIG_DIAMOND: fillDiamond(cx, cy, r, ink); break;

    case FIG_STAR: {
      int xs[10];
      int ys[10];
      for (int i = 0; i < 10; ++i) {
        xs[i] = cx + STAR_X[i] * r / 1000;
        ys[i] = cy + STAR_Y[i] * r / 1000;
      }
      renderer.fillPolygon(xs, ys, 10, ink);
      break;
    }

    case FIG_MOON:
      // Disco lleno al que se le come un mordisco con el color de la carta.
      fillCircle(cx, cy, r, ink);
      fillCircle(cx + r * 38 / 100, cy, r * 78 / 100, !ink);
      break;

    case FIG_HOURGLASS: {
      fillTriangle(cx - r, cy - r, cx + r, cy - r, cx, cy, ink);
      fillTriangle(cx - r, cy + r, cx + r, cy + r, cx, cy, ink);
      renderer.fillRect(cx - r, cy - r, 2 * r + 1, 4, ink);
      renderer.fillRect(cx - r, cy + r - 3, 2 * r + 1, 4, ink);
      break;
    }

    case FIG_CHECKER: {
      const int cell = (2 * r) / 4;
      for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
          if ((row + col) % 2 != 0) continue;
          renderer.fillRect(cx - r + col * cell, cy - r + row * cell, cell, cell, ink);
        }
      }
      break;
    }

    case FIG_SPIRAL: {
      const int step = r / 2 > 4 ? r / 2 : 4;
      int x = cx - step / 2;
      int y = cy - step / 2;
      int len = step;
      for (int i = 0; i < 6; ++i) {
        const int nx = x + SPIRAL_DX[i % 4] * len;
        const int ny = y + SPIRAL_DY[i % 4] * len;
        renderer.drawLine(x, y, nx, ny, 3, ink);
        x = nx;
        y = ny;
        if (i % 2 == 1) len += step;
      }
      break;
    }

    case FIG_RING:
      fillCircle(cx, cy, r, ink);
      fillCircle(cx, cy, r * 55 / 100, !ink);
      break;

    case FIG_XCROSS:
      renderer.drawLine(cx - r, cy - r, cx + r, cy + r, 5, ink);
      renderer.drawLine(cx - r, cy + r, cx + r, cy - r, 5, ink);
      break;

    case FIG_ARROW: {
      fillTriangle(cx, cy - r, cx + r * 3 / 4, cy, cx - r * 3 / 4, cy, ink);
      const int t = r / 2 > 4 ? r / 2 : 4;
      renderer.fillRect(cx - t / 2, cy, t, r, ink);
      break;
    }

    case FIG_SUN: {
      fillCircle(cx, cy, r / 2, ink);
      for (int i = 0; i < 8; ++i) {
        const int x1 = cx + RAY_X[i] * (r * 68 / 100) / 1000;
        const int y1 = cy + RAY_Y[i] * (r * 68 / 100) / 1000;
        const int x2 = cx + RAY_X[i] * r / 1000;
        const int y2 = cy + RAY_Y[i] * r / 1000;
        renderer.drawLine(x1, y1, x2, y2, 3, ink);
      }
      break;
    }

    case FIG_BOXES:
      renderer.drawRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, 3, ink);
      renderer.drawRect(cx - r * 6 / 10, cy - r * 6 / 10, r * 12 / 10, r * 12 / 10, 3, ink);
      renderer.fillRect(cx - r / 4, cy - r / 4, r / 2, r / 2, ink);
      break;

    case FIG_HASH:
    default: {
      const int off = r * 2 / 5;
      renderer.fillRect(cx - off - 2, cy - r, 4, 2 * r + 1, ink);
      renderer.fillRect(cx + off - 2, cy - r, 4, 2 * r + 1, ink);
      renderer.fillRect(cx - r, cy - off - 2, 2 * r + 1, 4, ink);
      renderer.fillRect(cx - r, cy + off - 2, 2 * r + 1, 4, ink);
      break;
    }
  }
}

void MemoryActivity::drawCard(const int idx, const int x, const int y, const int w, const int h) const {
  const uint8_t st = status[static_cast<size_t>(idx)];
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  int r = (w < h ? w : h) / 2 - 12;
  if (r < 8) r = 8;

  if (st == 2) {
    // Emparejada: queda destapada en negativo, bien distinta de la recién dada vuelta.
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
    drawFigure(figures[static_cast<size_t>(idx)], cx, cy, r, false);
    return;
  }

  renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  if (st == 1) {
    drawFigure(figures[static_cast<size_t>(idx)], cx, cy, r, true);
  } else {
    // Dorso: rayitas en diagonal, siempre adentro del recuadro.
    for (int py = y + 12; py < y + h - 16; py += 12) {
      for (int px = x + 12; px < x + w - 16; px += 12) {
        renderer.drawLine(px, py + 7, px + 7, py, 1, true);
      }
    }
  }

  if (state == State::PLAY && idx == cursor) {
    renderer.drawRoundedRect(x - 3, y - 3, w + 6, h + 6, 3, 12, true);
  }
}

void MemoryActivity::drawInfoBar(const int y) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[48];

  snprintf(buf, sizeof(buf), "%s %d/%d", I18N.get(StrId::STR_GAME_PAIRS), pairsFound, pairsTotal);
  renderer.drawText(UI_10_FONT_ID, MARGIN_X, y, buf);

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_MOVES), moves);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);

  const unsigned long secs = elapsedSeconds();
  snprintf(buf, sizeof(buf), "%s %lu:%02lu", I18N.get(StrId::STR_GAME_TIME), secs / 60UL, secs % 60UL);
  renderer.drawText(UI_10_FONT_ID, pageWidth - MARGIN_X - renderer.getTextWidth(UI_10_FONT_ID, buf), y, buf);
}

void MemoryActivity::drawBoard(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  const int cols = COLS[level];
  const int rows = ROWS[level];

  drawInfoBar(top + 4);

  const int boardTop = top + 30;
  const int availH = bottom - boardTop;
  if (availH <= 0) return;

  const int cellW = (pageWidth - 2 * MARGIN_X - (cols - 1) * GAP) / cols;
  int cellH = (availH - (rows - 1) * GAP) / rows;
  if (cellH > cellW * 5 / 4) cellH = cellW * 5 / 4;  // no dejar cartas larguísimas
  const int gridW = cols * cellW + (cols - 1) * GAP;
  const int gridH = rows * cellH + (rows - 1) * GAP;
  const int gridLeft = (pageWidth - gridW) / 2;
  const int gridTop = boardTop + (availH - gridH) / 2;

  for (int i = 0; i < static_cast<int>(status.size()); ++i) {
    const int row = i / cols;
    const int col = i % cols;
    drawCard(i, gridLeft + col * (cellW + GAP), gridTop + row * (cellH + GAP), cellW, cellH);
  }
}

void MemoryActivity::drawLevelScreen(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  constexpr int ROW_H = 74;
  const int listTop = top + (bottom - top - LEVEL_COUNT * ROW_H) / 2;
  char buf[64];

  for (int i = 0; i < LEVEL_COUNT; ++i) {
    const int y = listTop + i * ROW_H;
    const bool sel = i == level;
    if (sel) renderer.fillRoundedRect(MARGIN_X - 4, y, pageWidth - 2 * (MARGIN_X - 4), ROW_H - 10, 10, Color::Black);

    snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_LEVEL), i + 1);
    renderer.drawText(UI_12_FONT_ID, MARGIN_X + 8, y + 10, buf, !sel, EpdFontFamily::BOLD);

    snprintf(buf, sizeof(buf), "%dx%d - %d %s", COLS[i], ROWS[i], COLS[i] * ROWS[i] / 2,
             I18N.get(StrId::STR_GAME_PAIRS));
    renderer.drawText(UI_10_FONT_ID, MARGIN_X + 8, y + 36, buf, !sel);

    if (bestMoves[static_cast<size_t>(i)] > 0) {
      snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST),
               static_cast<unsigned>(bestMoves[static_cast<size_t>(i)]));
      renderer.drawText(UI_10_FONT_ID, pageWidth - MARGIN_X - 8 - renderer.getTextWidth(UI_10_FONT_ID, buf), y + 36,
                        buf, !sel);
    }
  }
}

void MemoryActivity::drawSummary(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[64];
  int y = top + (bottom - top) / 2 - 90;

  renderer.fillRoundedRect(MARGIN_X, y - 12, pageWidth - 2 * MARGIN_X, 58, 12, Color::Black);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 6, I18N.get(StrId::STR_GAME_WON), false, EpdFontFamily::BOLD);
  y += 76;

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_MOVES), moves);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += 34;

  snprintf(buf, sizeof(buf), "%s %lu:%02lu", I18N.get(StrId::STR_GAME_TIME), finalSeconds / 60UL, finalSeconds % 60UL);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += 34;

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_LEVEL), level + 1);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 30;

  const size_t lv = static_cast<size_t>(level);
  if (bestMoves[lv] > 0) {
    snprintf(buf, sizeof(buf), "%s %u %s - %u:%02u", I18N.get(StrId::STR_GAME_BEST),
             static_cast<unsigned>(bestMoves[lv]), I18N.get(StrId::STR_GAME_MOVES),
             static_cast<unsigned>(bestSeconds[lv] / 60u), static_cast<unsigned>(bestSeconds[lv] % 60u));
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  }
}

void MemoryActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(StrId::STR_GAME_MEMORY));

  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  switch (state) {
    case State::LEVEL: drawLevelScreen(top, bottom); break;
    case State::WON: drawSummary(top, bottom); break;
    default: drawBoard(top, bottom); break;
  }

  if (state == State::LEVEL) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_NEW), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::WON) {
    const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // Regla del panel: parcial rápido para cada carta y uno limpio cada tantos,
  // más los cambios de pantalla, que si no fantasmea.
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN) {
    mode = HalDisplay::HALF_REFRESH;
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(mode);
}
