#include "TetrisActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 14;
constexpr int GAP = 16;       // entre el pozo y la columna de datos
constexpr int SIDE_MIN = 96;  // ancho mínimo de la columna de datos

// Las siete piezas clásicas, cada una en sus cuatro giros, sobre una caja de
// 4x4. El bit (fila * 4 + columna) prendido es un casillero de la pieza. Las
// piezas de dos estados (I, S, Z) repiten los giros para que la tabla sea
// pareja y el giro nunca corra la pieza de lugar.
constexpr uint16_t SHAPES[7][4] = {
    {0x00F0, 0x4444, 0x00F0, 0x4444},  // I
    {0x0071, 0x0226, 0x0470, 0x0322},  // J
    {0x0074, 0x0622, 0x0170, 0x0223},  // L
    {0x0066, 0x0066, 0x0066, 0x0066},  // O
    {0x0036, 0x0462, 0x0036, 0x0462},  // S
    {0x0072, 0x0262, 0x0270, 0x0232},  // T
    {0x0063, 0x0264, 0x0063, 0x0264},  // Z
};

// Pataditas del giro: si girando queda pisando la pared o lo apilado, se prueba
// correr la pieza un poco antes de dar el giro por imposible.
constexpr int KICK_X[6] = {0, -1, 1, -2, 2, 0};
constexpr int KICK_Y[6] = {0, 0, 0, 0, 0, -1};

// Puntos por cantidad de líneas hechas de una vez, multiplicados por el nivel.
constexpr int LINE_POINTS[5] = {0, 100, 300, 500, 800};

inline bool shapeBit(const int piece, const int rot, const int r, const int c) {
  return (SHAPES[piece][rot] & (1u << (r * 4 + c))) != 0;
}
}  // namespace

long TetrisActivity::bestScore = 0;

// ----------------------------------------------------------------- partida --

void TetrisActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  startGame();
}

void TetrisActivity::startGame() {
  board.fill(0);
  fullRow.fill(false);
  fullRowCount = 0;
  score = 0;
  lines = 0;
  level = 1;
  bagIndex = PIECES;
  nextPiece = takeFromBag();
  state = State::PLAYING;
  forceClean = true;
  spawnPiece();
}

// Bolsa de 7 barajada (Fisher-Yates): salen todas las piezas antes de repetir,
// que es lo que hace que el juego sea justo y no una lotería de barras.
void TetrisActivity::refillBag() {
  for (int i = 0; i < PIECES; ++i) bag[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
  for (int i = PIECES - 1; i > 0; --i) {
    const int j = static_cast<int>(random(i + 1));
    const uint8_t tmp = bag[static_cast<size_t>(i)];
    bag[static_cast<size_t>(i)] = bag[static_cast<size_t>(j)];
    bag[static_cast<size_t>(j)] = tmp;
  }
  bagIndex = 0;
}

int TetrisActivity::takeFromBag() {
  if (bagIndex >= PIECES) refillBag();
  return static_cast<int>(bag[static_cast<size_t>(bagIndex++)]);
}

void TetrisActivity::spawnPiece() {
  piece = nextPiece;
  nextPiece = takeFromBag();
  rot = 0;
  px = 3;
  py = 0;
  landed = false;
  lastFall = millis();
  // No entra la pieza nueva: se llenó el pozo.
  if (collides(piece, rot, px, py)) {
    gameOver();
    return;
  }
  requestUpdate();
}

bool TetrisActivity::collides(const int shapePiece, const int shapeRot, const int atX, const int atY) const {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!shapeBit(shapePiece, shapeRot, r, c)) continue;
      const int bx = atX + c;
      const int by = atY + r;
      if (bx < 0 || bx >= COLS || by >= ROWS) return true;
      if (by >= 0 && cellFilled(by, bx)) return true;
    }
  }
  return false;
}

// El nivel sube cada diez líneas y acelera la caída, pero nunca por debajo del
// piso que puede mostrar la tinta electrónica.
unsigned long TetrisActivity::fallIntervalMs() const {
  const unsigned long step = static_cast<unsigned long>(level - 1) * 55UL;
  return FALL_START_MS > FALL_MIN_MS + step ? FALL_START_MS - step : FALL_MIN_MS;
}

int TetrisActivity::ghostY() const {
  int y = py;
  while (!collides(piece, rot, px, y + 1)) ++y;
  return y;
}

// Una pasada de la caída automática. Si no puede bajar se da una pasada de
// gracia (`landed`) para acomodarla, y recién en la siguiente se apoya.
void TetrisActivity::tick() {
  lastFall = millis();
  if (!collides(piece, rot, px, py + 1)) {
    ++py;
    landed = false;
    requestUpdate();
    return;
  }
  if (landed) {
    lockPiece();
    return;
  }
  landed = true;  // nada cambió en pantalla: no se repinta
}

