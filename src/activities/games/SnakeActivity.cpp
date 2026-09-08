#include "SnakeActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 12;
constexpr int INFO_HEIGHT = 34;  // línea de puntaje debajo del tablero

// Las cuatro direcciones, en orden de reloj: girar a la derecha es +1 y a la
// izquierda -1, que es todo lo que necesita el control de dos botones.
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
}  // namespace

long SnakeActivity::bestScore = 0;

// ----------------------------------------------------------------- partida --

void SnakeActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::START;
  fullRepaint = true;
  requestUpdate();
}

void SnakeActivity::startGame() {
  occupied.fill(0);
  body.fill(0);
  score = 0;
  eaten = 0;
  level = 1;
  won = false;
  dirtyCount = 0;
  infoDirty = false;

  // Arranca en el medio, mirando para arriba, con cuatro casilleros: el
  // primero de `body` es la cola y el último la cabeza.
  const int col = COLS / 2;
  const int row = ROWS / 2 + 1;
  for (int i = 0; i < START_LENGTH; ++i) {
    const int r = row + (START_LENGTH - 1 - i);
    const int idx = r * COLS + col;
    body[static_cast<size_t>(i)] = static_cast<uint16_t>(idx);
    occupied[static_cast<size_t>(idx)] = 1;
  }
  tailPos = 0;
  headPos = START_LENGTH - 1;
  length = START_LENGTH;
  headCol = col;
  headRow = row;
  dir = 0;

  foodIdx = -1;
  placeFood();

  state = State::PLAYING;
  lastStep = millis();
  fullRepaint = true;
  requestUpdate();
}

// Comida en un casillero libre elegido al azar. Si no queda ninguno, la
// viborita llenó la pantalla: eso es ganar.
void SnakeActivity::placeFood() {
  const int free = CAP - length;
  if (free <= 0) {
    foodIdx = -1;
    gameOver(true);
    return;
  }
  int pick = static_cast<int>(random(free));
  for (int idx = 0; idx < CAP; ++idx) {
    if (occupied[static_cast<size_t>(idx)]) continue;
    if (pick-- > 0) continue;
    foodIdx = idx;
    markDirty(idx);
    return;
  }
  foodIdx = -1;
}

unsigned long SnakeActivity::stepIntervalMs() const {
  const unsigned long faster = static_cast<unsigned long>(level - 1) * STEP_LEVEL_MS;
  return STEP_START_MS > STEP_MIN_MS + faster ? STEP_START_MS - faster : STEP_MIN_MS;
}

// Un casillero por paso. La cola se va antes de mirar el choque, así que meterse
// justo donde estaba la cola no mata (es lo que espera cualquiera que jugó a
// esto alguna vez).
void SnakeActivity::step() {
  lastStep = millis();
  const int nc = headCol + DX[dir];
  const int nr = headRow + DY[dir];
  if (nc < 0 || nc >= COLS || nr < 0 || nr >= ROWS) {
    gameOver(false);
    return;
  }
  const int idx = nr * COLS + nc;
  const int tailIdx = static_cast<int>(body[static_cast<size_t>(tailPos)]);
  const bool eating = idx == foodIdx;
  if (occupied[static_cast<size_t>(idx)] && !(idx == tailIdx && !eating)) {
    gameOver(false);
    return;
  }

  if (!eating) {
    occupied[static_cast<size_t>(tailIdx)] = 0;
    markDirty(tailIdx);
    tailPos = (tailPos + 1) % CAP;
    --length;
  }

  markDirty(static_cast<int>(body[static_cast<size_t>(headPos)]));  // la cabeza vieja pasa a ser cuerpo
  headPos = (headPos + 1) % CAP;
  body[static_cast<size_t>(headPos)] = static_cast<uint16_t>(idx);
  occupied[static_cast<size_t>(idx)] = 1;
  ++length;
  headCol = nc;
  headRow = nr;
  markDirty(idx);

  if (eating) {
    ++eaten;
    score += 10L * level;
    level = 1 + eaten / FOOD_PER_LEVEL;
    if (score > bestScore) bestScore = score;
    infoDirty = true;
    placeFood();
  }
  requestUpdate();
}

