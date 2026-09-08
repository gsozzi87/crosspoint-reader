#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Bloques que caen (tipo Tetris) para la ws397. Pozo clásico de 10 columnas por
// 20 filas, con el casillero CHICO (28 px como mucho) para que entren las veinte
// filas y el campo siga ocupando bien la pantalla; las siete piezas con sus
// rotaciones, líneas que se completan y desaparecen, puntaje, nivel que acelera
// la caída y fin de partida cuando ya no entra la pieza nueva. Al costado va lo
// que viene, el puntaje, el nivel, las líneas y el mejor puntaje de la sesión.
//
// Control (cuatro botones, sin teclado ni pantalla táctil; ARRIBA y ABAJO son
// una palanca física: arriba XOR abajo, nunca las dos):
//   - Palanca ARRIBA: mueve la pieza a la IZQUIERDA. Palanca ABAJO: a la
//     DERECHA. Con una palanca vertical es lo más cómodo, y los hints de abajo
//     lo dicen con "<" y ">".
//   - OK: gira la pieza (con pataditas contra la pared y contra lo apilado).
//     OK no tiene pulsación larga en esta placa: mantenerlo apaga el aparato.
//   - Atrás mantenido 1 s: baja la pieza de golpe (y suma 2 puntos por fila).
//   - Atrás corto: pide confirmación para salir (OK sale, Atrás sigue jugando),
//     así no se pierde la partida por un toque de más.
//   - En el resumen final: OK juega otra partida, Atrás sale.
//
// Tinta electrónica: un refresco completo por caída es imposible, así que se
// repinta SOLO cuando cambió algo que se ve (la pieza bajó, se movió, giró, se
// completó una línea o cambió el estado), siempre con FAST_REFRESH y uno
// HALF_REFRESH cada 12 parciales (regla del panel). La caída automática arranca
// en una fila por segundo y acelera con el nivel hasta un piso de 260 ms: más
// rápido que eso el panel no lo puede mostrar, y el juego está diseñado
// asumiéndolo (hay pieza fantasma marcando dónde va a caer y una pasada de
// gracia al tocar el piso para poder acomodarla).
class TetrisActivity final : public Activity {
 public:
  explicit TetrisActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tetris", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t {
    PLAYING,   // la pieza cae
    CLEARING,  // destello de las filas completas antes de sacarlas
    CONFIRM,   // "¿salir?" encima del tablero
    OVER       // resumen final: OK juega de nuevo
  };

  static constexpr int COLS = 10;
  static constexpr int ROWS = 20;   // pozo clásico
  static constexpr int PIECES = 7;
  static constexpr int MAX_CELL = 28;  // tope del casillero: bloques chicos

  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel
  static constexpr unsigned long DROP_HOLD_MS = 1000;
  static constexpr unsigned long CLEAR_FLASH_MS = 420;
  static constexpr unsigned long FALL_START_MS = 1000;  // nivel 1: una fila por segundo
  static constexpr unsigned long FALL_MIN_MS = 260;     // piso del panel
  static constexpr int MAX_LEVEL = 15;

  // Tablero: una celda por casillero, 0 vacío. Las piezas ya apoyadas pierden
  // su identidad (la pantalla es blanco y negro, todas se dibujan igual).
  std::array<uint8_t, static_cast<size_t>(COLS) * ROWS> board{};

  State state = State::PLAYING;

  // Pieza en juego
  int piece = 0;
  int rot = 0;
  int px = 3;
  int py = 0;
  bool landed = false;  // no pudo bajar en la pasada anterior: pasada de gracia
  unsigned long lastFall = 0;

  // Bolsa de 7: cada pieza sale una vez por vuelta, como en el original.
  std::array<uint8_t, PIECES> bag{};
  int bagIndex = PIECES;
  int nextPiece = 0;

  // Marcador
  long score = 0;
  int lines = 0;
  int level = 1;
  static long bestScore;  // mejor de la sesión (no toca la SD)

  // Filas completas esperando el destello
  std::array<bool, ROWS> fullRow{};
  int fullRowCount = 0;
  unsigned long clearAt = 0;

  int partialCount = 0;
  bool forceClean = false;
  // Repetición al mantener la palanca: 400 ms para arrancar y una movida cada
  // 300 ms, que es más o menos lo que tarda un refresco parcial del panel.
  ButtonNavigator buttonNavigator{300, 400};

  // Partida
  void startGame();
  void refillBag();
  int takeFromBag();
  void spawnPiece();
  bool collides(int shapePiece, int shapeRot, int atX, int atY) const;
  void tick();            // caída automática
  void move(int dx);      // palanca
  void rotate();          // OK
  void hardDrop();        // Atrás mantenido
  void lockPiece();
  void finishClear();
  void gameOver();
  int ghostY() const;
  unsigned long fallIntervalMs() const;
  bool cellFilled(int r, int c) const { return board[static_cast<size_t>(r) * COLS + c] != 0; }
  void setCell(int r, int c, bool on) { board[static_cast<size_t>(r) * COLS + c] = on ? 1 : 0; }

  // Dibujo
  void drawBlock(int x, int y, int cell) const;
  void drawGhostBlock(int x, int y, int cell) const;
  void drawWell(int x, int y, int cell) const;
  void drawSidebar(int x, int y, int w, int h, int cell) const;
  void drawMiniPiece(int shapePiece, int boxX, int boxY, int boxW, int boxH, int cell) const;
  void drawStat(int x, int y, int w, const char* label, long value) const;
  void drawDropHint(int y) const;
  void drawOverlay() const;
};
