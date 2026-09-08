#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Ladrillos (tipo Arkanoid) para la ws397: paleta abajo, pelota que rebota,
// filas de ladrillos, vidas y niveles.
//
// Decisión de ritmo: en tinta electrónica una pelota que se mueve píxel a píxel
// es imposible (cada cuadro es un refresco de panel), así que el campo es una
// GRILLA de 12x16 casilleros y la pelota avanza de casillero en casillero, en
// diagonal o derecho para arriba. Queda un juego con ritmo de tablero, que es
// lo que el panel puede mostrar de verdad, en vez de un parpadeo permanente.
// Los ladrillos ocupan DOS casilleros de ancho (como los de verdad) y el centro
// de la paleta devuelve la pelota derecha para arriba: sin eso la pelota, que
// se mueve en diagonal, quedaría encerrada para siempre en los casilleros de
// una sola paridad y la mitad de los ladrillos serían inalcanzables.
//
// Control (cuatro botones, sin teclado ni pantalla táctil; ARRIBA y ABAJO son
// una palanca física: arriba XOR abajo, nunca las dos):
//   - Palanca ARRIBA: la paleta va a la IZQUIERDA. Palanca ABAJO: a la DERECHA.
//     No es obvio, así que los hints de abajo lo dicen con todas las letras.
//     Mantenida, la paleta sigue andando.
//   - OK: saca la pelota y, en juego, pausa. OK no tiene pulsación larga en
//     esta placa: mantenerlo apaga el aparato.
//   - Atrás: desde el juego pausa (no se pierde la partida por un toque);
//     desde la pausa, el inicio o el final, sale.
//
// Se repintan SOLO los casilleros que cambian (la pelota que se va y la que
// llega, los de la paleta y el ladrillo que se rompe); la pantalla entera se
// rehace al entrar, al cambiar de estado y cada 12 parciales, que es cuando
// además sale un HALF_REFRESH (regla del panel).
class BreakoutActivity final : public Activity {
 public:
  explicit BreakoutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Breakout", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // La pelota se mueve sola: sin esto el aparato baja el reloj por inactividad
  // y se duerme en medio de la partida. Con tope de tres minutos sin tocar un
  // botón, porque una pelota subiendo y bajando derecho no termina nunca y el
  // aparato no puede quedarse despierto para siempre.
  bool preventAutoSleep() override {
    return (state == State::PLAYING || state == State::SERVE) && millis() - lastInputAt < IDLE_AWAKE_MS;
  }

 private:
  enum class State : uint8_t {
    START,       // pantalla de inicio: qué hace cada botón
    SERVE,       // la pelota espera sobre la paleta
    PLAYING,     // la pelota anda
    PAUSED,      // pausa, con la opción de salir
    LEVEL_DONE,  // no quedan ladrillos
    OVER         // se acabaron las vidas
  };

  static constexpr int COLS = 12;  // par: los ladrillos ocupan dos casilleros
  static constexpr int ROWS = 16;
  static constexpr int CELLS = COLS * ROWS;
  static constexpr int BCOLS = COLS / 2;      // ladrillos por fila
  static constexpr int BRICK_TOP = 1;         // primera fila con ladrillos
  static constexpr int MAX_BRICK_ROWS = 5;
  static constexpr int PADDLE_ROW = ROWS - 1;
  static constexpr int MAX_CELL = 42;
  static constexpr int MAX_DIRTY = 16;
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel
  static constexpr int START_LIVES = 3;
  static constexpr unsigned long STEP_START_MS = 240;
  static constexpr unsigned long STEP_MIN_MS = 170;
  static constexpr unsigned long STEP_LEVEL_MS = 18;
  static constexpr unsigned long IDLE_AWAKE_MS = 180000;  // tope para no dormir

  // --- partida ---
  State state = State::START;
  State resumeState = State::SERVE;  // a qué estado vuelve la pausa
  std::array<uint8_t, MAX_BRICK_ROWS * BCOLS> bricks{};  // golpes que le faltan a cada ladrillo
  int brickRows = 4;
  int bricksLeft = 0;
  int ballCol = 0;
  int ballRow = 0;
  int bdx = 0;   // -1, 0 o 1
  int bdy = -1;  // -1 sube, 1 baja
  int paddleCol = 0;
  int paddleW = 3;
  int lives = START_LIVES;
  int level = 1;
  long score = 0;
  static long bestScore;  // mejor de la sesión (no toca la SD)
  unsigned long lastStep = 0;
  unsigned long lastInputAt = 0;

  // --- pintura ---
  std::array<uint16_t, MAX_DIRTY> dirty{};
  int dirtyCount = 0;
  bool infoDirty = false;
  bool fullRepaint = true;
  int partialCount = 0;
  // Repetición al mantener la palanca: 320 ms para arrancar y una movida cada
  // 180 ms, más o menos lo que tarda un refresco parcial del panel.
  ButtonNavigator buttonNavigator{180, 320};
  int cellPx = 0;
  int boardX = 0;
  int boardY = 0;
  int infoY = 0;

  // Partida
  void startGame();
  void buildLevel();
  void resetBall();
  void step();
  void movePaddle(int dir);
  void hitBrick(int col, int row);
  void loseLife();
  void gameOver();
  bool brickAt(int col, int row) const;
  unsigned long stepIntervalMs() const;
  void markDirty(int col, int row);
  void markPaddleDirty();

  // Dibujo
  void layout();
  void fillCircle(int cx, int cy, int r, bool on) const;
  void drawCell(int idx) const;
  void drawBrickCell(int col, int row, int hits) const;
  void drawBoardFrame() const;
  void drawInfo() const;
  void drawOverlay() const;
  void drawHints() const;
};
