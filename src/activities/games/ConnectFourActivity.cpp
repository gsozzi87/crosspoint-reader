#include "ConnectFourActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/Selection.h"

namespace {
constexpr int SIDE_MARGIN = 20;
constexpr int CURSOR_BAND = 74;  // franja de arriba con la flecha y la ficha
constexpr int INFO_BAND = 116;   // turno, marcador y modo, debajo del tablero

// Las cuatro direcciones de una línea (la de vuelta sale de restar).
constexpr int LINE_DR[4] = {0, 1, 1, 1};
constexpr int LINE_DC[4] = {1, 0, 1, -1};

// Columnas de adentro para afuera: con la poda alfa-beta, mirar primero el
// centro (que es lo mejor en este juego) corta muchísimas ramas.
constexpr int COL_ORDER[7] = {3, 2, 4, 1, 5, 0, 6};

// Ventanas de a cuatro: lo que vale tener 2, 3 o 4 fichas propias sin que el
// otro meta ninguna. Las del rival pesan un poco más, así la máquina tapa antes
// de atacar.
constexpr int MINE_SCORE[5] = {0, 1, 10, 50, 0};
constexpr int OPP_SCORE[5] = {0, 1, 12, 60, 0};
constexpr int CENTER_SCORE = 6;
}  // namespace

// ----------------------------------------------------------------- partida --

void ConnectFourActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::MODE;
  forceClean = true;
  requestUpdate();
}

void ConnectFourActivity::newGame() {
  board.fill(0);
  cursorCol = COLS / 2;
  turn = 1;
  winner = 0;
  drawn = false;
  haveWinLine = false;
  lastDropIdx = -1;
  moveNumber = 0;
  aiPrepared = false;
  state = State::PLAYING;
  forceClean = true;
  requestUpdate();
}

void ConnectFourActivity::moveCursor(const int dir) {
  const int next = dir > 0 ? ButtonNavigator::nextIndex(cursorCol, COLS) : ButtonNavigator::previousIndex(cursorCol, COLS);
  if (next == cursorCol) return;
  cursorCol = next;
  requestUpdate();
}

void ConnectFourActivity::dropPiece() {
  const int row = dropRow(board, cursorCol);
  if (row < 0) return;  // columna llena: no pasa nada
  board[static_cast<size_t>(row * COLS + cursorCol)] = turn;
  lastDropIdx = row * COLS + cursorCol;
  ++moveNumber;
  finishTurn(row, cursorCol);
}

// Cierra la jugada: mira si ganó, si quedó empate o si le toca al otro (y si el
// otro es la máquina, arranca a pensar).
void ConnectFourActivity::finishTurn(const int row, const int col) {
  if (winLineThrough(board, row, col, turn, winLine.data())) {
    haveWinLine = true;
    winner = turn;
    if (winner == 1) {
      ++winsP1;
    } else {
      ++winsP2;
    }
    gameOver();
    return;
  }
  if (boardFull(board)) {
    drawn = true;
    gameOver();
    return;
  }
  turn = static_cast<int8_t>(3 - turn);
  if (mode == Mode::VS_MACHINE && turn == 2) {
    state = State::AI_TURN;
    aiPrepared = false;
    requestUpdate();  // pinta "Pensando..." antes de arrancar la búsqueda
    return;
  }
  state = State::PLAYING;
  requestUpdate();
}

void ConnectFourActivity::gameOver() {
  state = State::OVER;
  forceClean = true;
  requestUpdate();
}

// ----------------------------------------------------------------- reglas --

int ConnectFourActivity::dropRow(const Board& b, const int col) {
  if (col < 0 || col >= COLS) return -1;
  for (int r = ROWS - 1; r >= 0; --r) {
    if (b[static_cast<size_t>(r * COLS + col)] == 0) return r;
  }
  return -1;
}

