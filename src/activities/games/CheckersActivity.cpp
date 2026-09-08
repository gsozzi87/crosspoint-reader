#include "CheckersActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Las cuatro diagonales.
constexpr int DR[4] = {-1, -1, 1, 1};
constexpr int DC[4] = {-1, 1, -1, 1};

// Valores de la evaluación (centipeones).
constexpr int MAN_VALUE = 100;
constexpr int KING_VALUE = 185;
constexpr int ADVANCE_VALUE = 6;   // por fila avanzada de un peón
constexpr int BACK_ROW_VALUE = 5;  // peón que se queda cuidando la fila propia
constexpr int CENTER_VALUE = 3;

constexpr int INFO_HEIGHT = 108;
constexpr int BOARD_MARGIN = 24;   // deja lugar al marco doble del tablero
constexpr int MAX_CELL = 54;
constexpr int FRAME_GAP = 8;       // separación entre el marco de afuera y el tablero
constexpr int HATCH_STEP = 5;      // paso de la trama de las casillas oscuras
}  // namespace

void CheckersActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  newGame();
}

// ---------------------------------------------------------------- partida --

void CheckersActivity::newGame() {
  board.fill(0);
  for (int sq = 0; sq < CELLS; ++sq) {
    const int r = rowOf(sq), c = colOf(sq);
    if (((r + c) & 1) == 0) continue;  // solo casillas oscuras
    if (r <= 2) board[sq] = -1;        // máquina arriba, avanza hacia abajo
    if (r >= 5) board[sq] = 1;         // jugador abajo, avanza hacia arriba
  }
  state = PICK_PIECE;
  result = NONE;
  moveNumber = 0;
  noProgress = 0;
  lastFrom = lastTo = -1;
  pieceCursor = destCursor = 0;
  destCount = 0;
  aiPrepared = false;
  partialCount = 0;
  forceClean = true;
  refreshHumanMoves();
  if (state != GAME_OVER) requestUpdate();
}

// Rearma la lista de fichas con movida legal. Sin movidas, el jugador pierde.
void CheckersActivity::refreshHumanMoves() {
  generateMoves(board, 1, humanMoves);
  pieceCount = 0;
  destCount = 0;
  // De abajo hacia arriba: la primera de la lista es la más cercana al jugador.
  for (int sq = CELLS - 1; sq >= 0; --sq) {
    bool used = false;
    for (uint8_t i = 0; i < humanMoves.count && !used; ++i) used = humanMoves.items[i].from() == sq;
    if (!used) continue;
    if (pieceCount < pieceList.size()) pieceList[pieceCount++] = static_cast<uint8_t>(sq);
  }
  if (pieceCount == 0) {
    gameOver(LOST);
    return;
  }
  if (pieceCursor >= pieceCount) pieceCursor = 0;
}

// Con una sola ficha (o un solo destino) la palanca no mueve nada: si igual se
// repintara, el panel destellaría de gusto.
void CheckersActivity::moveCursor(const int dir) {
  if (state == PICK_PIECE && pieceCount > 1) {
    pieceCursor = dir > 0 ? ButtonNavigator::nextIndex(pieceCursor, pieceCount)
                          : ButtonNavigator::previousIndex(pieceCursor, pieceCount);
  } else if (state == PICK_MOVE && destCount > 1) {
    destCursor = dir > 0 ? ButtonNavigator::nextIndex(destCursor, destCount)
                         : ButtonNavigator::previousIndex(destCursor, destCount);
  } else {
    return;
  }
  requestUpdate();
}

void CheckersActivity::selectPiece() {
  if (pieceCount == 0) return;
  const uint8_t sq = pieceList[pieceCursor];
  destCount = 0;
  for (uint8_t i = 0; i < humanMoves.count; ++i) {
    if (humanMoves.items[i].from() != sq) continue;
    if (destCount < destMove.size()) destMove[destCount++] = i;
  }
  if (destCount == 0) return;
  destCursor = 0;
  state = PICK_MOVE;
  requestUpdate();
}

