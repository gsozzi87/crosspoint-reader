#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Sudoku 9x9 con tableros propios: se llena una grilla completa por backtracking
// con orden al azar y después se le sacan números de a uno, verificando con un
// solver (que cuenta hasta dos soluciones) que la solución siga siendo única.
// Tres niveles según cuántas pistas quedan. El solver es iterativo, con su
// propia pila en un miembro, para no comerse el stack de la tarea de la UI, y
// tiene tope de nodos para que la generación no cuelgue el loop.
//
// Control (cuatro botones, sin teclado ni pantalla táctil):
//   - Nivel: la palanca (ARRIBA/ABAJO) elige el nivel y OK arranca la partida.
//   - Jugando: la palanca recorre SOLO las celdas que se pueden escribir, fila
//     por fila y en círculo (las pistas se saltean); OK abre el selector.
//   - Selector: la palanca recorre 1..9 y "Borrar", OK confirma, Atrás cancela.
//   - Atrás sale del juego; Atrás mantenido 1 s vuelve a la pantalla de nivel
//     (partida nueva). OK no tiene pulsación larga en esta placa (apaga).
class SudokuActivity final : public Activity {
 public:
  explicit SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sudoku", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using Grid = std::array<uint8_t, 81>;

  enum class State : uint8_t { LEVEL, GENERATING, PLAY, PICK, WON };

  static constexpr int LEVEL_COUNT = 3;
  static constexpr int CLEAR_OPTION = 9;          // última opción del selector
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 12 parciales
  static constexpr unsigned long RESTART_HOLD_MS = 1000;

  // --- estado de la partida ---
  State state = State::LEVEL;
  int level = 0;          // 0..2, más alto = menos pistas
  Grid given{};           // el tablero como salió del generador (0 = celda libre)
  Grid puzzle{};          // pistas + lo que puso el usuario
  std::vector<uint8_t> editable;  // celdas libres, en orden de lectura
  int cursor = 0;         // índice dentro de editable
  int pickOption = 0;     // 0..8 = dígitos 1..9, CLEAR_OPTION = borrar
  int moves = 0;
  unsigned long startedAt = 0;
  unsigned long elapsedS = 0;
  bool pendingGenerate = false;
  int partialCount = 0;
  ButtonNavigator buttonNavigator;

  // --- solver / generador ---
  struct Frame {
    uint8_t cell;
    uint8_t placed;      // dígito puesto en esta celda (0 = ninguno)
    uint16_t remaining;  // candidatos que faltan probar (bits 1..9)
  };
  Grid work{};
  std::array<uint16_t, 9> rowMask{};
  std::array<uint16_t, 9> colMask{};
  std::array<uint16_t, 9> boxMask{};
  std::array<Frame, 81> solverStack{};
  uint32_t nodes = 0;
  uint32_t nodeBudget = 0;

  static int boxOf(int idx) { return (idx / 27) * 3 + (idx % 9) / 3; }
  static int bitCount(uint16_t bits);

  void buildMasks();
  uint16_t candidates(int idx) const;
  void place(int idx, int digit);
  void unplace(int idx);
  // Resuelve `work`; devuelve cuántas soluciones encontró (hasta `limit`) o -1
  // si se quedó sin presupuesto de nodos.
  int solve(int limit, bool randomOrder);

  void generate();
  void startGame();
  bool conflicts(int idx) const;
  bool solved() const;
  void advanceToNextEmpty();

  void drawBoard(int gridLeft, int gridTop, int cell, int highlight) const;
  void drawPicker(int gridLeft, int cell, int y) const;
};