// ¿Hay cuatro en línea pasando por (row, col)? Se cuenta para los dos lados de
// cada una de las cuatro direcciones. Si `out` no es nulo, deja ahí las cuatro
// casillas desde un extremo, para poder marcarlas.
bool ConnectFourActivity::winLineThrough(const Board& b, const int row, const int col, const int8_t player,
                                         uint8_t* out) {
  if (!inBoard(row, col) || player == 0) return false;
  if (b[static_cast<size_t>(row * COLS + col)] != player) return false;
  for (int d = 0; d < 4; ++d) {
    int count = 1;
    int r = row + LINE_DR[d];
    int c = col + LINE_DC[d];
    while (inBoard(r, c) && b[static_cast<size_t>(r * COLS + c)] == player) {
      ++count;
      r += LINE_DR[d];
      c += LINE_DC[d];
    }
    int sr = row - LINE_DR[d];
    int sc = col - LINE_DC[d];
    while (inBoard(sr, sc) && b[static_cast<size_t>(sr * COLS + sc)] == player) {
      ++count;
      sr -= LINE_DR[d];
      sc -= LINE_DC[d];
    }
    if (count < 4) continue;
    if (out != nullptr) {
      sr += LINE_DR[d];  // volver al primer casillero de la línea
      sc += LINE_DC[d];
      for (int k = 0; k < 4; ++k) {
        out[k] = static_cast<uint8_t>(sr * COLS + sc);
        sr += LINE_DR[d];
        sc += LINE_DC[d];
      }
    }
    return true;
  }
  return false;
}

bool ConnectFourActivity::boardFull(const Board& b) {
  for (int c = 0; c < COLS; ++c) {
    if (b[static_cast<size_t>(c)] == 0) return false;  // fila de arriba libre
  }
  return true;
}

// Suma de todas las ventanas de a cuatro (horizontales, verticales y las dos
// diagonales), más un premio por el centro. Positivo = mejor para `side`.
int ConnectFourActivity::evaluate(const Board& b, const int8_t side) {
  const int8_t other = static_cast<int8_t>(3 - side);
  int score = 0;
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      for (int d = 0; d < 4; ++d) {
        const int er = r + 3 * LINE_DR[d];
        const int ec = c + 3 * LINE_DC[d];
        if (!inBoard(er, ec)) continue;
        int mine = 0, opp = 0;
        for (int k = 0; k < 4; ++k) {
          const int8_t v = b[static_cast<size_t>((r + k * LINE_DR[d]) * COLS + (c + k * LINE_DC[d]))];
          if (v == side) ++mine;
          if (v == other) ++opp;
        }
        if (mine > 0 && opp > 0) continue;  // ventana muerta para los dos
        if (mine > 0) score += MINE_SCORE[mine];
        if (opp > 0) score -= OPP_SCORE[opp];
      }
    }
  }
  for (int r = 0; r < ROWS; ++r) {
    const int8_t v = b[static_cast<size_t>(r * COLS + COLS / 2)];
    if (v == side) score += CENTER_SCORE;
    if (v == other) score -= CENTER_SCORE;
  }
  return score;
}

// ---------------------------------------------------------------- máquina --

int ConnectFourActivity::pickDepth() const {
  int empty = 0;
  for (int i = 0; i < CELLS; ++i) {
    if (board[static_cast<size_t>(i)] == 0) ++empty;
  }
  return empty <= 14 ? END_DEPTH : BASE_DEPTH;
}

// Negamax con poda alfa-beta. El tablero se toca y se destoca en el lugar (no
// se copia) y ganar vale menos cuanto más lejos está, así prefiere la victoria
// más corta y la derrota más larga.
int ConnectFourActivity::negamax(Board& b, const int8_t side, const int depth, int alpha, const int beta,
                                 const int ply) {
  if (depth <= 0) return evaluate(b, side);
  int best = -INF_SCORE;
  bool any = false;
  for (int i = 0; i < COLS; ++i) {
    const int col = COL_ORDER[i];
    const int row = dropRow(b, col);
    if (row < 0) continue;
    any = true;
    const size_t idx = static_cast<size_t>(row * COLS + col);
    b[idx] = side;
    int v;
    if (winLineThrough(b, row, col, side, nullptr)) {
      v = WIN_SCORE - ply;
    } else {
      v = -negamax(b, static_cast<int8_t>(3 - side), depth - 1, -beta, -alpha, ply + 1);
    }
    b[idx] = 0;
    if (v > best) best = v;
    if (best > alpha) alpha = best;
    if (alpha >= beta) break;  // poda
  }
  if (!any) return 0;  // tablero lleno: empate
  return best;
}