// La palanca gira: -1 a la izquierda, +1 a la derecha. Se repinta la cabeza
// sola para que el giro se vea antes del próximo paso.
void SnakeActivity::turn(const int delta) {
  dir = (dir + delta + 4) % 4;
  if (length > 0) markDirty(static_cast<int>(body[static_cast<size_t>(headPos)]));
  requestUpdate();
}

void SnakeActivity::gameOver(const bool win) {
  won = win;
  if (score > bestScore) bestScore = score;
  state = State::OVER;
  fullRepaint = true;
  requestUpdate();
}

// Lista de casilleros a repintar. Si se llena, se rehace la pantalla entera:
// más barato que perder un cambio y dejar basura en el tablero.
void SnakeActivity::markDirty(const int idx) {
  if (idx < 0 || idx >= CAP) return;
  for (int i = 0; i < dirtyCount; ++i) {
    if (dirty[static_cast<size_t>(i)] == static_cast<uint16_t>(idx)) return;
  }
  if (dirtyCount >= MAX_DIRTY) {
    fullRepaint = true;
    return;
  }
  dirty[static_cast<size_t>(dirtyCount++)] = static_cast<uint16_t>(idx);
}

// -------------------------------------------------------------------- loop --

void SnakeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == State::PLAYING) {
      // Atrás no tira la partida: primero pausa, y desde la pausa sale.
      state = State::PAUSED;
      fullRepaint = true;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::START:
      case State::OVER:
        startGame();
        break;
      case State::PLAYING:
        state = State::PAUSED;
        fullRepaint = true;
        requestUpdate();
        break;
      case State::PAUSED:
        state = State::PLAYING;
        lastStep = millis();
        fullRepaint = true;
        requestUpdate();
        break;
    }
    return;
  }

  if (state != State::PLAYING) return;

  // Un giro por toque (sin repetición al mantener): con repetición la viborita
  // giraría en redondo sola.
  buttonNavigator.onPreviousPress([this] { turn(-1); });
  buttonNavigator.onNextPress([this] { turn(1); });

  if (millis() - lastStep >= stepIntervalMs()) step();
}

// ------------------------------------------------------------------ dibujo --

void SnakeActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight;
  // Siempre libre abajo lo que ocupan los hints, si no la última fila se tapa.
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

void SnakeActivity::fillCircle(const int cx, const int cy, const int r, const bool on) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, on);
  }
}

// Cuerpo: cuadrado negro con un anillo blanco, así dos casilleros pegados se
// distinguen en blanco y negro. Cabeza: el mismo cuadrado con una punta blanca
// del lado para donde va.
void SnakeActivity::drawSegment(const int x, const int y, const bool head) const {
  const int cell = cellPx;
  int inset = cell / 8;
  if (inset < 2) inset = 2;
  renderer.fillRect(x + 1, y + 1, cell - 2, cell - 2, true);
  if (!head) {
    renderer.fillRect(x + inset + 1, y + inset + 1, cell - 2 * inset - 2, cell - 2 * inset - 2, false);
    renderer.fillRect(x + cell / 3, y + cell / 3, cell - 2 * (cell / 3), cell - 2 * (cell / 3), true);
    return;
  }
  const int cx = x + cell / 2;
  const int cy = y + cell / 2;
  const int arm = cell / 2 - 3;
  // Triángulo blanco apuntando para donde avanza.
  const int tipX = cx + DX[dir] * arm;
  const int tipY = cy + DY[dir] * arm;
  const int baseX = cx - DX[dir] * (arm / 3);
  const int baseY = cy - DY[dir] * (arm / 3);
  const int side = arm / 2;
  const int xs[3] = {tipX, baseX - DY[dir] * side, baseX + DY[dir] * side};
  const int ys[3] = {tipY, baseY - DX[dir] * side, baseY + DX[dir] * side};
  renderer.fillPolygon(xs, ys, 3, false);
}

