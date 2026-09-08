#include "SudokuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Pistas que quedan en el tablero según el nivel. Menos pistas = más difícil.
constexpr int CLUES[] = {46, 35, 28};
constexpr uint16_t ALL_DIGITS = 0x03FE;  // bits 1..9

// Tope de nodos para llenar la grilla completa (con MRV casi nunca hace falta
// retroceder, pero sin tope un caso raro colgaría el loop).
constexpr uint32_t FILL_BUDGET = 120000;
// Presupuesto total repartido entre las verificaciones de unicidad al sacar
// números, y tope por verificación. Si se acaba, se deja de sacar: el tablero
// queda más fácil pero la partida sale igual y la pantalla no se congela.
constexpr uint32_t DIG_BUDGET = 260000;
constexpr uint32_t DIG_BUDGET_PER_CELL = 30000;
}  // namespace

int SudokuActivity::bitCount(uint16_t bits) {
  int n = 0;
  for (; bits != 0; bits &= static_cast<uint16_t>(bits - 1)) ++n;
  return n;
}

void SudokuActivity::buildMasks() {
  rowMask.fill(0);
  colMask.fill(0);
  boxMask.fill(0);
  for (int i = 0; i < 81; ++i) {
    const uint8_t d = work[i];
    if (d == 0) continue;
    const uint16_t bit = static_cast<uint16_t>(1u << d);
    rowMask[i / 9] |= bit;
    colMask[i % 9] |= bit;
    boxMask[boxOf(i)] |= bit;
  }
}

uint16_t SudokuActivity::candidates(const int idx) const {
  const uint16_t used = static_cast<uint16_t>(rowMask[idx / 9] | colMask[idx % 9] | boxMask[boxOf(idx)]);
  return static_cast<uint16_t>(~used & ALL_DIGITS);
}

void SudokuActivity::place(const int idx, const int digit) {
  work[idx] = static_cast<uint8_t>(digit);
  const uint16_t bit = static_cast<uint16_t>(1u << digit);
  rowMask[idx / 9] |= bit;
  colMask[idx % 9] |= bit;
  boxMask[boxOf(idx)] |= bit;
}

void SudokuActivity::unplace(const int idx) {
  const uint8_t d = work[idx];
  if (d == 0) return;
  const uint16_t bit = static_cast<uint16_t>(~(1u << d));
  work[idx] = 0;
  rowMask[idx / 9] &= bit;
  colMask[idx % 9] &= bit;
  boxMask[boxOf(idx)] &= bit;
}

// Backtracking iterativo con MRV (siempre la celda con menos candidatos): la
// pila es un miembro, así que ni la recursión ni el stack de la tarea de la UI
// entran en juego. Con randomOrder prueba los candidatos en orden al azar, que
// es lo que hace variados los tableros.
int SudokuActivity::solve(const int limit, const bool randomOrder) {
  buildMasks();
  int solutions = 0;
  int depth = 0;
  bool descend = true;

  while (true) {
    if (descend) {
      int best = -1;
      int bestCount = 10;
      uint16_t bestCand = 0;
      for (int i = 0; i < 81; ++i) {
        if (work[i] != 0) continue;
        const uint16_t cand = candidates(i);
        const int count = bitCount(cand);
        if (count < bestCount) {
          bestCount = count;
          bestCand = cand;
          best = i;
          if (count <= 1) break;  // no hay nada mejor que una sola opción
        }
      }
      if (best < 0) {
        // No quedan celdas vacías: tablero resuelto.
        if (++solutions >= limit) return solutions;
      } else if (bestCount > 0) {
        solverStack[depth].cell = static_cast<uint8_t>(best);
        solverStack[depth].placed = 0;
        solverStack[depth].remaining = bestCand;
        ++depth;
      }
      // Si la celda no tiene candidatos (o ya contamos una solución) caemos al
      // bloque de abajo, que deshace y prueba la siguiente opción.
      descend = false;
    }

    if (depth == 0) return solutions;
    Frame& frame = solverStack[depth - 1];
    if (frame.placed != 0) {
      unplace(frame.cell);
      frame.placed = 0;
    }
    if (frame.remaining == 0) {
      --depth;  // se agotó: sigue el marco de abajo
      continue;
    }
    if (++nodes > nodeBudget) return -1;  // sin presupuesto: el que llama decide

    int digit = 0;
    if (randomOrder) {
      int pick = static_cast<int>(random(bitCount(frame.remaining)));
      for (int d = 1; d <= 9; ++d) {
        if ((frame.remaining & (1u << d)) == 0) continue;
        if (pick-- == 0) {
          digit = d;
          break;
        }
      }
    } else {
      for (int d = 1; d <= 9; ++d) {
        if ((frame.remaining & (1u << d)) != 0) {
          digit = d;
          break;
        }
      }
    }
    frame.remaining &= static_cast<uint16_t>(~(1u << digit));
    place(frame.cell, digit);
    frame.placed = static_cast<uint8_t>(digit);
    descend = true;
  }
}