// Una columna de la raíz por pasada del loop: así la UI nunca se cuelga.
void ConnectFourActivity::stepAi() {
  if (!aiPrepared) {
    rootCount = 0;
    for (int i = 0; i < COLS; ++i) {
      const int col = COL_ORDER[i];
      if (dropRow(board, col) >= 0) rootCols[static_cast<size_t>(rootCount++)] = static_cast<uint8_t>(col);
    }
    if (rootCount == 0) {  // no debería pasar: el empate se detecta antes
      drawn = true;
      gameOver();
      return;
    }
    aiPrepared = true;
    aiIndex = 0;
    aiAlpha = -INF_SCORE;
    aiBestScore = -INF_SCORE;
    aiBestCol = rootCols[0];
    aiTies = 0;
    aiDepth = pickDepth();
    return;
  }

  if (aiIndex < rootCount) {
    const unsigned long t0 = millis();
    const int col = rootCols[static_cast<size_t>(aiIndex)];
    const int row = dropRow(board, col);
    const size_t idx = static_cast<size_t>(row * COLS + col);
    board[idx] = 2;
    int v;
    if (winLineThrough(board, row, col, 2, nullptr)) {
      v = WIN_SCORE - 1;
    } else {
      v = -negamax(board, 1, aiDepth - 1, -INF_SCORE, -aiAlpha, 1);
    }
    board[idx] = 0;
    if (v > aiBestScore) {
      aiBestScore = v;
      aiBestCol = col;
      aiTies = 1;
      if (v > aiAlpha) aiAlpha = v;
    } else if (v == aiBestScore) {
      // Entre columnas igual de buenas elige al azar: dos partidas no salen iguales.
      ++aiTies;
      if (random(aiTies) == 0) aiBestCol = col;
    }
    const unsigned long spent = millis() - t0;
    if (spent > ROOT_BUDGET_MS && aiDepth > 4) --aiDepth;  // válvula de escape
    ++aiIndex;
    return;
  }

  const int col = aiBestCol;
  const int row = dropRow(board, col);
  aiPrepared = false;
  if (row < 0) {  // imposible, pero no se juega una columna llena
    drawn = true;
    gameOver();
    return;
  }
  board[static_cast<size_t>(row * COLS + col)] = 2;
  lastDropIdx = row * COLS + col;
  cursorCol = col;
  ++moveNumber;
  finishTurn(row, col);
}

// -------------------------------------------------------------------- loop --

void ConnectFourActivity::loop() {
  if (state != State::MODE && mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    state = State::MODE;
    forceClean = true;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::AI_TURN) {
    stepAi();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::MODE:
        mode = modeCursor == 0 ? Mode::TWO_PLAYERS : Mode::VS_MACHINE;
        newGame();
        break;
      case State::PLAYING:
        dropPiece();
        break;
      case State::OVER:
        newGame();
        break;
      default:
        break;
    }
    return;
  }

  if (state == State::MODE) {
    buttonNavigator.onNext([this] {
      modeCursor = ButtonNavigator::nextIndex(modeCursor, MODE_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      modeCursor = ButtonNavigator::previousIndex(modeCursor, MODE_COUNT);
      requestUpdate();
    });
    return;
  }

  if (state != State::PLAYING) return;
  // La palanca es vertical pero las columnas van a los costados: arriba a la
  // izquierda, abajo a la derecha (los hints lo dicen).
  buttonNavigator.onPrevious([this] { moveCursor(-1); });
  buttonNavigator.onNext([this] { moveCursor(1); });
}

// ------------------------------------------------------------------ dibujo --

void ConnectFourActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight;
  // Siempre libre abajo lo que ocupan los hints, si no la última fila se tapa.
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  int cell = (pageWidth - 2 * SIDE_MARGIN) / COLS;
  const int byHeight = (bottom - top - CURSOR_BAND - INFO_BAND) / ROWS;
  if (byHeight < cell) cell = byHeight;
  if (cell > MAX_CELL) cell = MAX_CELL;
  if (cell < 16) cell = 16;

  cellPx = cell;
  const int boardH = cell * ROWS;
  int free = bottom - top - CURSOR_BAND - INFO_BAND - boardH;
  if (free < 0) free = 0;
  cursorY = top + free / 2;
  boardY = cursorY + CURSOR_BAND;
  boardX = (pageWidth - cell * COLS) / 2;
  infoY = boardY + boardH + 24;
}

void ConnectFourActivity::fillCircle(const int cx, const int cy, const int r, const bool on) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, on);
  }
}

void ConnectFourActivity::drawRing(const int cx, const int cy, const int outer, const int inner) const {
  fillCircle(cx, cy, outer, true);
  fillCircle(cx, cy, inner > 1 ? inner : 1, false);
}