void CheckersActivity::confirmMove() {
  if (destCount == 0) return;
  const Move m = humanMoves.items[destMove[destCursor]];
  const bool progress = m.capture || isManMove(board, m);
  applyMove(board, m);
  noProgress = progress ? 0 : noProgress + 1;
  moveNumber++;
  lastFrom = lastTo = -1;
  destCount = 0;
  afterHumanMove();
}

void CheckersActivity::afterHumanMove() {
  if (noProgress >= NO_PROGRESS_DRAW) {
    gameOver(DRAW);
    return;
  }
  state = AI_TURN;
  aiPrepared = false;
  requestUpdate();  // pinta "pensando" antes de arrancar la búsqueda
}

void CheckersActivity::afterAiMove() {
  if (noProgress >= NO_PROGRESS_DRAW) {
    gameOver(DRAW);
    return;
  }
  state = PICK_PIECE;
  pieceCursor = 0;
  refreshHumanMoves();
  if (state != GAME_OVER) requestUpdate();
}

void CheckersActivity::gameOver(const Result r) {
  state = GAME_OVER;
  result = r;
  if (r == WON) wins++;
  forceClean = true;
  requestUpdate();
}

// ----------------------------------------------------------------- reglas --

// Un salto mueve dos filas y dos columnas, así que el promedio de los índices
// cae justo en la casilla comida.
void CheckersActivity::applyMove(Board& b, const Move& m) {
  if (m.len < 2) return;
  int8_t p = b[m.seq[0]];
  b[m.seq[0]] = 0;
  for (uint8_t i = 1; i < m.len; ++i) {
    if (m.capture) b[(m.seq[i - 1] + m.seq[i]) / 2] = 0;
  }
  const int dst = m.seq[m.len - 1];
  if (p == 1 && rowOf(dst) == 0) p = 2;
  if (p == -1 && rowOf(dst) == 7) p = -2;
  b[dst] = p;
}

bool CheckersActivity::isManMove(const Board& b, const Move& m) {
  const int8_t p = b[m.seq[0]];
  return p == 1 || p == -1;
}

// Captura obligatoria: si hay saltos, solo se devuelven saltos.
void CheckersActivity::generateMoves(const Board& b, const int side, MoveList& out) const {
  out.clear();
  for (int sq = 0; sq < CELLS; ++sq) {
    if (!belongsTo(b[sq], side)) continue;
    Move base;
    base.len = 1;
    base.seq[0] = static_cast<uint8_t>(sq);
    base.capture = true;
    addCaptures(b, sq, side, base, out);
  }
  if (out.count > 0) return;

  for (int sq = 0; sq < CELLS; ++sq) {
    const int8_t p = b[sq];
    if (!belongsTo(p, side)) continue;
    const int r = rowOf(sq), c = colOf(sq);
    const int forward = side > 0 ? -1 : 1;
    for (int d = 0; d < 4; ++d) {
      if (!isKing(p) && DR[d] != forward) continue;
      const int nr = r + DR[d], nc = c + DC[d];
      if (nr < 0 || nr > 7 || nc < 0 || nc > 7) continue;
      const int dst = nr * 8 + nc;
      if (b[dst] != 0) continue;
      Move m;
      m.seq[0] = static_cast<uint8_t>(sq);
      m.seq[1] = static_cast<uint8_t>(dst);
      m.len = 2;
      m.capture = false;
      out.add(m);
    }
  }
}