// Genera un tablero válido y de solución única. Corre una sola vez, con topes,
// mientras la pantalla muestra "Pensando...".
void SudokuActivity::generate() {
  const unsigned long t0 = millis();

  // 1) Grilla completa por backtracking con orden al azar.
  Grid full{};
  bool filled = false;
  for (int attempt = 0; attempt < 4 && !filled; ++attempt) {
    work.fill(0);
    nodes = 0;
    nodeBudget = FILL_BUDGET;
    if (solve(1, true) == 1) {
      full = work;
      filled = true;
    }
  }
  if (!filled) {
    // Red de seguridad: patrón base (que siempre es un sudoku válido) con los
    // dígitos renombrados al azar. No debería hacer falta nunca.
    std::array<uint8_t, 10> map{};
    for (int d = 1; d <= 9; ++d) map[d] = static_cast<uint8_t>(d);
    for (int d = 9; d > 1; --d) {
      const int j = static_cast<int>(random(d)) + 1;
      const uint8_t tmp = map[d];
      map[d] = map[j];
      map[j] = tmp;
    }
    for (int r = 0; r < 9; ++r) {
      for (int c = 0; c < 9; ++c) {
        full[r * 9 + c] = map[((r % 3) * 3 + r / 3 + c) % 9 + 1];
      }
    }
  }

  // 2) Sacar números de a uno mientras la solución siga siendo única.
  given = full;
  std::array<uint8_t, 81> order{};
  for (int i = 0; i < 81; ++i) order[i] = static_cast<uint8_t>(i);
  for (int i = 80; i > 0; --i) {
    const int j = static_cast<int>(random(i + 1));
    const uint8_t tmp = order[i];
    order[i] = order[j];
    order[j] = tmp;
  }

  const int target = CLUES[level];
  int clues = 81;
  uint32_t budget = DIG_BUDGET;
  for (int i = 0; i < 81 && clues > target && budget > 0; ++i) {
    const int idx = order[i];
    const uint8_t saved = given[idx];
    given[idx] = 0;
    work = given;
    nodes = 0;
    nodeBudget = budget < DIG_BUDGET_PER_CELL ? budget : DIG_BUDGET_PER_CELL;
    const int found = solve(2, false);
    budget -= (nodes < budget ? nodes : budget);
    if (found == 1) {
      --clues;  // sigue siendo única: la celda queda libre
    } else {
      given[idx] = saved;  // ambigua (o sin presupuesto): la pista se queda
    }
  }

  LOG_DBG("SUDOKU", "tablero nivel %d con %d pistas en %lu ms", level + 1, clues, millis() - t0);
  startGame();
}