// Sin color: el jugador 1 es un disco lleno y el jugador 2 (o la máquina) un
// anillo hueco, igual que en las damas. El agujero vacío es una circunferencia
// fina, que no se confunde con ninguna de las dos.
void ConnectFourActivity::drawDisc(const int cx, const int cy, const int r, const int8_t player) const {
  fillCircle(cx, cy, r + 2, false);  // halo blanco: despega la ficha de la grilla
  if (player == 0) {
    drawRing(cx, cy, r, r - 2);
    return;
  }
  if (player == 1) {
    fillCircle(cx, cy, r, true);
    return;
  }
  drawRing(cx, cy, r, r - r / 3 - 2);
}

void ConnectFourActivity::drawBoard() const {
  const int cell = cellPx;
  const int w = cell * COLS;
  const int h = cell * ROWS;
  int r = cell / 2 - 6;
  if (r < 6) r = 6;

  // Marco doble, como el tablero de verdad.
  renderer.drawRect(boardX - 8, boardY - 8, w + 16, h + 16, 4, true);
  renderer.drawRect(boardX - 2, boardY - 2, w + 4, h + 4, 2, true);
  for (int i = 1; i < COLS; ++i) renderer.drawLine(boardX + i * cell, boardY, boardX + i * cell, boardY + h - 1, true);
  for (int i = 1; i < ROWS; ++i) renderer.drawLine(boardX, boardY + i * cell, boardX + w - 1, boardY + i * cell, true);

  for (int idx = 0; idx < CELLS; ++idx) {
    const int row = idx / COLS;
    const int col = idx % COLS;
    drawDisc(boardX + col * cell + cell / 2, boardY + row * cell + cell / 2, r, board[static_cast<size_t>(idx)]);
  }

  // La última ficha soltada: un cuadradito en la esquina de su casilla, para no
  // perderla de vista cuando juega la máquina.
  if (lastDropIdx >= 0 && !haveWinLine) {
    const int x = boardX + (lastDropIdx % COLS) * cell;
    const int y = boardY + (lastDropIdx / COLS) * cell;
    renderer.fillRect(x + 4, y + 4, 7, 7, true);
  }

  if (!haveWinLine) return;

  // Línea ganadora: una raya gruesa que une los extremos y un anillo alrededor
  // de cada ficha (el hueco blanco corta la raya justo sobre las fichas).
  const int firstX = boardX + (winLine[0] % COLS) * cell + cell / 2;
  const int firstY = boardY + (winLine[0] / COLS) * cell + cell / 2;
  const int lastX = boardX + (winLine[3] % COLS) * cell + cell / 2;
  const int lastY = boardY + (winLine[3] / COLS) * cell + cell / 2;
  renderer.drawLine(firstX, firstY, lastX, lastY, 5, true);
  for (int k = 0; k < 4; ++k) {
    const int idx = winLine[static_cast<size_t>(k)];
    const int cx = boardX + (idx % COLS) * cell + cell / 2;
    const int cy = boardY + (idx / COLS) * cell + cell / 2;
    fillCircle(cx, cy, r + 7, true);
    fillCircle(cx, cy, r + 4, false);
    drawDisc(cx, cy, r, board[static_cast<size_t>(idx)]);
  }
}

// Arriba del tablero: la ficha que se va a soltar y una flecha en la columna.
// Todo se cuelga del borde de arriba del tablero (y el tamaño de la ficha se
// recorta a la franja) para que la flecha nunca pise el marco.
void ConnectFourActivity::drawCursor() const {
  if (state != State::PLAYING) return;
  const int cell = cellPx;
  const int cx = boardX + cursorCol * cell + cell / 2;
  int r = cell / 2 - 8;
  const int maxR = (CURSOR_BAND - 36) / 2;
  if (r > maxR) r = maxR;
  if (r < 5) r = 5;
  const int tipY = boardY - 14;   // la punta de la flecha, arriba del marco
  const int baseY = tipY - 12;
  const int cy = baseY - 8 - r;
  drawDisc(cx, cy, r, turn);
  const int xs[3] = {cx - 10, cx + 10, cx};
  const int ys[3] = {baseY, baseY, tipY};
  renderer.fillPolygon(xs, ys, 3, true);
  // Columna llena: una raya cruzada sobre la ficha, en el color que se ve
  // encima de esa ficha (blanca sobre el disco lleno, negra sobre el anillo).
  if (dropRow(board, cursorCol) < 0) renderer.fillRect(cx - r, cy - 2, 2 * r, 4, turn != 1);
}

const char* ConnectFourActivity::playerName(const int8_t player) const {
  if (player == 1) return tr(STR_GAME_CONNECT4_P1);
  return mode == Mode::VS_MACHINE ? tr(STR_GAME_CONNECT4_MACHINE) : tr(STR_GAME_CONNECT4_P2);
}