// Cadena de capturas: se sigue saltando mientras se pueda. Coronar termina la
// cadena (regla clásica), y la movida se guarda entera para poder mostrarla.
void CheckersActivity::addCaptures(const Board& b, const int sq, const int side, const Move& current,
                                   MoveList& out) const {
  const int8_t p = b[sq];
  const bool king = isKing(p);
  const int r = rowOf(sq), c = colOf(sq);
  const int forward = side > 0 ? -1 : 1;
  for (int d = 0; d < 4; ++d) {
    if (!king && DR[d] != forward) continue;
    const int mr = r + DR[d], mc = c + DC[d];
    const int tr2 = r + 2 * DR[d], tc2 = c + 2 * DC[d];
    if (tr2 < 0 || tr2 > 7 || tc2 < 0 || tc2 > 7) continue;
    const int8_t mid = b[mr * 8 + mc];
    if (mid == 0 || belongsTo(mid, side)) continue;
    const int dst = tr2 * 8 + tc2;
    if (b[dst] != 0) continue;
    if (current.len >= MAX_SEQ) continue;

    Board nb = b;
    nb[mr * 8 + mc] = 0;
    nb[sq] = 0;
    int8_t moved = p;
    bool crowned = false;
    if (!king && ((side > 0 && tr2 == 0) || (side < 0 && tr2 == 7))) {
      moved = static_cast<int8_t>(side > 0 ? 2 : -2);
      crowned = true;
    }
    nb[dst] = moved;

    Move next = current;
    next.seq[next.len++] = static_cast<uint8_t>(dst);
    next.capture = true;

    const uint8_t mark = out.count;
    if (!crowned && next.len < MAX_SEQ) addCaptures(nb, dst, side, next, out);
    if (out.count == mark) out.add(next);  // no había continuación: la cadena termina acá
  }
}

int CheckersActivity::evaluate(const Board& b) {
  int score = 0;
  for (int sq = 0; sq < CELLS; ++sq) {
    const int8_t p = b[sq];
    if (p == 0) continue;
    const int s = p > 0 ? 1 : -1;
    const int r = rowOf(sq), c = colOf(sq);
    if (isKing(p)) {
      score += s * KING_VALUE;
    } else {
      const int advance = p > 0 ? (7 - r) : r;
      score += s * (MAN_VALUE + advance * ADVANCE_VALUE);
      if ((p > 0 && r == 7) || (p < 0 && r == 0)) score += s * BACK_ROW_VALUE;
    }
    if (c >= 2 && c <= 5) score += s * CENTER_VALUE;
  }
  return score;
}

// ---------------------------------------------------------------- máquina --

int CheckersActivity::pickDepth() const {
  int pieces = 0;
  for (int sq = 0; sq < CELLS; ++sq)
    if (board[sq] != 0) pieces++;
  if (pieces <= 8) return MAX_DEPTH;
  if (pieces <= 12) return 7;
  if (pieces <= 18) return 6;
  return 5;
}

int CheckersActivity::negamax(const Board& b, const int side, const int depth, int alpha, const int beta,
                              const int ply) {
  if (ply >= MAX_PLY - 1) return evaluate(b) * side;
  MoveList& ml = plyMoves[ply];
  generateMoves(b, side, ml);
  if (ml.count == 0) return -WIN_SCORE + ply;  // sin movidas: el que juega pierde
  const bool captures = ml.items[0].capture;
  // Se corta al llegar a la profundidad, pero nunca en medio de un cambio.
  if ((depth <= 0 && !captures) || depth <= -CAPTURE_EXT) return evaluate(b) * side;

  int best = -INF_SCORE;
  for (uint8_t i = 0; i < ml.count; ++i) {
    Board nb = b;
    applyMove(nb, ml.items[i]);
    const int v = -negamax(nb, -side, depth - 1, -beta, -alpha, ply + 1);
    if (v > best) best = v;
    if (best > alpha) alpha = best;
    if (alpha >= beta) break;  // poda
  }
  return best;
}