void TetrisActivity::move(const int dx) {
  if (collides(piece, rot, px + dx, py)) return;
  px += dx;
  requestUpdate();
}

void TetrisActivity::rotate() {
  const int next = (rot + 1) % 4;
  if (next == rot) return;
  for (int k = 0; k < 6; ++k) {
    const int tryX = px + KICK_X[k];
    const int tryY = py + KICK_Y[k];
    if (collides(piece, next, tryX, tryY)) continue;
    rot = next;
    px = tryX;
    py = tryY;
    requestUpdate();
    return;
  }
}

// Atrás mantenido: la pieza baja de golpe y se apoya en el acto (sin pasada de
// gracia), sumando dos puntos por fila, como en el original.
void TetrisActivity::hardDrop() {
  int dropped = 0;
  while (!collides(piece, rot, px, py + 1)) {
    ++py;
    ++dropped;
  }
  score += 2L * dropped;
  lockPiece();
}

void TetrisActivity::lockPiece() {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!shapeBit(piece, rot, r, c)) continue;
      const int by = py + r;
      const int bx = px + c;
      if (by >= 0 && by < ROWS && bx >= 0 && bx < COLS) setCell(by, bx, true);
    }
  }

  fullRow.fill(false);
  fullRowCount = 0;
  for (int r = 0; r < ROWS; ++r) {
    bool full = true;
    for (int c = 0; c < COLS && full; ++c) {
      if (!cellFilled(r, c)) full = false;
    }
    if (full) {
      fullRow[static_cast<size_t>(r)] = true;
      ++fullRowCount;
    }
  }

  if (fullRowCount > 0) {
    // Destello de las filas completas antes de sacarlas: un solo refresco más y
    // se entiende qué pasó.
    state = State::CLEARING;
    clearAt = millis();
    forceClean = true;
    requestUpdate();
    return;
  }
  spawnPiece();
}

// Terminado el destello: se sacan las filas, baja lo de arriba y se cuenta.
void TetrisActivity::finishClear() {
  int write = ROWS - 1;
  for (int r = ROWS - 1; r >= 0; --r) {
    if (fullRow[static_cast<size_t>(r)]) continue;
    if (write != r) {
      for (int c = 0; c < COLS; ++c) setCell(write, c, cellFilled(r, c));
    }
    --write;
  }
  for (int r = write; r >= 0; --r) {
    for (int c = 0; c < COLS; ++c) setCell(r, c, false);
  }

  const int n = fullRowCount > 4 ? 4 : fullRowCount;
  score += static_cast<long>(LINE_POINTS[n]) * level;
  lines += fullRowCount;
  level = 1 + lines / 10;
  if (level > MAX_LEVEL) level = MAX_LEVEL;
  if (score > bestScore) bestScore = score;

  fullRow.fill(false);
  fullRowCount = 0;
  state = State::PLAYING;
  forceClean = true;
  spawnPiece();
}

void TetrisActivity::gameOver() {
  if (score > bestScore) bestScore = score;
  state = State::OVER;
  forceClean = true;
  requestUpdate();
}

// -------------------------------------------------------------------- loop --

void TetrisActivity::loop() {
  if (state == State::CLEARING) {
    if (millis() - clearAt >= CLEAR_FLASH_MS) finishClear();
    return;
  }

  // Atrás mantenido = bajar de golpe. Solo se consulta jugando: `wasLongPressed`
  // se come la suelta, y en los carteles hace falta el Atrás corto.
  if (state == State::PLAYING && mappedInput.wasLongPressed(MappedInputManager::Button::Back, DROP_HOLD_MS)) {
    hardDrop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    switch (state) {
      case State::PLAYING:
        // Salir pide confirmación: un toque de más no tira la partida.
        state = State::CONFIRM;
        forceClean = true;
        requestUpdate();
        break;
      case State::CONFIRM:
        state = State::PLAYING;
        lastFall = millis();
        forceClean = true;
        requestUpdate();
        break;
      default:
        finish();
        break;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::PLAYING:
        rotate();
        break;
      case State::CONFIRM:
        finish();
        break;
      default:
        startGame();
        break;
    }
    return;
  }

  if (state != State::PLAYING) return;

  // La palanca es vertical: arriba mueve a la izquierda, abajo a la derecha.
  buttonNavigator.onPrevious([this] { move(-1); });
  buttonNavigator.onNext([this] { move(1); });

  if (millis() - lastFall >= fallIntervalMs()) tick();
}

