#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Cuatro en línea (tablero clásico de 7 columnas por 6 filas), en dos modos:
// dos jugadores en el mismo aparato, o contra la máquina.
//
// Control (cuatro botones, sin teclado ni pantalla táctil; ARRIBA y ABAJO son
// una palanca física: arriba XOR abajo, nunca las dos):
//   - Pantalla de modo: la palanca elige y OK arranca.
//   - Jugando: la palanca mueve la columna (ARRIBA a la izquierda, ABAJO a la
//     derecha, como en el resto de los juegos de la placa) y OK suelta la
//     ficha. OK no tiene pulsación larga en esta placa: mantenerlo apaga el
//     aparato.
//   - Atrás sale; Atrás mantenido 1 s vuelve a la pantalla de modo (partida
//     nueva).
//
// Blanco y negro, sin color: las fichas del jugador 1 son discos llenos y las
// del jugador 2 (o de la máquina) anillos huecos, como en las damas, y los
// agujeros vacíos son circunferencias finas. La línea ganadora queda marcada
// con un anillo alrededor de cada una de las cuatro fichas y una raya gruesa
// que las une.
//
// La máquina juega con minimax y poda alfa-beta (7 de profundidad, 9 cuando
// quedan pocos casilleros; medido de escritorio, son unos 175 ms y 70 ms de
// aparato para la jugada entera) sobre ventanas de a cuatro. La búsqueda se reparte:
// cada pasada del loop resuelve UNA columna de la raíz, así el loop nunca queda
// bloqueado y la jugada sale muy adentro del segundo.
class ConnectFourActivity final : public Activity {
 public:
  explicit ConnectFourActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ConnectFour", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Mientras piensa conviene no esperar el delay del loop: la búsqueda avanza
  // una columna de la raíz por pasada.
  bool skipLoopDelay() override { return state == State::AI_TURN; }

 private:
  enum class State : uint8_t {
    MODE,     // elección de modo
    PLAYING,  // le toca a una persona
    AI_TURN,  // la máquina está pensando
    OVER      // ganó alguien o quedó empate
  };
  enum class Mode : uint8_t { TWO_PLAYERS, VS_MACHINE };

  static constexpr int COLS = 7;
  static constexpr int ROWS = 6;
  static constexpr int CELLS = COLS * ROWS;
  static constexpr int MODE_COUNT = 2;
  static constexpr int MAX_CELL = 64;
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel
  static constexpr int WIN_SCORE = 100000;
  static constexpr int INF_SCORE = 1000000;
  static constexpr int BASE_DEPTH = 7;
  static constexpr int END_DEPTH = 9;  // con el tablero casi lleno se busca más hondo
  static constexpr unsigned long ROOT_BUDGET_MS = 320;  // si una columna tarda más, se baja
  static constexpr unsigned long RESTART_HOLD_MS = 1000;

  // Casilla: fila * 7 + columna, fila 0 arriba. 0 vacío, 1 jugador 1, 2 jugador 2.
  using Board = std::array<int8_t, CELLS>;

  // --- partida ---
  Board board{};
  Mode mode = Mode::TWO_PLAYERS;
  State state = State::MODE;
  int modeCursor = 0;
  int cursorCol = COLS / 2;
  int8_t turn = 1;
  int8_t winner = 0;  // 0 = nadie todavía
  bool drawn = false;
  std::array<uint8_t, 4> winLine{};
  bool haveWinLine = false;
  int lastDropIdx = -1;
  int winsP1 = 0;
  int winsP2 = 0;
  int moveNumber = 0;

  // --- búsqueda de la máquina, repartida entre pasadas del loop ---
  std::array<uint8_t, COLS> rootCols{};
  int rootCount = 0;
  bool aiPrepared = false;
  int aiIndex = 0;
  int aiDepth = BASE_DEPTH;
  int aiAlpha = -INF_SCORE;
  int aiBestScore = -INF_SCORE;
  int aiBestCol = 0;
  int aiTies = 0;

  // --- pantalla ---
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;
  int cellPx = 0;
  int boardX = 0;
  int boardY = 0;
  int cursorY = 0;
  // El bloque de abajo se arma de abajo hacia arriba y con alto fijo (la ayuda
  // reserva sus renglones se usen o no): el tablero no se mueve cuando cambia
  // el texto.
  int statusTop = 0;
  int statsTop = 0;
  int helpTop = 0;

  // Partida
  void newGame();
  void moveCursor(int dir);
  void dropPiece();
  void finishTurn(int row, int col);
  void gameOver();

  // Reglas (puras, para poder probarlas de escritorio)
  static bool inBoard(int row, int col) { return row >= 0 && row < ROWS && col >= 0 && col < COLS; }
  static int dropRow(const Board& b, int col);  // fila donde cae la ficha, -1 si la columna está llena
  static bool winLineThrough(const Board& b, int row, int col, int8_t player, uint8_t* out);
  static bool boardFull(const Board& b);
  static int evaluate(const Board& b, int8_t side);

  // Máquina
  void stepAi();
  int pickDepth() const;
  int negamax(Board& b, int8_t side, int depth, int alpha, int beta, int ply);

  // Dibujo
  void layout();
  void fillCircle(int cx, int cy, int r, bool on) const;
  void drawRing(int cx, int cy, int outer, int inner) const;
  void drawDisc(int cx, int cy, int r, int8_t player) const;
  void drawBoard() const;
  void drawCursor() const;
  void drawInfo() const;
  void drawModeScreen() const;
  void drawHints() const;
  const char* playerName(int8_t player) const;
};