// Una movida raíz por pasada del loop: así la UI nunca se cuelga y la jugada
// entera sale en bastante menos de un segundo.
void CheckersActivity::stepAi() {
  if (!aiPrepared) {
    generateMoves(board, -1, rootMoves);
    if (rootMoves.count == 0) {
      gameOver(WON);
      return;
    }
    aiPrepared = true;
    aiIndex = 0;
    aiAlpha = -INF_SCORE;
    aiBestScore = -INF_SCORE;
    aiBestIndex = 0;
    aiTies = 0;
    aiDepth = pickDepth();
    if (rootMoves.count == 1) aiIndex = rootMoves.count;  // única movida: no hay nada que pensar
    return;
  }

  if (aiIndex < rootMoves.count) {
    const unsigned long t0 = millis();
    Board nb = board;
    applyMove(nb, rootMoves.items[aiIndex]);
    const int v = -negamax(nb, 1, aiDepth - 1, -INF_SCORE, -aiAlpha, 1);
    if (v > aiBestScore) {
      aiBestScore = v;
      aiBestIndex = aiIndex;
      aiTies = 1;
      if (v > aiAlpha) aiAlpha = v;
    } else if (v == aiBestScore) {
      // Entre movidas igual de buenas elige al azar: dos partidas no salen iguales.
      aiTies++;
      if (random(aiTies) == 0) aiBestIndex = aiIndex;
    }
    const unsigned long spent = millis() - t0;
    if (spent > ROOT_BUDGET_MS && aiDepth > 3) aiDepth--;  // válvula de escape
    aiIndex++;
    return;
  }

  const Move chosen = rootMoves.items[aiBestIndex];
  const bool progress = chosen.capture || isManMove(board, chosen);
  applyMove(board, chosen);
  noProgress = progress ? 0 : noProgress + 1;
  lastFrom = chosen.from();
  lastTo = chosen.to();
  moveNumber++;
  aiPrepared = false;
  afterAiMove();
}

// ------------------------------------------------------------------- loop --

void CheckersActivity::loop() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    newGame();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == PICK_MOVE) {
      state = PICK_PIECE;
      destCount = 0;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (state == AI_TURN) {
    stepAi();
    return;
  }

  if (state == GAME_OVER) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) newGame();
    return;
  }

  buttonNavigator.onNext([this] { moveCursor(1); });
  buttonNavigator.onPrevious([this] { moveCursor(-1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == PICK_PIECE) {
      selectPiece();
    } else {
      confirmMove();
    }
  }
}

// ----------------------------------------------------------------- dibujo --

void CheckersActivity::fillCircle(const int cx, const int cy, const int r, const bool state) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, state);
  }
}

// Corona de tres puntas con su base: la marca de la dama. Entra en un cuadrado
// de lado 2r y se lee incluso en la casilla más chica.
void CheckersActivity::drawCrown(const int cx, const int cy, const int r, const bool state) const {
  if (r < 4) {
    renderer.fillRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, state);
    return;
  }
  const int half = r / 2;
  const int xs[7] = {cx - r, cx - half, cx, cx + half, cx + r, cx + r, cx - r};
  const int ys[7] = {cy - r, cy - r / 4, cy - r, cy - r / 4, cy - r, cy + half, cy + half};
  renderer.fillPolygon(xs, ys, 7, state);
  renderer.fillRect(cx - r, cy + half - 1, 2 * r + 1, r / 2 + 2, state);
}

// Trama de la casilla oscura: rayas en diagonal recortadas a mano contra la
// casilla (drawLine no recorta sola). En negro macizo la ficha desaparecía.
void CheckersActivity::hatchCell(const int x, const int y, const int cell) const {
  for (int d = -cell; d < cell; d += HATCH_STEP) {
    const int t0 = d < 0 ? -d : 0;
    const int t1 = cell - d < cell ? cell - d : cell;
    if (t1 > t0) renderer.drawLine(x + d + t0, y + t0, x + d + t1 - 1, y + t1 - 1, true);
  }
}

// Escuadras en las cuatro esquinas de una casilla: marcan los candidatos sin
// tapar la ficha ni confundirse con el marco del cursor.
void CheckersActivity::drawCornerTicks(const int x, const int y, const int cell, const int arm,
                                       const int thickness) const {
  const int t = thickness;
  renderer.fillRect(x, y, arm, t, true);
  renderer.fillRect(x, y, t, arm, true);
  renderer.fillRect(x + cell - arm, y, arm, t, true);
  renderer.fillRect(x + cell - t, y, t, arm, true);
  renderer.fillRect(x, y + cell - t, arm, t, true);
  renderer.fillRect(x, y + cell - arm, t, arm, true);
  renderer.fillRect(x + cell - arm, y + cell - t, arm, t, true);
  renderer.fillRect(x + cell - t, y + cell - arm, t, arm, true);
}