// ------------------------------------------------------------------ dibujo --

// Casillero apoyado o de la pieza: negro con un anillo blanco y un corazón
// negro, así dos casilleros pegados se distinguen en blanco y negro.
void TetrisActivity::drawBlock(const int x, const int y, const int cell) const {
  int inset = cell / 7;
  if (inset < 2) inset = 2;
  int core = cell * 2 / 7;
  if (core < inset + 1) core = inset + 1;
  renderer.fillRect(x + 1, y + 1, cell - 2, cell - 2, true);
  renderer.fillRect(x + inset, y + inset, cell - 2 * inset, cell - 2 * inset, false);
  if (cell - 2 * core >= 3) renderer.fillRect(x + core, y + core, cell - 2 * core, cell - 2 * core, true);
}

// Pieza fantasma: dónde va a caer si nadie la toca. Con la caída lenta de la
// tinta electrónica es lo que hace jugable el juego.
void TetrisActivity::drawGhostBlock(const int x, const int y, const int cell) const {
  renderer.drawRect(x + 3, y + 3, cell - 6, cell - 6, 2, true);
}

void TetrisActivity::drawWell(const int x, const int y, const int cell) const {
  const int w = cell * COLS;
  const int h = cell * ROWS;

  renderer.drawRect(x - 3, y - 3, w + 6, h + 6, 3, true);

  // Puntitos en las esquinas de los casilleros: ayudan a contar columnas sin
  // ensuciar la pantalla con una grilla entera.
  for (int r = 1; r < ROWS; ++r) {
    for (int c = 1; c < COLS; ++c) renderer.drawPixel(x + c * cell, y + r * cell, true);
  }

  for (int r = 0; r < ROWS; ++r) {
    if (state == State::CLEARING && fullRow[static_cast<size_t>(r)]) {
      renderer.fillRect(x, y + r * cell, w, cell, true);  // destello de la fila completa
      continue;
    }
    for (int c = 0; c < COLS; ++c) {
      if (cellFilled(r, c)) drawBlock(x + c * cell, y + r * cell, cell);
    }
  }

  if (state != State::PLAYING && state != State::CONFIRM) return;

  const int gy = ghostY();
  if (gy != py) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        if (!shapeBit(piece, rot, r, c)) continue;
        const int by = gy + r;
        if (by < 0 || by >= ROWS) continue;
        drawGhostBlock(x + (px + c) * cell, y + by * cell, cell);
      }
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!shapeBit(piece, rot, r, c)) continue;
      const int by = py + r;
      if (by < 0 || by >= ROWS) continue;
      drawBlock(x + (px + c) * cell, y + by * cell, cell);
    }
  }
}

// La pieza que viene, centrada en su caja (se recorta la caja de 4x4 a lo que
// la pieza ocupa de verdad, si no la I y la O quedan descentradas).
void TetrisActivity::drawMiniPiece(const int shapePiece, const int boxX, const int boxY, const int boxW,
                                   const int boxH, const int cell) const {
  int minR = 3, maxR = 0, minC = 3, maxC = 0;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!shapeBit(shapePiece, 0, r, c)) continue;
      if (r < minR) minR = r;
      if (r > maxR) maxR = r;
      if (c < minC) minC = c;
      if (c > maxC) maxC = c;
    }
  }
  const int w = (maxC - minC + 1) * cell;
  const int h = (maxR - minR + 1) * cell;
  const int originX = boxX + (boxW - w) / 2 - minC * cell;
  const int originY = boxY + (boxH - h) / 2 - minR * cell;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (shapeBit(shapePiece, 0, r, c)) drawBlock(originX + c * cell, originY + r * cell, cell);
    }
  }
}

void TetrisActivity::drawStat(const int x, const int y, const int w, const char* label, const long value) const {
  char buf[32];
  renderer.drawText(SMALL_FONT_ID, x, y, renderer.truncatedText(SMALL_FONT_ID, label, w).c_str());
  snprintf(buf, sizeof(buf), "%ld", value);
  renderer.drawText(UI_12_FONT_ID, x, y + 20, buf, true, EpdFontFamily::BOLD);
}

