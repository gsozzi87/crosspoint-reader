#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Memoria (encontrar las parejas). Grilla de cartas boca abajo: se dan vuelta
// de a dos, si coinciden quedan destapadas (en negativo) y si no se tapan solas
// después de un momento (con millis(), nunca delay()). Tres niveles: 4x4, 4x5 y
// 5x6, o sea 8, 10 y 15 parejas. Las figuras se dibujan con primitivas (círculo,
// cuadrado, triángulo, cruz, rombo, estrella, media luna, reloj de arena,
// damero, espiral, anillo, aspa, flecha, sol, cuadros concéntricos y almohadilla):
// se ven mucho mejor que las letras y no dependen del idioma. El mejor resultado
// de cada nivel vive en un miembro estático, así dura mientras dure el aparato
// prendido.
//
// Control (cuatro botones, sin teclado ni pantalla táctil):
//   - Nivel: la palanca (ARRIBA/ABAJO) elige el nivel y OK arranca la partida.
//   - Jugando: la palanca recorre SOLO las cartas tapadas, en orden de lectura y
//     en círculo (las emparejadas y la que ya está dada vuelta se saltean); OK da
//     vuelta la carta del cursor.
//   - Con dos cartas arriba que no coinciden, OK las tapa en el acto sin esperar.
//   - Terminada la partida: OK juega de nuevo (vuelve a la pantalla de nivel).
//   - Atrás sale del juego; Atrás mantenido 1 s abandona la partida y vuelve a la
//     pantalla de nivel. OK no tiene pulsación larga en esta placa (apaga).
class MemoryActivity final : public Activity {
 public:
  explicit MemoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Memory", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { LEVEL, PLAY, PEEK, WON };

  static constexpr int LEVEL_COUNT = 3;
  static constexpr int FIGURE_COUNT = 16;  // hacen falta 15 para el nivel más grande
  static constexpr int PARTIALS_BEFORE_CLEAN = 10;   // regla del panel: limpio cada 10-15 parciales
  static constexpr unsigned long PEEK_MS = 1200;     // lo que quedan a la vista las que no coinciden
  static constexpr unsigned long RESTART_HOLD_MS = 1000;

  static constexpr int MARGIN_X = 16;
  static constexpr int GAP = 8;

  // Estado de la partida
  State state = State::LEVEL;
  int level = 0;
  std::vector<uint8_t> figures;  // figura de cada carta
  std::vector<uint8_t> status;   // 0 = tapada, 1 = dada vuelta, 2 = emparejada
  int cursor = 0;
  int firstPick = -1;
  int secondPick = -1;
  int moves = 0;
  int pairsFound = 0;
  int pairsTotal = 0;
  unsigned long startedAt = 0;
  unsigned long peekSince = 0;
  unsigned long finalSeconds = 0;

  int partialCount = 0;
  bool forceClean = false;
  ButtonNavigator buttonNavigator;

  // Mejor marca por nivel (0 = todavía no hay). Estática: sobrevive salir y
  // volver a entrar al juego sin tocar la SD.
  static std::array<uint16_t, LEVEL_COUNT> bestMoves;
  static std::array<uint16_t, LEVEL_COUNT> bestSeconds;

  // Partida
  void startGame();
  void flipAtCursor();
  void resolvePeek();
  void moveCursor(int delta);
  void normalizeCursor();
  bool selectable(int idx) const;
  unsigned long elapsedSeconds() const;

  // Dibujo
  void drawLevelScreen(int top, int bottom) const;
  void drawInfoBar(int y) const;
  void drawBoard(int top, int bottom) const;
  void drawCard(int idx, int x, int y, int w, int h) const;
  void drawSummary(int top, int bottom) const;

  // Primitivas propias (el renderer no trae círculos ni rombos)
  void fillCircle(int cx, int cy, int r, bool ink) const;
  void fillDiamond(int cx, int cy, int r, bool ink) const;
  void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, bool ink) const;
  void drawFigure(int figure, int cx, int cy, int r, bool ink) const;
};