void SudokuActivity::startGame() {
  puzzle = given;
  editable.clear();
  editable.reserve(81);
  for (int i = 0; i < 81; ++i) {
    if (given[i] == 0) editable.push_back(static_cast<uint8_t>(i));
  }
  cursor = 0;
  pickOption = 0;
  moves = 0;
  startedAt = millis();
  elapsedS = 0;
  state = editable.empty() ? State::WON : State::PLAY;
  partialCount = 0;
  requestUpdate();
}

// Un dígito choca si se repite en su fila, su columna o su bloque. Las pistas
// nunca chocan (el tablero es válido), así que solo se marca lo que puso el
// usuario.
bool SudokuActivity::conflicts(const int idx) const {
  const uint8_t v = puzzle[idx];
  if (v == 0) return false;
  const int row = idx / 9;
  const int col = idx % 9;
  for (int k = 0; k < 9; ++k) {
    const int inRow = row * 9 + k;
    if (inRow != idx && puzzle[inRow] == v) return true;
    const int inCol = k * 9 + col;
    if (inCol != idx && puzzle[inCol] == v) return true;
  }
  const int boxRow = (row / 3) * 3;
  const int boxCol = (col / 3) * 3;
  for (int r = boxRow; r < boxRow + 3; ++r) {
    for (int c = boxCol; c < boxCol + 3; ++c) {
      const int other = r * 9 + c;
      if (other != idx && puzzle[other] == v) return true;
    }
  }
  return false;
}

bool SudokuActivity::solved() const {
  for (int i = 0; i < 81; ++i) {
    if (puzzle[i] == 0) return false;
  }
  for (int i = 0; i < 81; ++i) {
    if (conflicts(i)) return false;
  }
  return true;
}

// Después de escribir un número, saltar a la próxima celda vacía: se juega
// mucho más rápido que volver a recorrer todo con la palanca.
void SudokuActivity::advanceToNextEmpty() {
  const int total = static_cast<int>(editable.size());
  if (total == 0) return;
  for (int step = 1; step <= total; ++step) {
    const int next = (cursor + step) % total;
    if (puzzle[editable[next]] == 0) {
      cursor = next;
      return;
    }
  }
}

void SudokuActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::LEVEL;
  level = 0;
  partialCount = 0;
  requestUpdate();
}