void ConnectFourActivity::drawInfo() const {
  char line[128];
  const char* title;
  if (state == State::AI_TURN) {
    title = tr(STR_GAME_THINKING);
  } else if (state == State::OVER) {
    title = drawn ? tr(STR_GAME_DRAW) : nullptr;
  } else {
    title = nullptr;
  }
  if (title != nullptr) {
    renderer.drawCenteredText(UI_12_FONT_ID, infoY, title, true, EpdFontFamily::BOLD);
  } else if (state == State::OVER) {
    snprintf(line, sizeof(line), tr(STR_GAME_CONNECT4_WINS), playerName(winner));
    renderer.drawCenteredText(UI_12_FONT_ID, infoY, line, true, EpdFontFamily::BOLD);
  } else {
    snprintf(line, sizeof(line), tr(STR_GAME_CONNECT4_TURN), playerName(turn));
    renderer.drawCenteredText(UI_12_FONT_ID, infoY, line, true, EpdFontFamily::BOLD);
  }

  if (state == State::PLAYING) {
    renderer.drawCenteredText(UI_10_FONT_ID, infoY + 34, tr(STR_GAME_CONNECT4_COLUMN));
  }

  snprintf(line, sizeof(line), "%s %d  -  %d %s", tr(STR_GAME_CONNECT4_P1), winsP1, winsP2,
           mode == Mode::VS_MACHINE ? tr(STR_GAME_CONNECT4_MACHINE) : tr(STR_GAME_CONNECT4_P2));
  renderer.drawCenteredText(SMALL_FONT_ID, infoY + 66, line);
  snprintf(line, sizeof(line), "%s %d", tr(STR_GAME_MOVES), moveNumber);
  renderer.drawCenteredText(SMALL_FONT_ID, infoY + 88, line);
}

// Pantalla de modo: dos jugadores en el mismo aparato, o contra la máquina.
void ConnectFourActivity::drawModeScreen() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight;
  const int rowH = 74;
  const int listTop = top + 120;

  UITheme::drawCenteredWrappedText(renderer, Rect{SIDE_MARGIN, top + 24, pageWidth - 2 * SIDE_MARGIN, 80},
                                   UI_10_FONT_ID, tr(STR_GAME_CONNECT4_HELP), 3);

  for (int i = 0; i < MODE_COUNT; ++i) {
    const int y = listTop + i * rowH;
    const bool sel = i == modeCursor;
    if (sel) {
      drawSelectionRow(renderer, SIDE_MARGIN, y, pageWidth - 2 * SIDE_MARGIN, rowH - 12, 12);
    } else {
      renderer.drawRoundedRect(SIDE_MARGIN, y, pageWidth - 2 * SIDE_MARGIN, rowH - 12, 2, 12, true);
    }
    const char* label = i == 0 ? tr(STR_GAME_CONNECT4_TWO) : tr(STR_GAME_CONNECT4_CPU);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 16, label, SELECTION_INK, EpdFontFamily::BOLD);
  }

  // Muestra las dos fichas para que se entienda quién es quién antes de jugar.
  const int y = listTop + MODE_COUNT * rowH + 30;
  const int r = 20;
  drawDisc(pageWidth / 2 - 90, y, r, 1);
  drawDisc(pageWidth / 2 + 90, y, r, 2);
  renderer.drawText(SMALL_FONT_ID, pageWidth / 2 - 90 + r + 8, y - 8, tr(STR_GAME_CONNECT4_P1));
  renderer.drawText(SMALL_FONT_ID, pageWidth / 2 + 90 + r + 8, y - 8,
                    modeCursor == 1 ? tr(STR_GAME_CONNECT4_MACHINE) : tr(STR_GAME_CONNECT4_P2));
}

// Las ayudas dicen siempre lo que hace cada botón AHORA.
void ConnectFourActivity::drawHints() const {
  switch (state) {
    case State::MODE: {
      const auto labels =
          mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::PLAYING: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_CONNECT4_DROP),
                                                tr(STR_GAME_CONNECT4_LEFT), tr(STR_GAME_CONNECT4_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::AI_TURN: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), "", "", "");
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

void ConnectFourActivity::render(RenderLock&&) {
  layout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_CONNECT4));

  if (state == State::MODE) {
    drawModeScreen();
  } else {
    drawCursor();
    drawBoard();
    drawInfo();
  }
  drawHints();

  // Casi todo parcial; uno limpio cada tanto para que el panel no fantasmee.
  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