// Jugador: disco lleno. Máquina: anillo hueco. Las dos con un halo blanco
// alrededor para despegarlas de la trama de la casilla. La dama lleva una
// corona en el medio, en el color contrario al de la ficha.
void CheckersActivity::drawPiece(const int cx, const int cy, const int cell, const int8_t piece) const {
  const int r = cell / 2 - 5;
  if (r < 4) return;
  fillCircle(cx, cy, r + 2, false);  // halo: la trama no toca la ficha
  if (piece > 0) {
    fillCircle(cx, cy, r, true);
    if (isKing(piece)) drawCrown(cx, cy, r / 2, false);
  } else {
    fillCircle(cx, cy, r, true);
    fillCircle(cx, cy, r - 5 > 2 ? r - 5 : 2, false);
    if (isKing(piece)) drawCrown(cx, cy, r / 2 - 1, true);
  }
}

void CheckersActivity::drawBoard(const int left, const int top, const int cell) const {
  const int size = cell * 8;

  // Marco doble: uno grueso por fuera y una línea pegada al tablero.
  renderer.drawRect(left - FRAME_GAP, top - FRAME_GAP, size + 2 * FRAME_GAP, size + 2 * FRAME_GAP, 4, true);
  renderer.drawRect(left - 2, top - 2, size + 4, size + 4, 2, true);

  // Qué casilla está resaltada ahora mismo.
  int cursorSq = -1;
  int sourceSq = -1;
  if (state == PICK_PIECE && pieceCount > 0) cursorSq = pieceList[pieceCursor];
  if (state == PICK_MOVE && destCount > 0) {
    cursorSq = humanMoves.items[destMove[destCursor]].to();
    sourceSq = humanMoves.items[destMove[0]].from();
  }

  // Fondo: trama en las oscuras y blanco en las claras. Las dos casillas que
  // importan (la ficha elegida y el cursor) van sin trama: destramarlas es lo
  // que las hace inconfundibles sobre cualquier casilla.
  for (int sq = 0; sq < CELLS; ++sq) {
    const int r = rowOf(sq), c = colOf(sq);
    if (((r + c) & 1) == 0 || sq == cursorSq || sq == sourceSq) continue;
    hatchCell(left + c * cell, top + r * cell, cell);
  }

  // Grilla: una línea por casilla, así se cuentan las filas de un vistazo.
  for (int i = 1; i < 8; ++i) {
    renderer.drawLine(left + i * cell, top, left + i * cell, top + size - 1, true);
    renderer.drawLine(left, top + i * cell, left + size - 1, top + i * cell, true);
  }

  // La última movida de la máquina: un cuadradito macizo en dos esquinas
  // opuestas de la casilla, que no se confunde con las marcas del jugador.
  for (int sq = 0; sq < CELLS; ++sq) {
    if (sq != lastFrom && sq != lastTo) continue;
    const int x = left + colOf(sq) * cell, y = top + rowOf(sq) * cell;
    renderer.fillRect(x + 3, y + 3, 7, 7, true);
    renderer.fillRect(x + cell - 10, y + cell - 10, 7, 7, true);
  }

  for (int sq = 0; sq < CELLS; ++sq) {
    if (board[sq] == 0) continue;
    drawPiece(left + colOf(sq) * cell + cell / 2, top + rowOf(sq) * cell + cell / 2, cell, board[sq]);
  }

  if (state != PICK_PIECE && state != PICK_MOVE) return;

  // Candidatos: escuadras gruesas en las esquinas.
  if (state == PICK_PIECE) {
    for (uint8_t i = 0; i < pieceCount; ++i) {
      const int sq = pieceList[i];
      if (sq == cursorSq) continue;
      drawCornerTicks(left + colOf(sq) * cell, top + rowOf(sq) * cell, cell, cell / 3, 3);
    }
  } else {
    for (uint8_t i = 0; i < destCount; ++i) {
      const int sq = humanMoves.items[destMove[i]].to();
      if (sq == cursorSq) continue;
      drawCornerTicks(left + colOf(sq) * cell, top + rowOf(sq) * cell, cell, cell / 3, 3);
    }
  }

  // La ficha ya elegida: marco fino sobre su casilla sin trama (siempre tiene
  // la ficha encima, así que no se confunde con el cursor).
  if (sourceSq >= 0) {
    const int x = left + colOf(sourceSq) * cell, y = top + rowOf(sourceSq) * cell;
    renderer.drawRect(x + 1, y + 1, cell - 2, cell - 2, 2, true);
  }

  // El cursor: marco macizo pegado al borde de la casilla, sobre fondo blanco.
  if (cursorSq >= 0) {
    const int x = left + colOf(cursorSq) * cell, y = top + rowOf(cursorSq) * cell;
    renderer.drawRect(x, y, cell, cell, 4, true);
  }
}

void CheckersActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int reserved = metrics.buttonHintsHeight + metrics.verticalSpacing;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_CHECKERS));

  const int availW = pageWidth - 2 * BOARD_MARGIN;
  const int availH = pageHeight - headerBottom - INFO_HEIGHT - reserved - 24;
  int cell = (availW < availH ? availW : availH) / 8;
  if (cell > MAX_CELL) cell = MAX_CELL;
  const int size = cell * 8;
  const int boardLeft = (pageWidth - size) / 2;
  int boardTop = headerBottom + (pageHeight - reserved - headerBottom - size - INFO_HEIGHT) / 2;
  if (boardTop < headerBottom + 8) boardTop = headerBottom + 8;

  drawBoard(boardLeft, boardTop, cell);

  // Estado de la partida, debajo del tablero.
  const int infoTop = boardTop + size + 14;
  const char* title = tr(STR_GAME_YOUR_TURN);
  if (state == AI_TURN) title = tr(STR_GAME_THINKING);
  if (state == GAME_OVER)
    title = result == WON ? tr(STR_GAME_WON) : result == LOST ? tr(STR_GAME_LOST) : tr(STR_GAME_DRAW);
  renderer.drawCenteredText(UI_12_FONT_ID, infoTop, title, true, EpdFontFamily::BOLD);

  // Debajo, qué está eligiendo el jugador y en qué lugar de la lista va: sin eso
  // no se sabe si la palanca hizo algo.
  char sub[96] = "";
  if (state == PICK_PIECE && pieceCount > 0) {
    snprintf(sub, sizeof(sub), "%s  (%d/%d)", tr(STR_GAME_SELECT_PIECE), pieceCursor + 1, pieceCount);
  } else if (state == PICK_MOVE && destCount > 0) {
    snprintf(sub, sizeof(sub), "%s  (%d/%d)", tr(STR_GAME_SELECT_MOVE), destCursor + 1, destCount);
  } else if (state == GAME_OVER) {
    snprintf(sub, sizeof(sub), "%s", tr(STR_GAME_OVER));
  }
  if (sub[0] != '\0') renderer.drawCenteredText(UI_10_FONT_ID, infoTop + 32, sub);

  int human = 0, machine = 0;
  for (int sq = 0; sq < CELLS; ++sq) {
    if (board[sq] > 0) human++;
    if (board[sq] < 0) machine++;
  }
  char line[96];
  snprintf(line, sizeof(line), "%s  %d - %d", tr(STR_GAME_SCORE), human, machine);
  renderer.drawCenteredText(SMALL_FONT_ID, infoTop + 58, line);
  snprintf(line, sizeof(line), "%s %d   %s %d", tr(STR_GAME_MOVES), moveNumber, tr(STR_GAME_BEST), wins);
  renderer.drawCenteredText(SMALL_FONT_ID, infoTop + 80, line);

  // Las ayudas dicen siempre lo que hace cada botón AHORA: eligiendo la movida,
  // Atrás cancela la ficha en vez de salir del juego.
  const char* confirmLabel = state == GAME_OVER ? tr(STR_GAME_NEW) : state == AI_TURN ? "" : tr(STR_SELECT);
  const char* backLabel = state == PICK_MOVE ? tr(STR_CANCEL) : tr(STR_GAME_QUIT);
  const bool navigable = state == PICK_PIECE || state == PICK_MOVE;
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, navigable ? tr(STR_DIR_UP) : "",
                                            navigable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Casi todo parcial; uno limpio cada tanto para que el panel no fantasmee.
  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
