#pragma once

#include <Arduino.h>

#include <array>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Ajedrez con reglas completas, contra la máquina o entre dos personas.
//
// Esquema de control (la placa tiene una palanca de un solo eje, OK y Atrás; OK
// mantenido apaga el aparato, así que no existe el "OK largo"):
//   - ARRIBA/ABAJO recorren el tablero en ZIGZAG, en orden de lectura (fila de
//     arriba de izquierda a derecha, después la siguiente), pero PARANDO SOLO en
//     las casillas que importan: primero las piezas propias que tienen movida
//     legal y, una vez elegida la pieza, sus destinos legales. Recorrer las 64
//     casillas de a una con dos botones es interminable, y parar solo en lo
//     jugable deja el mismo orden natural sin pasos de más.
//   - OK confirma: elige la pieza, después ejecuta la movida y, si hay
//     coronación, elige la pieza nueva.
//   - Atrás cancela la pieza elegida y, si no hay nada elegido, sale.
//   - Atrás mantenido (1 s): partida nueva (vuelve a la elección de modo).
//
// Reglas: los movimientos de las seis piezas, enroque corto y largo (con las
// casillas vacías y sin pasar por jaque), captura al paso, coronación a dama,
// torre, alfil o caballo, jaque, jaque mate, ahogado, tablas por 50 movidas sin
// comer ni mover peón y por material insuficiente. Ninguna movida que deje al
// propio rey en jaque llega a la lista: la legalidad se comprueba aplicando la
// movida y mirando si el rey queda atacado.
//
// Dibujo: casillas oscuras TRAMADAS (el negro macizo pesa demasiado en e-ink y
// se come la pieza, ya nos pasó con las damas), piezas rasterizadas a una
// máscara y pintadas MACIZAS las negras y con CONTORNO las blancas, las dos con
// un halo blanco alrededor para despegarlas de la trama. Se distinguen por la
// silueta, que es lo único que hay sin color.
//
// La máquina: negamax con poda alfa-beta, búsqueda de capturas al final
// (quiescencia), ordenamiento de movidas por MVV-LVA y profundización iterativa
// para poder cortar por tiempo. Todo repartido entre pasadas del loop (una
// movida raíz por pasada) para que la pantalla y los botones nunca queden
// colgados, con tope total por jugada y tope de profundidad para no reventar la
// pila del ESP32-S3.
class ChessActivity final : public Activity {
 public:
  explicit ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Chess", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Mientras piensa no conviene esperar el delay del loop: la búsqueda avanza
  // una movida raíz por pasada.
  bool skipLoopDelay() override { return state == AI_TURN; }

  // ------------------------------------------------------------- reglas ----
  // Públicas (y estáticas) a propósito: así las reglas se pueden probar de
  // escritorio con g++ corriendo perft, sin nada de la pantalla en el medio.

  // Piezas: 1 peón, 2 caballo, 3 alfil, 4 torre, 5 dama, 6 rey. Positivo =
  // blancas, negativo = negras, 0 = vacío.
  enum PieceType : int8_t { PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

  // Banderas de una movida.
  enum MoveFlag : uint8_t { F_CAPTURE = 1, F_EP = 2, F_CASTLE = 4, F_DOUBLE = 8, F_PROMO = 16 };

  // Derechos de enroque.
  enum CastleFlag : uint8_t { C_WK = 1, C_WQ = 2, C_BK = 4, C_BQ = 8 };

  static constexpr int MAX_MOVES = 128;  // tope de movidas pseudolegales de una posición

  struct Move {
    uint8_t from = 0;
    uint8_t to = 0;
    int8_t promo = 0;  // 0, o el tipo de pieza coronada (KNIGHT..QUEEN)
    uint8_t flags = 0;
  };

  struct MoveList {
    std::array<Move, MAX_MOVES> items{};
    uint16_t count = 0;
    void clear() { count = 0; }
    void add(const Move& m) {
      if (count < MAX_MOVES) items[count++] = m;
    }
  };

  // Tablero 0x88: casilla = fila * 16 + columna, y (casilla & 0x88) != 0 marca
  // el afuera. La fila 0 es la de arriba de la PANTALLA, o sea la fila 8 del
  // ajedrez (las negras), y la fila 7 la primera de las blancas; así el índice
  // del tablero es directamente el de dibujo.
  struct Position {
    std::array<int8_t, 128> squares{};
    uint8_t side = 0;      // 0 blancas, 1 negras
    uint8_t castling = 0;  // C_WK | C_WQ | C_BK | C_BQ
    int8_t ep = -1;        // casilla de captura al paso, -1 si no hay
    uint8_t halfmove = 0;  // medias jugadas sin comer ni mover peón (regla de 50)
    std::array<uint8_t, 2> kingSquare{{0x74, 0x04}};
  };

  static bool onBoard(const int sq) { return (sq & 0x88) == 0; }
  static int rowOf(const int sq) { return sq >> 4; }
  static int colOf(const int sq) { return sq & 7; }
  static int squareOf(const int row, const int col) { return row * 16 + col; }
  static int8_t typeOf(const int8_t piece) { return piece < 0 ? static_cast<int8_t>(-piece) : piece; }
  static int sideOf(const int8_t piece) { return piece < 0 ? 1 : 0; }