void TetrisActivity::drawSidebar(const int x, const int y, const int w, const int cell) const {
  // Caja de lo que viene, con un triangulito arriba que dice para dónde va.
  const int boxH = cell * 3;
  const int triW = 14;
  const int cx = x + w / 2;
  const int xs[3] = {cx - triW / 2, cx + triW / 2, cx};
  const int ys[3] = {y, y, y + 10};
  renderer.fillPolygon(xs, ys, 3, true);

  const int boxY = y + 16;
  renderer.drawRect(x, boxY, w, boxH, 2, true);
  drawMiniPiece(nextPiece, x + 2, boxY + 2, w - 4, boxH - 4, cell * 2 / 3);

  int statY = boxY + boxH + 26;
  const int statStep = 58;
  drawStat(x, statY, w, I18N.get(StrId::STR_GAME_SCORE), score);
  statY += statStep;
  drawStat(x, statY, w, I18N.get(StrId::STR_GAME_LEVEL), level);
  statY += statStep;
  drawStat(x, statY, w, I18N.get(StrId::STR_GAME_LINES), lines);
  statY += statStep;
  drawStat(x, statY, w, I18N.get(StrId::STR_GAME_BEST), bestScore);
}

// Debajo del pozo: Atrás mantenido un segundo baja la pieza de golpe (los
// hints de abajo solo tienen lugar para lo corto).
void TetrisActivity::drawDropHint(const int y) const {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s  ·  1 s", I18N.get(StrId::STR_GAME_DROP));
  const int pageWidth = renderer.getScreenWidth();
  const int textW = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int textX = (pageWidth - textW) / 2 + 10;
  renderer.drawText(SMALL_FONT_ID, textX, y, buf);
  const int cx = textX - 16;
  const int xs[3] = {cx - 6, cx + 6, cx};
  const int ys[3] = {y + 3, y + 3, y + 13};
  renderer.fillPolygon(xs, ys, 3, true);
}

// Cartel de confirmación de salida o resumen final, encima del tablero.
void TetrisActivity::drawOverlay() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const bool over = state == State::OVER;
  const int w = pageWidth - 72;
  const int h = over ? 300 : 150;
  const int x = (pageWidth - w) / 2;
  const int y = (pageHeight - h) / 2 - 40;
  char buf[64];

  renderer.fillRoundedRect(x, y, w, h, 16, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 16, true);

  int ty = y + 26;
  renderer.drawCenteredText(UI_12_FONT_ID, ty, I18N.get(over ? StrId::STR_GAME_OVER : StrId::STR_GAME_QUIT), true,
                            EpdFontFamily::BOLD);
  ty += 44;

  if (!over) {
    snprintf(buf, sizeof(buf), "%s %ld", I18N.get(StrId::STR_GAME_SCORE), score);
    renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
    return;
  }

  snprintf(buf, sizeof(buf), "%s  %ld", I18N.get(StrId::STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf);
  ty += 40;
  snprintf(buf, sizeof(buf), "%s  %d", I18N.get(StrId::STR_GAME_LINES), lines);
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf);
  ty += 40;
  snprintf(buf, sizeof(buf), "%s  %d", I18N.get(StrId::STR_GAME_LEVEL), level);
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf);
  ty += 44;
  snprintf(buf, sizeof(buf), "%s  %ld", I18N.get(StrId::STR_GAME_BEST), bestScore);
  renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
}

void TetrisActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(StrId::STR_GAME_TETRIS));

  const int top = metrics.topPadding + metrics.headerHeight;
  // Siempre libre abajo lo que ocupan los hints, si no la última fila se tapa.
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  // El casillero es lo más grande que entra a lo ancho (dejando la columna de
  // datos) y a lo alto (dejando la línea del hint de abajo).
  const int byWidth = (pageWidth - 2 * MARGIN - SIDE_MIN - GAP) / COLS;
  const int byHeight = (bottom - top - 12 - 28) / ROWS;
  int cell = byWidth < byHeight ? byWidth : byHeight;
  if (cell < 12) cell = 12;

  const int wellW = cell * COLS;
  const int wellH = cell * ROWS;
  const int wellX = MARGIN;
  const int wellY = top + 12;
  const int sideX = wellX + wellW + GAP;
  const int sideW = pageWidth - MARGIN - sideX;

  drawWell(wellX, wellY, cell);
  if (sideW >= 40) drawSidebar(sideX, wellY + 4, sideW, cell);
  if (state == State::PLAYING) drawDropHint(wellY + wellH + 12);

  if (state == State::CONFIRM || state == State::OVER) drawOverlay();

  switch (state) {
    case State::CONFIRM: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_CONTINUE), tr(STR_GAME_QUIT), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::OVER: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    default: {
      // "<" y ">" porque la palanca es vertical pero mueve a los costados; no
      // hay texto inventado, son símbolos.
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_ROTATE), "<", ">");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  // Regla del panel: parcial rápido para cada movida y uno limpio cada doce (o
  // cuando cambia la pantalla entera), si no fantasmea.
  ++partialCount;
  const bool clean = forceClean || partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