void SudokuActivity::loop() {
  switch (state) {
    case State::GENERATING:
      // La pantalla ya mostró "Pensando..." (requestUpdateAndWait), así que acá
      // se puede bloquear el tiempo que tarde el generador.
      if (pendingGenerate) {
        pendingGenerate = false;
        generate();
      }
      return;

    case State::LEVEL:
      buttonNavigator.onNext([this] {
        level = ButtonNavigator::nextIndex(level, LEVEL_COUNT);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        level = ButtonNavigator::previousIndex(level, LEVEL_COUNT);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        state = State::GENERATING;
        pendingGenerate = true;
        requestUpdateAndWait();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      return;

    case State::PLAY: {
      const int total = static_cast<int>(editable.size());
      buttonNavigator.onNext([this, total] {
        cursor = ButtonNavigator::nextIndex(cursor, total);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this, total] {
        cursor = ButtonNavigator::previousIndex(cursor, total);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && total > 0) {
        const uint8_t current = puzzle[editable[cursor]];
        pickOption = current > 0 ? current - 1 : 0;
        state = State::PICK;
        requestUpdate();
        return;
      }
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
        state = State::LEVEL;  // partida nueva: vuelve a elegir el nivel
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      return;
    }

    case State::PICK:
      buttonNavigator.onNext([this] {
        pickOption = ButtonNavigator::nextIndex(pickOption, CLEAR_OPTION + 1);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        pickOption = ButtonNavigator::previousIndex(pickOption, CLEAR_OPTION + 1);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        const int idx = editable[cursor];
        const uint8_t before = puzzle[idx];
        puzzle[idx] = pickOption == CLEAR_OPTION ? 0 : static_cast<uint8_t>(pickOption + 1);
        if (puzzle[idx] != before) ++moves;
        if (puzzle[idx] != 0) advanceToNextEmpty();
        if (solved()) {
          elapsedS = (millis() - startedAt) / 1000;
          state = State::WON;
        } else {
          state = State::PLAY;
        }
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = State::PLAY;
        requestUpdate();
      }
      return;

    case State::WON:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        state = State::LEVEL;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      return;
  }
}

void SudokuActivity::drawBoard(const int gridLeft, const int gridTop, const int cell, const int highlight) const {
  const int size = cell * 9;

  // Líneas: finas entre celdas, gruesas en los bordes de los bloques de 3x3.
  for (int i = 0; i <= 9; ++i) {
    const int thickness = i % 3 == 0 ? 3 : 1;
    const int offset = thickness / 2;
    renderer.fillRect(gridLeft + i * cell - offset, gridTop - offset, thickness, size + 2 * offset);
    renderer.fillRect(gridLeft - offset, gridTop + i * cell - offset, size + 2 * offset, thickness);
  }

  const int textHeight = renderer.getTextHeight(UI_12_FONT_ID);
  for (int i = 0; i < 81; ++i) {
    const int x = gridLeft + (i % 9) * cell;
    const int y = gridTop + (i / 9) * cell;
    const bool selected = i == highlight;
    if (selected) renderer.fillRoundedRect(x + 3, y + 3, cell - 6, cell - 6, 6, Color::Black);

    const uint8_t v = puzzle[i];
    if (v != 0) {
      const bool isGiven = given[i] != 0;
      const auto style = isGiven ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      char buf[2] = {static_cast<char>('0' + v), '\0'};
      const int width = renderer.getTextWidth(UI_12_FONT_ID, buf, style);
      renderer.drawText(UI_12_FONT_ID, x + (cell - width) / 2, y + (cell - textHeight) / 2, buf, !selected, style);
      // Choque: triangulito en la esquina de arriba a la izquierda.
      if (!isGiven && conflicts(i)) {
        for (int k = 0; k < 9; ++k) renderer.fillRect(x + 5, y + 5 + k, 9 - k, 1, !selected);
      }
    }
  }
}

void SudokuActivity::drawPicker(const int gridLeft, const int cell, const int y) const {
  const int size = cell * 9;
  const int boxHeight = 46;
  const int textHeight = renderer.getTextHeight(UI_12_FONT_ID);
  for (int d = 1; d <= 9; ++d) {
    const int x = gridLeft + (d - 1) * cell;
    const bool selected = pickOption == d - 1;
    if (selected) {
      renderer.fillRoundedRect(x + 2, y, cell - 4, boxHeight, 6, Color::Black);
    } else {
      renderer.drawRoundedRect(x + 2, y, cell - 4, boxHeight, 1, 6, true);
    }
    char buf[2] = {static_cast<char>('0' + d), '\0'};
    const int width = renderer.getTextWidth(UI_12_FONT_ID, buf, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (cell - width) / 2, y + (boxHeight - textHeight) / 2, buf, !selected,
                      EpdFontFamily::BOLD);
  }

  // Última opción: borrar lo que haya en la celda.
  const int clearY = y + boxHeight + 10;
  const int clearHeight = 44;
  const bool clearSelected = pickOption == CLEAR_OPTION;
  if (clearSelected) {
    renderer.fillRoundedRect(gridLeft, clearY, size, clearHeight, 8, Color::Black);
  } else {
    renderer.drawRoundedRect(gridLeft, clearY, size, clearHeight, 1, 8, true);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, clearY + (clearHeight - renderer.getTextHeight(UI_10_FONT_ID)) / 2,
                            tr(STR_GAME_EMPTY_CELL), !clearSelected, EpdFontFamily::BOLD);
}

void SudokuActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_SUDOKU));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomLimit = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  // Las ayudas se arman por estado: eligiendo el número, Atrás cancela el
  // selector en vez de salir del juego.
  const auto labels = mappedInput.mapLabels(state == State::PICK ? tr(STR_CANCEL) : tr(STR_GAME_QUIT),
                                            state == State::LEVEL ? tr(STR_GAME_NEW) : tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  char buf[64];

  if (state == State::LEVEL) {
    const int rowHeight = 78;
    const int top = contentTop + 40;
    for (int i = 0; i < LEVEL_COUNT; ++i) {
      const int y = top + i * rowHeight;
      const bool selected = i == level;
      if (selected) renderer.fillRoundedRect(20, y, pageWidth - 40, rowHeight - 12, 10, Color::Black);
      snprintf(buf, sizeof(buf), "%s %d", tr(STR_GAME_LEVEL), i + 1);
      renderer.drawText(UI_12_FONT_ID, 40, y + 12, buf, !selected, EpdFontFamily::BOLD);
      // Dificultad en cuadraditos (i + 1 llenos de tres) + cuántas pistas trae.
      const int pipY = y + 42;
      for (int p = 0; p < LEVEL_COUNT; ++p) {
        const int px = 40 + p * 26;
        if (p <= i) {
          renderer.fillRect(px, pipY, 18, 14, !selected);
        } else {
          renderer.drawRect(px, pipY, 18, 14, 1, !selected);
        }
      }
      snprintf(buf, sizeof(buf), "%d", CLUES[i]);
      const int width = renderer.getTextWidth(UI_10_FONT_ID, buf);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 40 - width, pipY - 2, buf, !selected);
    }
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Pantalla entera nueva: refresco limpio.
    partialCount = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  if (state == State::GENERATING) {
    renderer.drawCenteredText(UI_12_FONT_ID, (contentTop + bottomLimit) / 2, tr(STR_GAME_THINKING), true,
                              EpdFontFamily::BOLD);
    GUI.drawButtonHints(renderer, "", "", "", "");
    partialCount = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int cell = (pageWidth - 32) / 9;
  const int gridSize = cell * 9;
  const int gridLeft = (pageWidth - gridSize) / 2;
  const int gridTop = contentTop + 10;
  const int highlight = (state == State::WON || editable.empty()) ? -1 : editable[cursor];
  drawBoard(gridLeft, gridTop, cell, highlight);

  const int belowGrid = gridTop + gridSize + 18;
  if (state == State::WON) {
    renderer.drawCenteredText(UI_12_FONT_ID, belowGrid, tr(STR_GAME_WON), true, EpdFontFamily::BOLD);
    snprintf(buf, sizeof(buf), "%s %02lu:%02lu   %s %d", tr(STR_GAME_TIME), elapsedS / 60, elapsedS % 60,
             tr(STR_GAME_MOVES), moves);
    renderer.drawCenteredText(UI_10_FONT_ID, belowGrid + 40, buf);
    snprintf(buf, sizeof(buf), "%s %d", tr(STR_GAME_LEVEL), level + 1);
    renderer.drawCenteredText(SMALL_FONT_ID, belowGrid + 70, buf);
    const auto wonLabels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
    GUI.drawButtonHints(renderer, wonLabels.btn1, wonLabels.btn2, wonLabels.btn3, wonLabels.btn4);
    partialCount = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  snprintf(buf, sizeof(buf), "%s %d   %s %d", tr(STR_GAME_LEVEL), level + 1, tr(STR_GAME_MOVES), moves);
  renderer.drawCenteredText(UI_10_FONT_ID, belowGrid, buf);

  if (state == State::PICK) {
    drawPicker(gridLeft, cell, belowGrid + 36);
  } else {
    renderer.drawCenteredText(SMALL_FONT_ID, belowGrid + 36, tr(STR_GAME_SELECT_CELL));
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Casi todo parcial; uno limpio cada tantos para que el panel no fantasmee.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
