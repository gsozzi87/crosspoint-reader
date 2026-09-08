#include "BreakoutActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 12;
constexpr int INFO_HEIGHT = 34;  // línea de vidas y puntaje debajo del campo
constexpr int POINTS_BRICK = 10;
constexpr int POINTS_CRACK = 5;   // primer golpe a un ladrillo duro
constexpr int POINTS_LEVEL = 50;  // por terminar el nivel
}  // namespace

long BreakoutActivity::bestScore = 0;

// ----------------------------------------------------------------- partida --

void BreakoutActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::START;
  fullRepaint = true;
  requestUpdate();
}

void BreakoutActivity::startGame() {
  score = 0;
  lives = START_LIVES;
  level = 1;
  buildLevel();
  fullRepaint = true;
  requestUpdate();
}

// Cada nivel suma una fila, endurece las de arriba (dos golpes) y acelera la
// pelota. La paleta siempre mide tres casilleros: con dos no habría casillero
// del medio, la pelota nunca saldría derecha para arriba y quedaría atrapada en
// los casilleros de una sola paridad, con ladrillos imposibles de tocar.
void BreakoutActivity::buildLevel() {
  bricks.fill(0);
  brickRows = 3 + level;
  if (brickRows > MAX_BRICK_ROWS) brickRows = MAX_BRICK_ROWS;
  const int hardRows = level >= 3 ? 2 : level >= 2 ? 1 : 0;
  bricksLeft = 0;
  for (int r = 0; r < brickRows; ++r) {
    for (int c = 0; c < BCOLS; ++c) {
      bricks[static_cast<size_t>(r * BCOLS + c)] = r < hardRows ? 2 : 1;
      ++bricksLeft;
    }
  }
  paddleW = 3;
  resetBall();
}

void BreakoutActivity::resetBall() {
  paddleCol = (COLS - paddleW) / 2;
  ballCol = paddleCol + paddleW / 2;
  ballRow = PADDLE_ROW - 1;
  bdx = 0;
  bdy = -1;
  state = State::SERVE;
  lastStep = millis();
}

bool BreakoutActivity::brickAt(const int col, const int row) const {
  if (col < 0 || col >= COLS) return false;
  const int r = row - BRICK_TOP;
  if (r < 0 || r >= brickRows) return false;
  return bricks[static_cast<size_t>(r * BCOLS + col / 2)] > 0;
}

void BreakoutActivity::hitBrick(const int col, const int row) {
  const int r = row - BRICK_TOP;
  if (col < 0 || col >= COLS || r < 0 || r >= brickRows) return;
  const int bcol = col / 2;
  uint8_t& hits = bricks[static_cast<size_t>(r * BCOLS + bcol)];
  if (hits == 0) return;
  --hits;
  if (hits == 0) {
    --bricksLeft;
    score += static_cast<long>(POINTS_BRICK) * level;
  } else {
    score += POINTS_CRACK;
  }
  if (score > bestScore) bestScore = score;
  infoDirty = true;
  // El ladrillo ocupa dos casilleros: se repintan los dos.
  markDirty(bcol * 2, row);
  markDirty(bcol * 2 + 1, row);
}

unsigned long BreakoutActivity::stepIntervalMs() const {
  const unsigned long faster = static_cast<unsigned long>(level - 1) * STEP_LEVEL_MS;
  return STEP_START_MS > STEP_MIN_MS + faster ? STEP_START_MS - faster : STEP_MIN_MS;
}

