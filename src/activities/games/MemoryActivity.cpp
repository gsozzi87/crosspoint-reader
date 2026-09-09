#include "MemoryActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "memoryIcons.h"
#include "components/Selection.h"

namespace {
// Nivel -> forma de la grilla. 8, 10 y 15 parejas.
constexpr int COLS[] = {4, 4, 5};
constexpr int ROWS[] = {4, 5, 6};

// Las dieciséis figuras, en los dos tamaños de `memoryIcons.h`. Son iconos
// Lucide de verdad: se reconocen de un vistazo y no hay dos que se confundan.
const freeink::Icon* const FIGURES_SMALL[] = {
    &icon_heart_48, &icon_star_48,   &icon_sun_48,    &icon_moon_48,
    &icon_cloud_48, &icon_umbrella_48, &icon_anchor_48, &icon_key_48,
    &icon_gift_48,  &icon_fish_48,   &icon_bird_48,   &icon_cat_48,
    &icon_rocket_48, &icon_ghost_48, &icon_crown_48,  &icon_flame_48,
};
const freeink::Icon* const FIGURES_BIG[] = {
    &icon_heart_64, &icon_star_64,   &icon_sun_64,    &icon_moon_64,
    &icon_cloud_64, &icon_umbrella_64, &icon_anchor_64, &icon_key_64,
    &icon_gift_64,  &icon_fish_64,   &icon_bird_64,   &icon_cat_64,
    &icon_rocket_64, &icon_ghost_64, &icon_crown_64,  &icon_flame_64,
};
constexpr int FIGURES_IN_TABLE = sizeof(FIGURES_SMALL) / sizeof(FIGURES_SMALL[0]);

// El icono se dibuja pixel por pixel a través del renderer, así queda bien en
// cualquier orientación y se puede invertir sobre la carta emparejada.
void blitIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const bool ink) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) == 0) renderer.drawPixel(x + col, y + row, ink);
    }
  }
}
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

void MemoryActivity::drawFigure(const int figure, const int cx, const int cy, const bool big,
                                const bool ink) const {
  const int idx = figure >= 0 && figure < FIGURES_IN_TABLE ? figure : 0;
  const freeink::Icon& icon = *(big ? FIGURES_BIG[idx] : FIGURES_SMALL[idx]);
  blitIcon(renderer, icon, cx - icon.w / 2, cy - icon.h / 2, ink);
}

void MemoryActivity::drawCard(const int idx, const int x, const int y, const int w, const int h) const {
  const uint8_t st = status[static_cast<size_t>(idx)];
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  // El icono grande (64 px) solo si la carta le deja aire alrededor.
  const bool big = w >= 84 && h >= 84;

  if (st == 2) {
    // Emparejada: queda destapada en negativo, bien distinta de la recién dada vuelta.
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
    drawFigure(figures[static_cast<size_t>(idx)], cx, cy, big, false);
    return;
  }

  if (st == 1) {
    // Recién dada vuelta: marco doble para que se vea de una cuál se acaba de tocar.
    renderer.fillRoundedRect(x, y, w, h, 10, Color::White);
    renderer.drawRoundedRect(x, y, w, h, 3, 10, true);
    renderer.drawRoundedRect(x + 6, y + 6, w - 12, h - 12, 1, 7, true);
    drawFigure(figures[static_cast<size_t>(idx)], cx, cy, big, true);
  } else {
    drawCardBack(x, y, w, h);
  }

  if (state == State::PLAY && idx == cursor) drawCursor(x, y, w, h);
}

// Dorso: marco fino y una red de rombos adentro, recortada a mano contra el
// rectángulo interior (drawLine no recorta sola).
void MemoryActivity::drawCardBack(const int x, const int y, const int w, const int h) const {
  renderer.fillRoundedRect(x, y, w, h, 10, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  const int ix = x + 8;
  const int iy = y + 8;
  const int iw = w - 16;
  const int ih = h - 16;
  if (iw <= 4 || ih <= 4) return;
  renderer.drawRect(ix, iy, iw, ih, 1, true);
  constexpr int STEP = 9;
  for (int d = -ih; d < iw; d += STEP) {
    const int t0 = d < 0 ? -d : 0;
    const int t1 = iw - d < ih ? iw - d : ih;
    if (t1 > t0) renderer.drawLine(ix + d + t0, iy + t0, ix + d + t1, iy + t1, true);
  }
  for (int d = 0; d <= iw + ih; d += STEP) {
    const int t0 = d - iw > 0 ? d - iw : 0;
    const int t1 = d < ih ? d : ih;
    if (t1 > t0) renderer.drawLine(ix + d - t0, iy + t0, ix + d - t1, iy + t1, true);
  }
}

// Cursor: marco por fuera de la carta (nunca encima, así no tapa el dibujo) con
// las cuatro esquinas engrosadas. Se ve igual de claro sobre el dorso rayado que
// sobre una carta blanca.
void MemoryActivity::drawCursor(const int x, const int y, const int w, const int h) const {
  const int o = 4;  // cuánto se sale del borde de la carta
  renderer.drawRoundedRect(x - o, y - o, w + 2 * o, h + 2 * o, 3, 13, true);
  const int arm = (w < h ? w : h) / 3;
  const int t = 4;  // grosor de las escuadras
  // Arriba y abajo de las cuatro esquinas, siempre dentro de la banda del marco.
  renderer.fillRect(x - o, y - o, arm, t, true);
  renderer.fillRect(x + w + o - arm, y - o, arm, t, true);
  renderer.fillRect(x - o, y + h + o - t, arm, t, true);
  renderer.fillRect(x + w + o - arm, y + h + o - t, arm, t, true);
  renderer.fillRect(x - o, y - o, t, arm, true);
  renderer.fillRect(x + w + o - t, y - o, t, arm, true);
  renderer.fillRect(x - o, y + h + o - arm, t, arm, true);
  renderer.fillRect(x + w + o - t, y + h + o - arm, t, arm, true);
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
    if (sel) drawSelectionRow(renderer, MARGIN_X - 4, y, pageWidth - 2 * (MARGIN_X - 4), ROW_H - 10, 10);

    snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_LEVEL), i + 1);
    renderer.drawText(UI_12_FONT_ID, MARGIN_X + 8, y + 10, buf, SELECTION_INK, EpdFontFamily::BOLD);

    snprintf(buf, sizeof(buf), "%dx%d - %d %s", COLS[i], ROWS[i], COLS[i] * ROWS[i] / 2,
             I18N.get(StrId::STR_GAME_PAIRS));
    renderer.drawText(UI_10_FONT_ID, MARGIN_X + 8, y + 36, buf, SELECTION_INK);

    if (bestMoves[static_cast<size_t>(i)] > 0) {
      snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST),
               static_cast<unsigned>(bestMoves[static_cast<size_t>(i)]));
      renderer.drawText(UI_10_FONT_ID, pageWidth - MARGIN_X - 8 - renderer.getTextWidth(UI_10_FONT_ID, buf), y + 36,
                        buf, SELECTION_INK);
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

  // Las ayudas dicen siempre qué hace cada botón AHORA. Con dos cartas a la
  // vista la palanca no mueve nada y OK las tapa: no se anuncian flechas.
  switch (state) {
    case State::LEVEL: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::WON: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PEEK: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_CONTINUE), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    default: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
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