  static void setupInitial(Position& p);
  // Movidas PSEUDOLEGALES (todas menos las que dejan al rey en jaque, que las
  // filtra tryMakeMove). El enroque ya sale filtrado por casillas atacadas.
  static void generateMoves(const Position& p, MoveList& out);
  // Movidas legales de verdad: las pseudolegales que sobreviven a tryMakeMove.
  static void generateLegal(const Position& p, MoveList& out);
  static bool isAttacked(const Position& p, int sq, int bySide);
  static bool inCheck(const Position& p, int side);
  // Aplica `m` de `in` a `out`. Devuelve false si la movida es ilegal porque
  // deja al propio rey en jaque; en ese caso `out` queda inservible.
  static bool tryMakeMove(const Position& in, const Move& m, Position& out);
  static bool insufficientMaterial(const Position& p);

 private:
  static constexpr int MAX_PLY = 24;        // tope de la recursión (profundidad + quiescencia)
  static constexpr int MAX_DEPTH = 6;       // tope de la profundización iterativa
  static constexpr int QS_PLIES = 6;        // capturas que se siguen mirando después de la profundidad
  static constexpr int INF_SCORE = 30000;
  static constexpr int MATE_SCORE = 20000;
  static constexpr unsigned long THINK_BUDGET_MS = 2200;  // tope total por jugada
  static constexpr unsigned long SLICE_MS = 350;          // tope de UNA movida raíz (una pasada del loop)
  static constexpr unsigned long RESTART_HOLD_MS = 1000;
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 12 parciales
  static constexpr int MAX_CELL = 54;
  static constexpr int MASK_BYTES = (MAX_CELL * MAX_CELL + 7) / 8;

  enum State { MODE_SELECT, PICK_PIECE, PICK_MOVE, PROMOTE, AI_TURN, GAME_OVER };
  enum Mode { VS_MACHINE_WHITE, VS_MACHINE_BLACK, TWO_PLAYERS };
  enum Result { RES_NONE, RES_WHITE, RES_BLACK, RES_DRAW };
  enum EndReason { END_NONE, END_MATE, END_STALEMATE, END_FIFTY, END_MATERIAL };

  // --- partida ---
  Position pos;
  Mode mode = VS_MACHINE_WHITE;
  State state = MODE_SELECT;
  Result result = RES_NONE;
  EndReason endReason = END_NONE;
  int modeCursor = 0;
  int fullMoveNumber = 1;
  int lastFrom = -1, lastTo = -1;
  char lastMoveText[24] = "";
  bool boardFlipped = false;  // true cuando el humano juega con negras: sus piezas quedan abajo

  // --- selección del jugador (zigzag sobre las casillas jugables) ---
  MoveList legalMoves;
  std::array<uint8_t, 16> pieceList{};
  uint8_t pieceCount = 0;
  int pieceCursor = 0;
  std::array<uint8_t, MAX_MOVES> destMove{};  // índices dentro de legalMoves
  uint8_t destCount = 0;
  int destCursor = 0;
  std::array<uint8_t, 4> promoMove{};  // las cuatro coronaciones del destino elegido
  uint8_t promoCount = 0;
  int promoCursor = 0;

  // --- búsqueda de la máquina, repartida entre pasadas del loop ---
  std::array<Position, MAX_PLY> plyPos{};
  std::array<MoveList, MAX_PLY> plyMoves{};
  MoveList rootMoves;
  bool aiPrepared = false;
  bool aiAborted = false;       // se acabó el tiempo de ESTA movida raíz
  bool aiIterAborted = false;   // la profundidad entera quedó a medias
  int aiDepth = 1;
  int aiIndex = 0;
  int aiAlpha = -INF_SCORE;
  int aiIterBest = 0;
  int aiIterBestScore = -INF_SCORE;
  int aiIterTies = 0;
  int aiBestIndex = 0;
  int aiBestScore = 0;
  int aiBestDepth = 0;
  unsigned long aiStartMs = 0;
  unsigned long sliceDeadline = 0;
  uint32_t nodes = 0;
  // Puntajes del ordenamiento. Va acá y no en la pila de search() porque son
  // 256 bytes por nodo y la recursión llega a trece niveles: en el ESP32-S3 la
  // pila no da para eso.
  std::array<int16_t, MAX_MOVES> orderScores{};

  // --- pantalla ---
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;

  // Partida
  void startGame(Mode m);
  void newGame();
  void refreshSelection();
  void afterMove();
  void moveCursor(int dir);
  void selectPiece();
  void confirmDestination();
  void applyPlayerMove(const Move& m);
  void gameOver(Result r, EndReason reason);
  bool machinePlays(int side) const;

  // Máquina
  void stepAi();
  void playAiMove(const Move& m);
  int search(int ply, int depth, int alpha, int beta);
  int quiesce(int ply, int qdepth, int alpha, int beta);
  bool outOfTime();
  static int evaluate(const Position& p);  // positivo = mejor para las blancas
  void orderMoves(const Position& p, MoveList& ml);

  // Notación de coordenadas ("e2e4", "e7e8D", "O-O"), con + o # al final.
  static void formatMove(const Position& before, const Move& m, const Position& after, char* out, size_t outSize);

  // Dibujo
  int displayRow(const int sq) const { return boardFlipped ? 7 - rowOf(sq) : rowOf(sq); }
  int displayCol(const int sq) const { return boardFlipped ? 7 - colOf(sq) : colOf(sq); }
  int squareFromDisplay(int dr, int dc) const;
  void hatchCell(int x, int y, int cell) const;
  void drawCornerTicks(int x, int y, int cell, int arm, int thickness) const;
  void drawPiece(int x, int y, int cell, int8_t piece) const;
  void drawBoard(int left, int top, int cell) const;
  void drawModeSelect() const;
  void drawPromotionBar(int top) const;
};