// Un casillero por paso. Primero las paredes, después los ladrillos (el de
// arriba/abajo, el del costado y el de la diagonal) y al final la paleta.
void BreakoutActivity::step() {
  lastStep = millis();
  const int fromCol = ballCol;
  const int fromRow = ballRow;
  int nc = ballCol + bdx;
  int nr = ballRow + bdy;

  if (nc < 0 || nc >= COLS) {
    bdx = -bdx;
    nc = ballCol + bdx;
  }
  if (nr < 0) {
    bdy = -bdy;
    nr = ballRow + bdy;
  }

  const bool vBrick = brickAt(ballCol, ballRow + bdy);
  const bool hBrick = bdx != 0 && brickAt(ballCol + bdx, ballRow);
  const bool dBrick = brickAt(nc, nr);
  if (vBrick || hBrick || dBrick) {
    if (vBrick) hitBrick(ballCol, ballRow + bdy);
    if (hBrick) hitBrick(ballCol + bdx, ballRow);
    if (!vBrick && !hBrick) hitBrick(nc, nr);
    if (vBrick || !hBrick) bdy = -bdy;  // de frente o de esquina: rebota para el otro lado
    if (hBrick) bdx = -bdx;
    nc = ballCol + bdx;
    nr = ballRow + bdy;
    if (nc < 0 || nc >= COLS) {
      bdx = -bdx;
      nc = ballCol + bdx;
    }
    if (nr < 0) {
      bdy = -bdy;
      nr = ballRow + bdy;
    }
    // Encajonada entre ladrillos: se queda donde está y sigue en la próxima.
    if (brickAt(nc, nr)) {
      nc = ballCol;
      nr = ballRow;
    }
  }

  if (nr >= PADDLE_ROW) {
    if (nc >= paddleCol && nc < paddleCol + paddleW) {
      // Donde pega decide el ángulo: los bordes abren, el centro la manda
      // derecho para arriba (y de paso cambia la paridad del casillero, que es
      // lo que hace alcanzables todas las columnas).
      const int rel = nc - paddleCol;
      if (rel == 0) {
        bdx = -1;
      } else if (rel == paddleW - 1) {
        bdx = 1;
      } else if (bdx != 0) {
        bdx = 0;  // pegándole con el centro la pelota sale derecha para arriba
      } else {
        // Ya venía derecha y volvió a caer en el centro: sin esto sube y baja
        // por la misma columna para siempre y la partida no termina nunca.
        bdx = random(2) == 0 ? -1 : 1;
      }
      bdy = -1;
      nr = PADDLE_ROW - 1;
    } else {
      markDirty(fromCol, fromRow);
      loseLife();
      return;
    }
  }

  if (nc != fromCol || nr != fromRow) {
    markDirty(fromCol, fromRow);
    ballCol = nc;
    ballRow = nr;
    markDirty(ballCol, ballRow);
  }

  if (bricksLeft <= 0) {
    score += static_cast<long>(POINTS_LEVEL) * level;
    if (score > bestScore) bestScore = score;
    state = State::LEVEL_DONE;
    fullRepaint = true;
  }
  requestUpdate();
}

void BreakoutActivity::movePaddle(const int dir) {
  int col = paddleCol + dir;
  if (col < 0) col = 0;
  if (col > COLS - paddleW) col = COLS - paddleW;
  if (col == paddleCol) return;
  markPaddleDirty();
  const bool carryBall = state == State::SERVE;
  if (carryBall) markDirty(ballCol, ballRow);
  paddleCol = col;
  markPaddleDirty();
  if (carryBall) {
    ballCol = paddleCol + paddleW / 2;
    markDirty(ballCol, ballRow);
  }
  requestUpdate();
}

void BreakoutActivity::loseLife() {
  --lives;
  infoDirty = true;
  if (lives <= 0) {
    gameOver();
    return;
  }
  resetBall();
  fullRepaint = true;
  requestUpdate();
}

void BreakoutActivity::gameOver() {
  if (score > bestScore) bestScore = score;
  state = State::OVER;
  fullRepaint = true;
  requestUpdate();
}

void BreakoutActivity::markDirty(const int col, const int row) {
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
  const uint16_t idx = static_cast<uint16_t>(row * COLS + col);
  for (int i = 0; i < dirtyCount; ++i) {
    if (dirty[static_cast<size_t>(i)] == idx) return;
  }
  if (dirtyCount >= MAX_DIRTY) {
    fullRepaint = true;  // más cambios que la lista: sale más barato rehacer todo
    return;
  }
  dirty[static_cast<size_t>(dirtyCount++)] = idx;
}

void BreakoutActivity::markPaddleDirty() {
  for (int i = 0; i < paddleW; ++i) markDirty(paddleCol + i, PADDLE_ROW);
}

// -------------------------------------------------------------------- loop --

void BreakoutActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lastInputAt = millis();
    if (state == State::PLAYING || state == State::SERVE) {
      // Atrás no tira la partida: primero pausa, y desde la pausa sale.
      resumeState = state;
      state = State::PAUSED;
      fullRepaint = true;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lastInputAt = millis();
    switch (state) {
      case State::START:
      case State::OVER:
        startGame();
        break;
      case State::SERVE:
        // Saca la pelota siempre en diagonal: derecho para arriba es aburrido.
        bdx = random(2) == 0 ? -1 : 1;
        bdy = -1;
        state = State::PLAYING;
        lastStep = millis();
        fullRepaint = true;
        requestUpdate();
        break;
      case State::PLAYING:
        resumeState = State::PLAYING;
        state = State::PAUSED;
        fullRepaint = true;
        requestUpdate();
        break;
      case State::PAUSED:
        // Vuelve donde estaba: la pausa no puede costar la pelota que ya venía
        // andando ni saltearse el saque.
        state = resumeState;
        lastStep = millis();
        fullRepaint = true;
        requestUpdate();
        break;
      case State::LEVEL_DONE:
        ++level;
        buildLevel();
        fullRepaint = true;
        requestUpdate();
        break;
    }
    return;
  }

  if (state != State::PLAYING && state != State::SERVE) return;

  // La palanca es vertical pero la paleta va a los costados: arriba izquierda,
  // abajo derecha (los hints lo dicen). Mantenida, sigue andando.
  buttonNavigator.onPrevious([this] {
    lastInputAt = millis();
    movePaddle(-1);
  });
  buttonNavigator.onNext([this] {
    lastInputAt = millis();
    movePaddle(1);
  });

  if (state == State::PLAYING && millis() - lastStep >= stepIntervalMs()) step();
}

// ------------------------------------------------------------------ dibujo --

void BreakoutActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight;
  // Siempre libre abajo lo que ocupan los hints, si no la paleta se tapa.
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  int cell = (pageWidth - 2 * MARGIN) / COLS;
  const int byHeight = (bottom - top - 8 - INFO_HEIGHT) / ROWS;
  if (byHeight < cell) cell = byHeight;
  if (cell > MAX_CELL) cell = MAX_CELL;
  if (cell < 8) cell = 8;

  cellPx = cell;
  boardX = (pageWidth - cell * COLS) / 2;
  boardY = top + 8;
  infoY = boardY + cell * ROWS + 8;
}

void BreakoutActivity::fillCircle(const int cx, const int cy, const int r, const bool on) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, on);
  }
}

// Media mitad de ladrillo: el marco negro va por los tres lados de afuera y el
// lado que da a la otra mitad queda abierto, así las dos mitades se leen como
// un solo ladrillo ancho. Los duros (dos golpes) van macizos.
void BreakoutActivity::drawBrickCell(const int col, const int row, const int hits) const {
  const int cell = cellPx;
  const int x = boardX + col * cell;
  const int y = boardY + row * cell;
  const bool leftHalf = (col % 2) == 0;
  const int outerX = leftHalf ? x + 3 : x;
  const int outerW = cell - 3;
  renderer.fillRect(outerX, y + 3, outerW, cell - 6, true);
  if (hits >= 2) return;  // ladrillo duro: macizo
  if (leftHalf) {
    renderer.fillRect(x + 6, y + 6, cell - 6, cell - 12, false);
  } else {
    renderer.fillRect(x, y + 6, cell - 6, cell - 12, false);
  }
}

void BreakoutActivity::drawCell(const int idx) const {
  if (idx < 0 || idx >= CELLS) return;
  const int c = idx % COLS;
  const int r = idx / COLS;
  const int cell = cellPx;
  const int x = boardX + c * cell;
  const int y = boardY + r * cell;
  renderer.fillRect(x, y, cell, cell, false);  // borrar lo que había

  const int br = r - BRICK_TOP;
  if (br >= 0 && br < brickRows) {
    const int hits = bricks[static_cast<size_t>(br * BCOLS + c / 2)];
    if (hits > 0) drawBrickCell(c, r, hits);
  }

  if (r == PADDLE_ROW) {
    // La raya del piso vive en el borde de arriba de esta fila: al borrar el
    // casillero se va con él, así que se vuelve a poner en cada repintado.
    renderer.drawLine(x, y, x + cell - 1, y, true);
    if (c >= paddleCol && c < paddleCol + paddleW) {
      const int barH = cell / 3 + 2;
      renderer.fillRect(x, y + (cell - barH) / 2, cell, barH, true);
    }
  }

  if (state != State::START && c == ballCol && r == ballRow) {
    int rad = cell / 2 - 5;
    if (rad < 3) rad = 3;
    fillCircle(x + cell / 2, y + cell / 2, rad, true);
  }
}

void BreakoutActivity::drawBoardFrame() const {
  renderer.drawRect(boardX - 3, boardY - 3, cellPx * COLS + 6, cellPx * ROWS + 6, 3, true);
}