// Comida: un disco con el centro blanco, redondo contra los cuadrados del
// cuerpo para que no haya que mirar dos veces.
void SnakeActivity::drawFood(const int x, const int y) const {
  const int cell = cellPx;
  const int cx = x + cell / 2;
  const int cy = y + cell / 2;
  int r = cell / 2 - 4;
  if (r < 3) r = 3;
  fillCircle(cx, cy, r, true);
  fillCircle(cx, cy, r / 3, false);
}

void SnakeActivity::drawCell(const int idx) const {
  if (idx < 0 || idx >= CAP) return;
  const int c = idx % COLS;
  const int r = idx / COLS;
  const int x = boardX + c * cellPx;
  const int y = boardY + r * cellPx;
  renderer.fillRect(x, y, cellPx, cellPx, false);  // borrar lo que había
  if (idx == foodIdx) {
    drawFood(x, y);
    return;
  }
  if (!occupied[static_cast<size_t>(idx)]) return;
  const bool head = length > 0 && static_cast<int>(body[static_cast<size_t>(headPos)]) == idx;
  drawSegment(x, y, head);
}

void SnakeActivity::drawBoardFrame() const {
  renderer.drawRect(boardX - 3, boardY - 3, cellPx * COLS + 6, cellPx * ROWS + 6, 3, true);
}

void SnakeActivity::drawInfo() const {
  const int pageWidth = renderer.getScreenWidth();
  renderer.fillRect(0, infoY - 2, pageWidth, INFO_HEIGHT, false);
  char line[96];
  snprintf(line, sizeof(line), "%s %ld   %s %d   %s %d   %s %ld", tr(STR_GAME_SCORE), score, tr(STR_GAME_LEVEL),
           level, tr(STR_GAME_SNAKE_LENGTH), length, tr(STR_GAME_BEST), bestScore);
  renderer.drawCenteredText(SMALL_FONT_ID, infoY + 4, line);
}

// Cartel de inicio, pausa o final, encima del tablero.
void SnakeActivity::drawOverlay() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int w = pageWidth - 64;
  const int h = state == State::START ? 260 : 230;
  const int x = (pageWidth - w) / 2;
  const int y = (pageHeight - h) / 2 - 40;
  char buf[96];

  renderer.fillRoundedRect(x, y, w, h, 16, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 16, true);

  const char* title = tr(STR_GAME_SNAKE);
  if (state == State::PAUSED) title = tr(STR_GAME_SNAKE_PAUSE);
  if (state == State::OVER) title = won ? tr(STR_GAME_WON) : tr(STR_GAME_OVER);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 24, title, true, EpdFontFamily::BOLD);

  if (state == State::START) {
    UITheme::drawCenteredWrappedText(renderer, Rect{x + 16, y + 70, w - 32, h - 100}, UI_10_FONT_ID,
                                     tr(STR_GAME_SNAKE_HELP), 5);
    return;
  }

  int ty = y + 78;
  snprintf(buf, sizeof(buf), "%s  %ld", tr(STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf);
  ty += 42;
  snprintf(buf, sizeof(buf), "%s  %d", tr(STR_GAME_LEVEL), level);
  renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
  ty += 36;
  snprintf(buf, sizeof(buf), "%s  %ld", tr(STR_GAME_BEST), bestScore);
  renderer.drawCenteredText(UI_10_FONT_ID, ty, buf);
}

// Las ayudas dicen siempre lo que hace cada botón AHORA.
void SnakeActivity::drawHints() const {
  switch (state) {
    case State::PLAYING: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_SNAKE_PAUSE), tr(STR_GAME_SNAKE_PAUSE),
                                                tr(STR_GAME_SNAKE_LEFT), tr(STR_GAME_SNAKE_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PAUSED: {
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

void SnakeActivity::render(RenderLock&&) {
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
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_SNAKE));
    drawBoardFrame();
    for (int idx = 0; idx < CAP; ++idx) {
      if (occupied[static_cast<size_t>(idx)] || idx == foodIdx) drawCell(idx);
    }
    drawInfo();
    if (state != State::PLAYING) drawOverlay();
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
