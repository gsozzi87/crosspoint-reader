#pragma once

#include <Arduino.h>

#include <array>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Damas (reglas inglesas de 8x8) contra la máquina.
//
// Esquema de control, pensado para los cuatro botones de la placa (no hay dos
// ejes ni pulsación larga de OK):
//   - ARRIBA/ABAJO (palanca): recorren una lista. Primero la lista de fichas
//     propias que TIENEN movida legal; después, una vez elegida la ficha, la
//     lista de destinos posibles de esa ficha.
//   - OK: confirma. En la lista de fichas elige la ficha; en la de destinos
//     ejecuta la movida. En fin de partida arranca una nueva.
//   - Atrás: cancela la ficha elegida y, si no hay nada elegido, sale.
//   - Atrás mantenido (1 s): partida nueva en cualquier momento.
// Dibujo del tablero (ver src/activities/games/GameUi.h): las casillas oscuras
// van tramadas al 25 % (al 50 % el tablero vibra y se come la ficha), la grilla
// lleva línea propia y el tablero un marco doble bien grueso. Las fichas del
// jugador son discos con un anillo BLANCO adentro —el disco macizo se confundía
// con el cursor y al moverse dejaba una mancha uniforme— y las de la máquina
// anillos huecos, las dos con halo blanco para despegarlas de la trama; la dama
// lleva una corona adentro. La casilla del cursor se destrama (queda blanca) y
// se marca con un marco de 4 px, la ficha elegida con un marco fino y los
// candidatos con escuadras en las esquinas.
//
// La ÚLTIMA JUGADA DE LA MÁQUINA va con un marco de 2 px en la casilla de
// origen y en la de destino, las dos destramadas. Hasta 1.5.47 eran dos
// cuadraditos de 7 px en las esquinas: invisibles, así que el usuario no sabía
// qué había movido el aparato.
//
// Reglas: movimiento diagonal simple, captura OBLIGATORIA cuando existe,
// capturas múltiples encadenadas (la cadena termina al coronar), coronación en
// la última fila, damas que se mueven y comen en las cuatro diagonales, fin de
// partida por quedarse sin fichas o sin movidas y tablas por 60 medias jugadas
// sin captura ni avance de peón.
//
// La máquina juega con minimax + poda alfa-beta sobre material y avance. La
// búsqueda se reparte: cada pasada del loop resuelve UNA movida raíz, así el
// loop nunca queda bloqueado y la respuesta entra holgada en un segundo.
class CheckersActivity final : public Activity {
 public:
  explicit CheckersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Checkers", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Mientras piensa conviene no esperar el delay del loop: la búsqueda avanza
  // una movida raíz por pasada.
  bool skipLoopDelay() override { return state == AI_TURN; }

 private:
  static constexpr int CELLS = 64;
  static constexpr int MAX_MOVES = 40;   // movidas legales de un bando en una posición
  static constexpr int MAX_SEQ = 10;     // casillas de una cadena de capturas
  static constexpr int MAX_PLY = 16;
  static constexpr int MAX_DEPTH = 8;
  static constexpr int INF_SCORE = 30000;
  static constexpr int WIN_SCORE = 20000;
  static constexpr int CAPTURE_EXT = 4;          // plies extra para no cortar en medio de un cambio
  static constexpr int NO_PROGRESS_DRAW = 60;    // medias jugadas sin captura ni avance de peón
  static constexpr unsigned long RESTART_HOLD_MS = 1000;
  static constexpr unsigned long ROOT_BUDGET_MS = 320;  // si una movida raíz tarda más, se baja la profundidad
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 12 parciales
  // La franja de marcadores es de CUATRO columnas: cada etiqueta tiene 100 px y
  // hay idiomas donde no entra en un renglón, así que se reservan dos.
  static constexpr int STATS_LABEL_LINES = 2;

  // Casilla: fila * 8 + columna, fila 0 arriba (máquina) y fila 7 abajo (jugador).
  // Valores: 0 vacío, +1 peón del jugador, +2 dama del jugador, -1/-2 la máquina.
  using Board = std::array<int8_t, CELLS>;

  struct Move {
    std::array<uint8_t, MAX_SEQ> seq{};  // seq[0] = origen, el resto los saltos
    uint8_t len = 0;
    bool capture = false;
    uint8_t from() const { return seq[0]; }
    uint8_t to() const { return len > 0 ? seq[len - 1] : seq[0]; }
  };

  struct MoveList {
    std::array<Move, MAX_MOVES> items{};
    uint8_t count = 0;
    void clear() { count = 0; }
    bool add(const Move& m) {
      if (count >= MAX_MOVES) return false;
      items[count++] = m;
      return true;
    }
  };

  enum State { PICK_PIECE, PICK_MOVE, AI_TURN, GAME_OVER };
  enum Result { NONE, WON, LOST, DRAW };

  // --- partida ---
  Board board{};
  State state = PICK_PIECE;
  Result result = NONE;
  int moveNumber = 0;
  int wins = 0;            // partidas ganadas en esta sesión (STR_GAME_BEST)
  int noProgress = 0;
  int lastFrom = -1, lastTo = -1;  // última movida de la máquina, para marcarla

  // --- selección del jugador ---
  MoveList humanMoves;
  std::array<uint8_t, 12> pieceList{};   // casillas propias con movida legal
  uint8_t pieceCount = 0;
  int pieceCursor = 0;
  std::array<uint8_t, MAX_MOVES> destMove{};  // índices dentro de humanMoves
  uint8_t destCount = 0;
  int destCursor = 0;

  // --- búsqueda de la máquina, repartida entre pasadas del loop ---
  std::array<MoveList, MAX_PLY> plyMoves{};
  MoveList rootMoves;
  bool aiPrepared = false;
  int aiIndex = 0;
  int aiDepth = 5;
  int aiAlpha = -INF_SCORE;
  int aiBestScore = -INF_SCORE;
  int aiBestIndex = 0;
  int aiTies = 0;

  // --- pantalla ---
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;

  // Partida
  void newGame();
  void refreshHumanMoves();
  void moveCursor(int dir);
  void selectPiece();
  void confirmMove();
  void gameOver(Result r);
  void afterHumanMove();
  void afterAiMove();

  // Reglas
  static int rowOf(int sq) { return sq >> 3; }
  static int colOf(int sq) { return sq & 7; }
  static bool isKing(int8_t p) { return p == 2 || p == -2; }
  static bool belongsTo(int8_t p, int side) { return p != 0 && ((p > 0) == (side > 0)); }
  static void applyMove(Board& b, const Move& m);
  static bool isManMove(const Board& b, const Move& m);
  void generateMoves(const Board& b, int side, MoveList& out) const;
  void addCaptures(const Board& b, int sq, int side, const Move& current, MoveList& out) const;
  static int evaluate(const Board& b);  // positivo = mejor para el jugador

  // Máquina
  void stepAi();
  int pickDepth() const;
  int negamax(const Board& b, int side, int depth, int alpha, int beta, int ply);

  // Dibujo
  void fillCircle(int cx, int cy, int r, bool state) const;
  void drawCrown(int cx, int cy, int r, bool state) const;
  void drawCornerTicks(int x, int y, int cell, int arm, int thickness) const;
  void drawPiece(int cx, int cy, int cell, int8_t piece) const;
  void drawBoard(int left, int top, int cell) const;
};