void BreakoutActivity::drawInfo() const {
  const int pageWidth = renderer.getScreenWidth();
  renderer.fillRect(0, infoY - 2, pageWidth, INFO_HEIGHT, false);
  char line[128];
  snprintf(line, sizeof(line), "%s %d   %s %d   %s %ld   %s %ld", tr(STR_GAME_BREAKOUT_LIVES), lives,
           tr(STR_GAME_LEVEL), level, tr(STR_GAME_SCORE), score, tr(STR_GAME_BEST), bestScore);
  renderer.drawCenteredText(SMALL_FONT_ID, infoY + 4, line);
}

// Cartel de inicio, pausa, nivel terminado o final, encima del campo.
void BreakoutActivity::drawOverlay() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int w = pageWidth - 64;
  const int h = state == State::START ? 260 : 230;
  const int x = (pageWidth - w) / 2;
  const int y = (pageHeight - h) / 2 - 40;
  char buf[96];

  renderer.fillRoundedRect(x, y, w, h, 16, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 16, true);

  const char* title = tr(STR_GAME_BREAKOUT);
  if (state == State::PAUSED) title = tr(STR_GAME_BREAKOUT_PAUSE);
  if (state == State::LEVEL_DONE) title = tr(STR_GAME_BREAKOUT_LEVEL_DONE);
  if (state == State::OVER) title = tr(STR_GAME_OVER);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 24, title, true, EpdFontFamily::BOLD);

  if (state == State::START) {
    UITheme::drawCenteredWrappedText(renderer, Rect{x + 16, y + 70, w - 32, h - 100}, UI_10_FONT_ID,
                                     tr(STR_GAME_BREAKOUT_HELP), 5);
    return;
  }

  int ty = y + 78;
  snprintf(buf, sizeof(buf), "%s  %ld", tr(STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf);
  ty += 42;
  snprintf(buf, sizeof(buf), "%s  %d", tr(STR_GAME_LEVEL), level);
  renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
  ty += 36;
  if (state == State::OVER) {
    snprintf(buf, sizeof(buf), "%s  %ld", tr(STR_GAME_BEST), bestScore);
  } else {
    snprintf(buf, sizeof(buf), "%s  %d", tr(STR_GAME_BREAKOUT_LIVES), lives);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
}

// Las ayudas dicen siempre lo que hace cada botón AHORA.
void BreakoutActivity::drawHints() const {
  switch (state) {
    case State::SERVE: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_BREAKOUT_PAUSE), tr(STR_GAME_BREAKOUT_SERVE),
                                                tr(STR_GAME_BREAKOUT_LEFT), tr(STR_GAME_BREAKOUT_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PLAYING: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_BREAKOUT_PAUSE), tr(STR_GAME_BREAKOUT_PAUSE),
                                                tr(STR_GAME_BREAKOUT_LEFT), tr(STR_GAME_BREAKOUT_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PAUSED: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_CONTINUE), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::LEVEL_DONE: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_CONTINUE), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    default: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }
}

void BreakoutActivity::render(RenderLock&&) {
  layout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  // Regla del panel: parcial rápido para cada paso y uno limpio cada doce. El
  // limpio siempre rehace la pantalla entera, así el buffer no arrastra basura.
  const bool clean = fullRepaint || partialCount + 1 >= PARTIALS_BEFORE_CLEAN;
  // Nada cambió desde el último repintado: no se gasta un refresco de panel.
  if (!clean && dirtyCount == 0 && !infoDirty) return;

  if (clean) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_BREAKOUT));
    drawBoardFrame();
    for (int idx = 0; idx < CELLS; ++idx) {
      const int c = idx % COLS;
      const int r = idx / COLS;
      const bool hasBall = state != State::START && c == ballCol && r == ballRow;
      const int br = r - BRICK_TOP;
      const bool hasBrick = br >= 0 && br < brickRows && bricks[static_cast<size_t>(br * BCOLS + c / 2)] > 0;
      // La fila de la paleta va entera: ahí también se dibuja la raya del piso.
      if (hasBall || hasBrick || r == PADDLE_ROW) drawCell(idx);
    }
    drawInfo();
    if (state != State::PLAYING && state != State::SERVE) drawOverlay();
    drawHints();
    partialCount = 0;
  } else {
    for (int i = 0; i < dirtyCount; ++i) drawCell(static_cast<int>(dirty[static_cast<size_t>(i)]));
    if (infoDirty) drawInfo();
    ++partialCount;
  }
  dirtyCount = 0;
  infoDirty = false;
  fullRepaint = false;

  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
