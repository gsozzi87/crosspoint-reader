#include "ChessActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "GameUi.h"
#include "MappedInputManager.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// --------------------------------------------------------------- reglas ----

// Direcciones en 0x88. Las ocho del rey sirven también para la dama.
constexpr int8_t KNIGHT_D[8] = {-33, -31, -18, -14, 14, 18, 31, 33};
constexpr int8_t KING_D[8] = {-17, -16, -15, -1, 1, 15, 16, 17};
constexpr int8_t BISHOP_D[4] = {-17, -15, 15, 17};
constexpr int8_t ROOK_D[4] = {-16, -1, 1, 16};

// Valor de cada tipo de pieza en centipeones (índice = tipo, 0 sin usar).
constexpr int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 0};

// Tablas de posición, escritas como se ve el tablero con las blancas abajo:
// el índice 0 es a8 y el 63 es h1, o sea fila*8 + columna con la fila 0 arriba.
// Para una pieza negra se lee la tabla espejada verticalmente.
constexpr int8_t PST_PAWN[64] = {0,  0,  0,  0,   0,   0,  0,  0,   50, 50, 50,  50, 50,  50, 50, 50,
                                 10, 10, 20, 30,  30,  20, 10, 10,  5,  5,  10,  25, 25,  10, 5,  5,
                                 0,  0,  0,  20,  20,  0,  0,  0,   5,  -5, -10, 0,  0,   -10, -5, 5,
                                 5,  10, 10, -20, -20, 10, 10, 5,   0,  0,  0,   0,  0,   0,  0,  0};
constexpr int8_t PST_KNIGHT[64] = {-50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0,   0,   0,   0,   -20, -40,
                                   -30, 0,   10,  15,  15,  10,  0,   -30, -30, 5,   15,  20,  20,  15,  5,   -30,
                                   -30, 0,   15,  20,  20,  15,  0,   -30, -30, 5,   10,  15,  15,  10,  5,   -30,
                                   -40, -20, 0,   5,   5,   0,   -20, -40, -50, -40, -30, -30, -30, -30, -40, -50};
constexpr int8_t PST_BISHOP[64] = {-20, -10, -10, -10, -10, -10, -10, -20, -10, 0,   0,  0,  0,  0,   0,   -10,
                                   -10, 0,   5,   10,  10,  5,   0,   -10, -10, 5,   5,  10, 10, 5,   5,   -10,
                                   -10, 0,   10,  10,  10,  10,  0,   -10, -10, 10,  10, 10, 10, 10,  10,  -10,
                                   -10, 5,   0,   0,   0,   0,   5,   -10, -20, -10, -10, -10, -10, -10, -10, -20};
constexpr int8_t PST_ROOK[64] = {0,  0,  0,  0,  0,  0,  0,  0,  5,  10, 10, 10, 10, 10, 10, 5,
                                 -5, 0,  0,  0,  0,  0,  0,  -5, -5, 0,  0,  0,  0,  0,  0,  -5,
                                 -5, 0,  0,  0,  0,  0,  0,  -5, -5, 0,  0,  0,  0,  0,  0,  -5,
                                 -5, 0,  0,  0,  0,  0,  0,  -5, 0,  0,  0,  5,  5,  0,  0,  0};
constexpr int8_t PST_QUEEN[64] = {-20, -10, -10, -5, -5, -10, -10, -20, -10, 0,  0,  0,  0,  0,  0,   -10,
                                  -10, 0,   5,   5,  5,  5,   0,   -10, -5,  0,  5,  5,  5,  5,  0,   -5,
                                  0,   0,   5,   5,  5,  5,   0,   -5,  -10, 5,  5,  5,  5,  5,  0,   -10,
                                  -10, 0,   5,   0,  0,  0,   0,   -10, -20, -10, -10, -5, -5, -10, -10, -20};
constexpr int8_t PST_KING_MID[64] = {-30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
                                     -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
                                     -20, -30, -30, -40, -40, -30, -30, -20, -10, -20, -20, -20, -20, -20, -20, -10,
                                     20,  20,  0,   0,   0,   0,   20,  20,  20,  30,  10,  0,   0,   10,  30,  20};
constexpr int8_t PST_KING_END[64] = {-50, -40, -30, -20, -20, -30, -40, -50, -30, -20, -10, 0,   0,   -10, -20, -30,
                                     -30, -10, 20,  30,  30,  20,  -10, -30, -30, -10, 30,  40,  40,  30,  -10, -30,
                                     -30, -10, 30,  40,  40,  30,  -10, -30, -30, -10, 20,  30,  30,  20,  -10, -30,
                                     -30, -30, 0,   0,   0,   0,   -30, -30, -50, -30, -30, -30, -30, -30, -30, -50};

const int8_t* pstFor(const int8_t type, const bool endgame) {
  switch (type) {
    case 1: return PST_PAWN;
    case 2: return PST_KNIGHT;
    case 3: return PST_BISHOP;
    case 4: return PST_ROOK;
    case 5: return PST_QUEEN;
    default: return endgame ? PST_KING_END : PST_KING_MID;
  }
}

// --------------------------------------------------------- piezas (dibujo) --

// Máscara de 1 bit donde se rasteriza la pieza antes de pintarla. El dibujo va
// en coordenadas 0..100 y se escala al recuadro interior (`off`, `span`), que
// deja unos píxeles de aire para el halo blanco.
struct Mask {
  uint8_t* bits;
  int dim;
  int off;
  int span;

  void put(const int x, const int y, const bool on) const {
    if (x < 0 || y < 0 || x >= dim || y >= dim) return;
    const int i = y * dim + x;
    if (on) {
      bits[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
    } else {
      bits[i >> 3] &= static_cast<uint8_t>(~(0x80 >> (i & 7)));
    }
  }
  bool get(const int x, const int y) const {
    if (x < 0 || y < 0 || x >= dim || y >= dim) return false;
    const int i = y * dim + x;
    return (bits[i >> 3] & (0x80 >> (i & 7))) != 0;
  }
  int u(const int v) const { return off + (v * span + 50) / 100; }
};

void maskRect(const Mask& m, const int x0, const int y0, const int x1, const int y1, const bool on = true) {
  const int a = m.u(x0), b = m.u(y0), c = m.u(x1), d = m.u(y1);
  for (int y = b; y <= d; ++y)
    for (int x = a; x <= c; ++x) m.put(x, y, on);
}

void maskDisc(const Mask& m, const int cx, const int cy, const int r, const bool on = true) {
  const int px = m.u(cx), py = m.u(cy);
  const int pr = (r * m.span + 50) / 100;
  for (int dy = -pr; dy <= pr; ++dy)
    for (int dx = -pr; dx <= pr; ++dx)
      if (dx * dx + dy * dy <= pr * pr) m.put(px + dx, py + dy, on);
}

// Relleno de polígono por barrido (par-impar). `pts` son pares x,y en 0..100.
void maskPoly(const Mask& m, const int8_t* pts, const int n, const bool on = true) {
  int minY = 1 << 20, maxY = -(1 << 20);
  for (int i = 0; i < n; ++i) {
    const int y = m.u(pts[2 * i + 1]);
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
  }
  if (minY < 0) minY = 0;
  if (maxY >= m.dim) maxY = m.dim - 1;
  for (int y = minY; y <= maxY; ++y) {
    int xs[16];
    int count = 0;
    for (int i = 0; i < n && count < 16; ++i) {
      const int j = (i + 1) % n;
      const int y0 = m.u(pts[2 * i + 1]), y1 = m.u(pts[2 * j + 1]);
      if (y0 == y1) continue;
      const int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
      if (y < lo || y >= hi) continue;
      const int x0 = m.u(pts[2 * i]), x1 = m.u(pts[2 * j]);
      xs[count++] = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    }
    for (int a = 1; a < count; ++a) {  // orden por inserción, son poquísimos
      const int v = xs[a];
      int b = a - 1;
      while (b >= 0 && xs[b] > v) {
        xs[b + 1] = xs[b];
        --b;
      }
      xs[b + 1] = v;
    }
    for (int a = 0; a + 1 < count; a += 2)
      for (int x = xs[a]; x <= xs[a + 1]; ++x) m.put(x, y, on);
  }
}

// Siluetas en 0..100 (x hacia la derecha, y hacia abajo). Lo que las distingue
// es la forma: en blanco y negro no hay otra cosa.
constexpr int8_t SHAPE_PAWN_NECK[] = {40, 42, 60, 42, 64, 56, 36, 56};
constexpr int8_t SHAPE_PAWN_BODY[] = {36, 56, 64, 56, 72, 84, 28, 84};
constexpr int8_t SHAPE_ROOK_TOP[] = {22, 14, 34, 14, 34, 24, 43, 24, 43, 14, 57, 14,
                                     57, 24, 66, 24, 66, 14, 78, 14, 78, 34, 22, 34};
constexpr int8_t SHAPE_ROOK_BODY[] = {34, 44, 66, 44, 70, 80, 30, 80};
constexpr int8_t SHAPE_BISHOP_HEAD[] = {50, 14, 63, 34, 62, 50, 38, 50, 37, 34};
constexpr int8_t SHAPE_BISHOP_BODY[] = {36, 58, 64, 58, 70, 82, 30, 82};
constexpr int8_t SHAPE_BISHOP_SLIT[] = {46, 24, 62, 20, 64, 26, 48, 32};
constexpr int8_t SHAPE_KNIGHT[] = {24, 92, 24, 84, 32, 76, 26, 68, 18, 60, 20, 52, 28, 48, 24, 40, 30, 30, 38, 34,
                                   42, 24, 48, 14, 56, 18, 58, 8,  67, 18, 74, 30, 78, 46, 76, 64, 70, 80, 70, 92};
constexpr int8_t SHAPE_QUEEN_CROWN[] = {24, 42, 27, 20, 34, 34, 38, 14, 44, 32, 50, 10,
                                        56, 32, 62, 14, 66, 34, 73, 20, 76, 42};
constexpr int8_t SHAPE_QUEEN_BODY[] = {30, 50, 70, 50, 76, 82, 24, 82};
constexpr int8_t SHAPE_KING_CROWN[] = {26, 44, 30, 26, 40, 36, 50, 26, 60, 36, 70, 26, 74, 44};
constexpr int8_t SHAPE_KING_BODY[] = {30, 52, 70, 52, 76, 82, 24, 82};

void buildPieceMask(const Mask& m, const int8_t type) {
  switch (type) {
    case 1:  // peón
      maskDisc(m, 50, 30, 14);
      maskPoly(m, SHAPE_PAWN_NECK, 4);
      maskPoly(m, SHAPE_PAWN_BODY, 4);
      maskRect(m, 20, 84, 80, 95);
      break;
    case 2:  // caballo
      maskPoly(m, SHAPE_KNIGHT, 20);
      maskRect(m, 18, 88, 82, 96);
      maskDisc(m, 44, 36, 4, false);  // el ojo, hueco
      break;
    case 3:  // alfil
      maskDisc(m, 50, 9, 6);
      maskPoly(m, SHAPE_BISHOP_HEAD, 5);
      maskRect(m, 34, 50, 66, 58);
      maskPoly(m, SHAPE_BISHOP_BODY, 4);
      maskRect(m, 18, 82, 82, 95);
      maskPoly(m, SHAPE_BISHOP_SLIT, 4, false);  // el corte de la mitra
      break;
    case 4:  // torre
      maskPoly(m, SHAPE_ROOK_TOP, 12);
      maskRect(m, 30, 34, 70, 44);
      maskPoly(m, SHAPE_ROOK_BODY, 4);
      maskRect(m, 20, 80, 80, 95);
      break;
    case 5:  // dama
      maskPoly(m, SHAPE_QUEEN_CROWN, 11);
      maskDisc(m, 27, 18, 5);
      maskDisc(m, 38, 12, 5);
      maskDisc(m, 50, 8, 5);
      maskDisc(m, 62, 12, 5);
      maskDisc(m, 73, 18, 5);
      maskRect(m, 26, 42, 74, 50);
      maskPoly(m, SHAPE_QUEEN_BODY, 4);
      maskRect(m, 18, 82, 82, 95);
      break;
    default:  // rey
      maskRect(m, 45, 2, 55, 26);
      maskRect(m, 37, 10, 63, 19);
      maskPoly(m, SHAPE_KING_CROWN, 7);
      maskRect(m, 26, 44, 74, 52);
      maskPoly(m, SHAPE_KING_BODY, 4);
      maskRect(m, 18, 82, 82, 95);
      break;
  }
}

constexpr int FRAME_GAP = 7;
constexpr int ROW_H = 56;
constexpr int SIDE = gameui::SIDE;
constexpr int MIN_CELL = 20;
constexpr int COORD_H = 22;  // la fila de coordenadas de abajo del tablero

}  // namespace

// ================================================================= reglas ===

void ChessActivity::setupInitial(Position& p) {
  p.squares.fill(0);
  static constexpr int8_t BACK[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
  for (int c = 0; c < 8; ++c) {
    p.squares[squareOf(0, c)] = static_cast<int8_t>(-BACK[c]);  // negras arriba
    p.squares[squareOf(1, c)] = static_cast<int8_t>(-PAWN);
    p.squares[squareOf(6, c)] = PAWN;
    p.squares[squareOf(7, c)] = BACK[c];
  }
  p.side = 0;
  p.castling = C_WK | C_WQ | C_BK | C_BQ;
  p.ep = -1;
  p.halfmove = 0;
  p.kingSquare[0] = static_cast<uint8_t>(squareOf(7, 4));
  p.kingSquare[1] = static_cast<uint8_t>(squareOf(0, 4));
}

bool ChessActivity::isAttacked(const Position& p, const int sq, const int bySide) {
  const int8_t sign = bySide == 0 ? 1 : -1;

  // Peones: un peón blanco en sq+15 o sq+17 ataca sq (se mueve hacia la fila
  // menor); uno negro, en sq-15 o sq-17.
  const int pawnA = bySide == 0 ? sq + 15 : sq - 15;
  const int pawnB = bySide == 0 ? sq + 17 : sq - 17;
  if (onBoard(pawnA) && p.squares[pawnA] == sign * PAWN) return true;
  if (onBoard(pawnB) && p.squares[pawnB] == sign * PAWN) return true;

  for (int i = 0; i < 8; ++i) {
    const int t = sq + KNIGHT_D[i];
    if (onBoard(t) && p.squares[t] == sign * KNIGHT) return true;
  }
  for (int i = 0; i < 8; ++i) {
    const int t = sq + KING_D[i];
    if (onBoard(t) && p.squares[t] == sign * KING) return true;
  }
  for (int i = 0; i < 4; ++i) {
    int t = sq + BISHOP_D[i];
    while (onBoard(t)) {
      const int8_t piece = p.squares[t];
      if (piece != 0) {
        if (piece == sign * BISHOP || piece == sign * QUEEN) return true;
        break;
      }
      t += BISHOP_D[i];
    }
  }
  for (int i = 0; i < 4; ++i) {
    int t = sq + ROOK_D[i];
    while (onBoard(t)) {
      const int8_t piece = p.squares[t];
      if (piece != 0) {
        if (piece == sign * ROOK || piece == sign * QUEEN) return true;
        break;
      }
      t += ROOK_D[i];
    }
  }
  return false;
}

bool ChessActivity::inCheck(const Position& p, const int side) {
  return isAttacked(p, p.kingSquare[side], 1 - side);
}

void ChessActivity::generateMoves(const Position& p, MoveList& out) {
  out.clear();
  const int side = p.side;
  const int8_t sign = side == 0 ? 1 : -1;
  const int pawnDir = side == 0 ? -16 : 16;
  const int startRow = side == 0 ? 6 : 1;
  const int promoRow = side == 0 ? 0 : 7;

  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq)) continue;
    const int8_t piece = p.squares[sq];
    if (piece == 0 || sideOf(piece) != side) continue;
    const int8_t type = typeOf(piece);

    if (type == PAWN) {
      const int one = sq + pawnDir;
      if (onBoard(one) && p.squares[one] == 0) {
        if (rowOf(one) == promoRow) {
          for (int8_t promo = QUEEN; promo >= KNIGHT; --promo) {
            Move m;
            m.from = static_cast<uint8_t>(sq);
            m.to = static_cast<uint8_t>(one);
            m.promo = promo;
            m.flags = F_PROMO;
            out.add(m);
          }
        } else {
          Move m;
          m.from = static_cast<uint8_t>(sq);
          m.to = static_cast<uint8_t>(one);
          out.add(m);
          const int two = one + pawnDir;
          if (rowOf(sq) == startRow && onBoard(two) && p.squares[two] == 0) {
            Move d;
            d.from = static_cast<uint8_t>(sq);
            d.to = static_cast<uint8_t>(two);
            d.flags = F_DOUBLE;
            out.add(d);
          }
        }
      }
      for (int dc = -1; dc <= 1; dc += 2) {
        const int t = sq + pawnDir + dc;
        if (!onBoard(t)) continue;
        const int8_t target = p.squares[t];
        if (target != 0 && sideOf(target) != side) {
          if (rowOf(t) == promoRow) {
            for (int8_t promo = QUEEN; promo >= KNIGHT; --promo) {
              Move m;
              m.from = static_cast<uint8_t>(sq);
              m.to = static_cast<uint8_t>(t);
              m.promo = promo;
              m.flags = F_PROMO | F_CAPTURE;
              out.add(m);
            }
          } else {
            Move m;
            m.from = static_cast<uint8_t>(sq);
            m.to = static_cast<uint8_t>(t);
            m.flags = F_CAPTURE;
            out.add(m);
          }
        } else if (target == 0 && p.ep >= 0 && t == p.ep) {
          Move m;
          m.from = static_cast<uint8_t>(sq);
          m.to = static_cast<uint8_t>(t);
          m.flags = F_CAPTURE | F_EP;
          out.add(m);
        }
      }
      continue;
    }

    const int8_t* deltas;
    int count;
    bool slide;
    switch (type) {
      case KNIGHT:
        deltas = KNIGHT_D;
        count = 8;
        slide = false;
        break;
      case BISHOP:
        deltas = BISHOP_D;
        count = 4;
        slide = true;
        break;
      case ROOK:
        deltas = ROOK_D;
        count = 4;
        slide = true;
        break;
      case QUEEN:
        deltas = KING_D;
        count = 8;
        slide = true;
        break;
      default:
        deltas = KING_D;
        count = 8;
        slide = false;
        break;
    }
    for (int i = 0; i < count; ++i) {
      int t = sq + deltas[i];
      while (onBoard(t)) {
        const int8_t target = p.squares[t];
        if (target != 0) {
          if (sideOf(target) != side) {
            Move m;
            m.from = static_cast<uint8_t>(sq);
            m.to = static_cast<uint8_t>(t);
            m.flags = F_CAPTURE;
            out.add(m);
          }
          break;
        }
        Move m;
        m.from = static_cast<uint8_t>(sq);
        m.to = static_cast<uint8_t>(t);
        out.add(m);
        if (!slide) break;
        t += deltas[i];
      }
    }
  }

  // Enroques. Las casillas del medio vacías, el rey ni en jaque ni pasando por
  // una casilla atacada (la de llegada la filtra tryMakeMove).
  const int kingHome = side == 0 ? squareOf(7, 4) : squareOf(0, 4);
  const uint8_t shortRight = side == 0 ? C_WK : C_BK;
  const uint8_t longRight = side == 0 ? C_WQ : C_BQ;
  if (p.squares[kingHome] == sign * KING && !isAttacked(p, kingHome, 1 - side)) {
    if ((p.castling & shortRight) != 0 && p.squares[kingHome + 3] == sign * ROOK && p.squares[kingHome + 1] == 0 &&
        p.squares[kingHome + 2] == 0 && !isAttacked(p, kingHome + 1, 1 - side)) {
      Move m;
      m.from = static_cast<uint8_t>(kingHome);
      m.to = static_cast<uint8_t>(kingHome + 2);
      m.flags = F_CASTLE;
      out.add(m);
    }
    if ((p.castling & longRight) != 0 && p.squares[kingHome - 4] == sign * ROOK && p.squares[kingHome - 1] == 0 &&
        p.squares[kingHome - 2] == 0 && p.squares[kingHome - 3] == 0 && !isAttacked(p, kingHome - 1, 1 - side)) {
      Move m;
      m.from = static_cast<uint8_t>(kingHome);
      m.to = static_cast<uint8_t>(kingHome - 2);
      m.flags = F_CASTLE;
      out.add(m);
    }
  }
}

bool ChessActivity::tryMakeMove(const Position& in, const Move& m, Position& out) {
  out = in;
  const int side = in.side;
  const int8_t piece = out.squares[m.from];
  const int8_t type = typeOf(piece);

  out.ep = -1;
  out.halfmove = in.halfmove < 250 ? static_cast<uint8_t>(in.halfmove + 1) : in.halfmove;
  if (type == PAWN || (m.flags & F_CAPTURE) != 0) out.halfmove = 0;

  if ((m.flags & F_EP) != 0) out.squares[m.to + (side == 0 ? 16 : -16)] = 0;
  out.squares[m.from] = 0;
  if ((m.flags & F_PROMO) != 0) {
    out.squares[m.to] = side == 0 ? m.promo : static_cast<int8_t>(-m.promo);
  } else {
    out.squares[m.to] = piece;
  }

  if ((m.flags & F_CASTLE) != 0) {
    if (colOf(m.to) == 6) {  // corto: la torre salta del borde al lado del rey
      out.squares[m.from + 1] = out.squares[m.from + 3];
      out.squares[m.from + 3] = 0;
    } else {  // largo
      out.squares[m.from - 1] = out.squares[m.from - 4];
      out.squares[m.from - 4] = 0;
    }
  }
  if (type == KING) out.kingSquare[side] = m.to;
  if ((m.flags & F_DOUBLE) != 0) out.ep = static_cast<int8_t>((static_cast<int>(m.from) + m.to) / 2);

  // Derechos de enroque: se pierden al mover el rey o la torre, y también
  // cuando le comen la torre en su casilla de origen.
  const int from = m.from, to = m.to;
  if (from == squareOf(7, 4) || to == squareOf(7, 4)) out.castling &= static_cast<uint8_t>(~(C_WK | C_WQ));
  if (from == squareOf(7, 0) || to == squareOf(7, 0)) out.castling &= static_cast<uint8_t>(~C_WQ);
  if (from == squareOf(7, 7) || to == squareOf(7, 7)) out.castling &= static_cast<uint8_t>(~C_WK);
  if (from == squareOf(0, 4) || to == squareOf(0, 4)) out.castling &= static_cast<uint8_t>(~(C_BK | C_BQ));
  if (from == squareOf(0, 0) || to == squareOf(0, 0)) out.castling &= static_cast<uint8_t>(~C_BQ);
  if (from == squareOf(0, 7) || to == squareOf(0, 7)) out.castling &= static_cast<uint8_t>(~C_BK);

  out.side = static_cast<uint8_t>(1 - side);
  return !isAttacked(out, out.kingSquare[side], out.side);
}

void ChessActivity::generateLegal(const Position& p, MoveList& out) {
  MoveList pseudo;
  generateMoves(p, pseudo);
  out.clear();
  Position next;
  for (uint16_t i = 0; i < pseudo.count; ++i) {
    if (tryMakeMove(p, pseudo.items[i], next)) out.add(pseudo.items[i]);
  }
}

// Tablas por material insuficiente: rey solo, rey y alfil, rey y caballo.
bool ChessActivity::insufficientMaterial(const Position& p) {
  int minors[2] = {0, 0};
  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq)) continue;
    const int8_t piece = p.squares[sq];
    if (piece == 0) continue;
    const int8_t type = typeOf(piece);
    if (type == KING) continue;
    if (type == PAWN || type == ROOK || type == QUEEN) return false;
    if (++minors[sideOf(piece)] > 1) return false;
  }
  return true;
}

// ================================================================ máquina ===

int ChessActivity::evaluate(const Position& p) {
  int material = 0;
  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq)) continue;
    const int8_t piece = p.squares[sq];
    if (piece == 0) continue;
    material += PIECE_VALUE[typeOf(piece)];
  }
  const bool endgame = material < 2 * PIECE_VALUE[QUEEN] + 2 * PIECE_VALUE[ROOK];

  int score = 0;
  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq)) continue;
    const int8_t piece = p.squares[sq];
    if (piece == 0) continue;
    const int8_t type = typeOf(piece);
    const int row = rowOf(sq), col = colOf(sq);
    // Las tablas están escritas con las blancas abajo; para las negras se lee
    // la fila espejada.
    const int idx = piece > 0 ? row * 8 + col : (7 - row) * 8 + col;
    const int value = PIECE_VALUE[type] + pstFor(type, endgame)[idx];
    score += piece > 0 ? value : -value;
  }
  return score;
}

// MVV-LVA: primero las capturas de pieza gorda con pieza flaca, después las
// coronaciones y por último lo tranquilo. Es lo que hace que la poda sirva.
void ChessActivity::orderMoves(const Position& p, MoveList& ml) {
  int16_t* scores = orderScores.data();
  for (uint16_t i = 0; i < ml.count; ++i) {
    const Move& m = ml.items[i];
    int s = 0;
    if ((m.flags & F_CAPTURE) != 0) {
      const int8_t victim = (m.flags & F_EP) != 0 ? static_cast<int8_t>(PAWN) : typeOf(p.squares[m.to]);
      const int8_t attacker = typeOf(p.squares[m.from]);
      s = 10000 + PIECE_VALUE[victim] * 8 - PIECE_VALUE[attacker];
    }
    if ((m.flags & F_PROMO) != 0) s += 9000 + PIECE_VALUE[m.promo];
    if ((m.flags & F_CASTLE) != 0) s += 50;
    scores[i] = static_cast<int16_t>(s);
  }
  for (int i = 1; i < static_cast<int>(ml.count); ++i) {  // inserción: la lista es corta
    const Move m = ml.items[i];
    const int16_t s = scores[i];
    int j = i - 1;
    while (j >= 0 && scores[j] < s) {
      scores[j + 1] = scores[j];
      ml.items[j + 1] = ml.items[j];
      --j;
    }
    scores[j + 1] = s;
    ml.items[j + 1] = m;
  }
}

bool ChessActivity::outOfTime() {
  if ((++nodes & 511) != 0) return aiAborted;
  if (millis() > sliceDeadline) aiAborted = true;
  return aiAborted;
}

// Búsqueda de capturas: sin esto la máquina cambia piezas justo en el corte de
// la profundidad y cree que ganó material que en realidad devuelve enseguida.
int ChessActivity::quiesce(const int ply, const int qdepth, int alpha, const int beta) {
  if (outOfTime()) return 0;
  const Position& p = plyPos[ply];
  const int stand = p.side == 0 ? evaluate(p) : -evaluate(p);
  if (qdepth <= 0 || ply >= MAX_PLY - 2) return stand;
  if (stand >= beta) return beta;
  if (stand > alpha) alpha = stand;

  MoveList& ml = plyMoves[ply];
  generateMoves(p, ml);
  orderMoves(p, ml);
  for (uint16_t i = 0; i < ml.count; ++i) {
    const Move& m = ml.items[i];
    if ((m.flags & (F_CAPTURE | F_PROMO)) == 0) continue;
    if (!tryMakeMove(p, m, plyPos[ply + 1])) continue;
    const int v = -quiesce(ply + 1, qdepth - 1, -beta, -alpha);
    if (aiAborted) return 0;
    if (v >= beta) return beta;
    if (v > alpha) alpha = v;
  }
  return alpha;
}

int ChessActivity::search(const int ply, const int depth, int alpha, const int beta) {
  if (outOfTime()) return 0;
  if (depth <= 0 || ply >= MAX_PLY - 2) return quiesce(ply, QS_PLIES, alpha, beta);

  const Position& p = plyPos[ply];
  MoveList& ml = plyMoves[ply];
  generateMoves(p, ml);
  orderMoves(p, ml);

  int legal = 0;
  int best = -INF_SCORE;
  for (uint16_t i = 0; i < ml.count; ++i) {
    if (!tryMakeMove(p, ml.items[i], plyPos[ply + 1])) continue;
    ++legal;
    const int v = -search(ply + 1, depth - 1, -beta, -alpha);
    if (aiAborted) return 0;
    if (v > best) best = v;
    if (best > alpha) alpha = best;
    if (alpha >= beta) break;  // poda
  }
  if (legal == 0) return inCheck(p, p.side) ? -MATE_SCORE + ply : 0;  // mate o ahogado
  return best;
}

// Una movida raíz por pasada del loop, y una profundidad tras otra hasta
// gastar el presupuesto: así la pantalla y los botones siguen vivos y el tiempo
// por jugada queda acotado.
void ChessActivity::stepAi() {
  if (!aiPrepared) {
    generateLegal(pos, rootMoves);
    if (rootMoves.count == 0) {
      const bool mate = inCheck(pos, pos.side);
      gameOver(mate ? (pos.side == 0 ? RES_BLACK : RES_WHITE) : RES_DRAW, mate ? END_MATE : END_STALEMATE);
      return;
    }
    orderMoves(pos, rootMoves);
    aiPrepared = true;
    aiDepth = 1;
    aiIndex = 0;
    aiAlpha = -INF_SCORE;
    aiIterBest = 0;
    aiIterBestScore = -INF_SCORE;
    aiIterTies = 0;
    aiIterAborted = false;
    aiAborted = false;
    aiBestIndex = 0;
    aiBestScore = 0;
    aiBestDepth = 0;
    aiStartMs = millis();
    nodes = 0;
    return;  // la pantalla ya dice "Pensando"; la búsqueda arranca en la próxima pasada
  }

  if (aiIndex < static_cast<int>(rootMoves.count)) {
    sliceDeadline = millis() + SLICE_MS;
    aiAborted = false;
    if (!tryMakeMove(pos, rootMoves.items[aiIndex], plyPos[1])) {
      ++aiIndex;  // no debería pasar: rootMoves ya son legales
      return;
    }
    const int v = -search(1, aiDepth - 1, -INF_SCORE, -aiAlpha);
    if (aiAborted) {  // se acabó el tiempo: esta profundidad no vale
      aiIterAborted = true;
      aiIndex = static_cast<int>(rootMoves.count);
      return;
    }
    if (v > aiIterBestScore) {
      aiIterBestScore = v;
      aiIterBest = aiIndex;
      aiIterTies = 1;
      if (v > aiAlpha) aiAlpha = v;
    } else if (v == aiIterBestScore) {
      // Entre movidas igual de buenas, una al azar: dos partidas no salen iguales.
      ++aiIterTies;
      if (random(aiIterTies) == 0) aiIterBest = aiIndex;
    }
    ++aiIndex;
    return;
  }

  if (!aiIterAborted) {
    aiBestIndex = aiIterBest;
    aiBestScore = aiIterBestScore;
    aiBestDepth = aiDepth;
    if (aiBestIndex != 0) {  // la mejor al frente: ordena la próxima profundidad
      const Move tmp = rootMoves.items[0];
      rootMoves.items[0] = rootMoves.items[aiBestIndex];
      rootMoves.items[aiBestIndex] = tmp;
      aiBestIndex = 0;
    }
  }

  const bool timeUp = millis() - aiStartMs >= THINK_BUDGET_MS;
  const bool mateFound = aiBestScore > MATE_SCORE - 100 || aiBestScore < -MATE_SCORE + 100;
  if (aiIterAborted || timeUp || aiDepth >= MAX_DEPTH || mateFound || rootMoves.count == 1) {
    playAiMove(rootMoves.items[aiBestIndex]);
    return;
  }
  ++aiDepth;
  aiIndex = 0;
  aiAlpha = -INF_SCORE;
  aiIterBest = 0;
  aiIterBestScore = -INF_SCORE;
  aiIterTies = 0;
  aiIterAborted = false;
}

void ChessActivity::playAiMove(const Move& m) {
  Position next;
  if (!tryMakeMove(pos, m, next)) {  // salvavidas: nunca debería fallar
    aiPrepared = false;
    return;
  }
  formatMove(pos, m, next, lastMoveText, sizeof(lastMoveText));
  lastFrom = m.from;
  lastTo = m.to;
  if (pos.side == 1) ++fullMoveNumber;
  pos = next;
  aiPrepared = false;
  afterMove();
}

// ============================================================== notación ===

namespace {
// La primera letra (el primer punto de código UTF-8) de un nombre traducido.
// Copiar un solo byte partiría al medio la "Ф" de "Ферзь".
void firstLetter(const char* name, char* out, const size_t outSize) {
  if (outSize == 0) return;
  out[0] = '\0';
  if (!name || !*name) return;
  const auto lead = static_cast<unsigned char>(name[0]);
  size_t len = 1;
  if (lead >= 0xF0) len = 4;
  else if (lead >= 0xE0) len = 3;
  else if (lead >= 0xC0) len = 2;
  const size_t available = strlen(name);
  if (len > available) len = available;
  if (len >= outSize) len = outSize - 1;
  memcpy(out, name, len);
  out[len] = '\0';
}
}  // namespace

void ChessActivity::formatMove(const Position& before, const Move& m, const Position& after, char* out,
                               const size_t outSize) {
  char body[24];
  if ((m.flags & F_CASTLE) != 0) {
    snprintf(body, sizeof(body), "%s", colOf(m.to) == 6 ? "O-O" : "O-O-O");
  } else {
    const char fromFile = static_cast<char>('a' + colOf(m.from));
    const char fromRank = static_cast<char>('8' - rowOf(m.from));
    const char toFile = static_cast<char>('a' + colOf(m.to));
    const char toRank = static_cast<char>('8' - rowOf(m.to));
    if ((m.flags & F_PROMO) != 0) {
      StrId name = StrId::STR_GAME_QUEEN;
      if (m.promo == ROOK) name = StrId::STR_GAME_ROOK;
      if (m.promo == BISHOP) name = StrId::STR_GAME_BISHOP;
      if (m.promo == KNIGHT) name = StrId::STR_GAME_KNIGHT;
      // La INICIAL de la pieza, no el nombre entero: "e7e8=Dama" (y peor,
      // "e7e8=Springer") no entra en la columna de la franja de marcadores en
      // ningún idioma, y lo que se recortaba era justo la pieza coronada. En
      // los seis idiomas las cuatro piezas empiezan con letras distintas.
      char initial[8];
      firstLetter(I18N.get(name), initial, sizeof(initial));
      snprintf(body, sizeof(body), "%c%c%c%c=%s", fromFile, fromRank, toFile, toRank, initial);
    } else {
      snprintf(body, sizeof(body), "%c%c%c%c", fromFile, fromRank, toFile, toRank);
    }
  }
  (void)before;

  const char* suffix = "";
  if (inCheck(after, after.side)) {
    MoveList replies;
    generateLegal(after, replies);
    suffix = replies.count == 0 ? "#" : "+";
  }
  snprintf(out, outSize, "%s%s", body, suffix);
}

// ================================================================ partida ===

void ChessActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = MODE_SELECT;
  modeCursor = 0;
  forceClean = true;
  requestUpdate();
}

bool ChessActivity::machinePlays(const int side) const {
  if (mode == TWO_PLAYERS) return false;
  return mode == VS_MACHINE_WHITE ? side == 1 : side == 0;
}

void ChessActivity::startGame(const Mode m) {
  mode = m;
  boardFlipped = m == VS_MACHINE_BLACK;  // las piezas del humano, siempre abajo
  newGame();
}

void ChessActivity::newGame() {
  setupInitial(pos);
  result = RES_NONE;
  endReason = END_NONE;
  fullMoveNumber = 1;
  lastFrom = lastTo = -1;
  lastMoveText[0] = '\0';
  aiPrepared = false;
  pieceCursor = destCursor = promoCursor = 0;
  destCount = promoCount = 0;
  partialCount = 0;
  forceClean = true;
  if (machinePlays(pos.side)) {
    state = AI_TURN;
  } else {
    state = PICK_PIECE;
    refreshSelection();
  }
  requestUpdate();
}

// Arma la lista de piezas jugables en orden de lectura de la PANTALLA: eso es
// el zigzag que recorre la palanca.
void ChessActivity::refreshSelection() {
  generateLegal(pos, legalMoves);
  pieceCount = 0;
  destCount = 0;
  for (int dr = 0; dr < 8; ++dr) {
    for (int dc = 0; dc < 8; ++dc) {
      const int sq = squareFromDisplay(dr, dc);
      bool playable = false;
      for (uint16_t i = 0; i < legalMoves.count && !playable; ++i) playable = legalMoves.items[i].from == sq;
      if (!playable) continue;
      if (pieceCount < pieceList.size()) pieceList[pieceCount++] = static_cast<uint8_t>(sq);
    }
  }
  if (pieceCursor >= pieceCount) pieceCursor = 0;
}

int ChessActivity::squareFromDisplay(const int dr, const int dc) const {
  const int row = boardFlipped ? 7 - dr : dr;
  const int col = boardFlipped ? 7 - dc : dc;
  return squareOf(row, col);
}

void ChessActivity::afterMove() {
  if (pos.halfmove >= 100) {
    gameOver(RES_DRAW, END_FIFTY);
    return;
  }
  if (insufficientMaterial(pos)) {
    gameOver(RES_DRAW, END_MATERIAL);
    return;
  }
  generateLegal(pos, legalMoves);
  if (legalMoves.count == 0) {
    const bool mate = inCheck(pos, pos.side);
    gameOver(mate ? (pos.side == 0 ? RES_BLACK : RES_WHITE) : RES_DRAW, mate ? END_MATE : END_STALEMATE);
    return;
  }
  if (machinePlays(pos.side)) {
    state = AI_TURN;
    aiPrepared = false;
  } else {
    state = PICK_PIECE;
    pieceCursor = 0;
    destCount = 0;
    refreshSelection();
  }
  requestUpdate();
}

void ChessActivity::gameOver(const Result r, const EndReason reason) {
  state = GAME_OVER;
  result = r;
  endReason = reason;
  forceClean = true;
  requestUpdate();
}

void ChessActivity::moveCursor(const int dir) {
  if (state == MODE_SELECT) {
    modeCursor = dir > 0 ? ButtonNavigator::nextIndex(modeCursor, 3) : ButtonNavigator::previousIndex(modeCursor, 3);
  } else if (state == PICK_PIECE && pieceCount > 1) {
    pieceCursor = dir > 0 ? ButtonNavigator::nextIndex(pieceCursor, pieceCount)
                          : ButtonNavigator::previousIndex(pieceCursor, pieceCount);
  } else if (state == PICK_MOVE && destCount > 1) {
    destCursor = dir > 0 ? ButtonNavigator::nextIndex(destCursor, destCount)
                         : ButtonNavigator::previousIndex(destCursor, destCount);
  } else if (state == PROMOTE && promoCount > 1) {
    promoCursor = dir > 0 ? ButtonNavigator::nextIndex(promoCursor, promoCount)
                          : ButtonNavigator::previousIndex(promoCursor, promoCount);
  } else {
    return;  // con una sola opción no se repinta: sería un destello de gusto
  }
  requestUpdate();
}

void ChessActivity::selectPiece() {
  if (pieceCount == 0) return;
  const uint8_t sq = pieceList[pieceCursor];
  destCount = 0;
  // Destinos también en orden de lectura de la pantalla, y sin repetir la
  // casilla cuando hay coronación (esa se elige después).
  for (int dr = 0; dr < 8; ++dr) {
    for (int dc = 0; dc < 8; ++dc) {
      const int target = squareFromDisplay(dr, dc);
      for (uint16_t i = 0; i < legalMoves.count; ++i) {
        const Move& m = legalMoves.items[i];
        if (m.from != sq || m.to != target) continue;
        if ((m.flags & F_PROMO) != 0 && m.promo != QUEEN) continue;  // una sola entrada por casilla
        if (destCount < destMove.size()) destMove[destCount++] = static_cast<uint8_t>(i);
        break;
      }
    }
  }
  if (destCount == 0) return;
  destCursor = 0;
  state = PICK_MOVE;
  requestUpdate();
}

void ChessActivity::confirmDestination() {
  if (destCount == 0) return;
  const Move& chosen = legalMoves.items[destMove[destCursor]];
  if ((chosen.flags & F_PROMO) == 0) {
    applyPlayerMove(chosen);
    return;
  }
  // Coronación: la palanca elige entre dama, torre, alfil y caballo.
  promoCount = 0;
  for (int8_t type = QUEEN; type >= KNIGHT; --type) {
    for (uint16_t i = 0; i < legalMoves.count; ++i) {
      const Move& m = legalMoves.items[i];
      if (m.from != chosen.from || m.to != chosen.to || m.promo != type) continue;
      if (promoCount < promoMove.size()) promoMove[promoCount++] = static_cast<uint8_t>(i);
      break;
    }
  }
  if (promoCount == 0) return;
  promoCursor = 0;
  state = PROMOTE;
  requestUpdate();
}

void ChessActivity::applyPlayerMove(const Move& m) {
  Position next;
  if (!tryMakeMove(pos, m, next)) return;
  formatMove(pos, m, next, lastMoveText, sizeof(lastMoveText));
  lastFrom = m.from;
  lastTo = m.to;
  if (pos.side == 1) ++fullMoveNumber;
  pos = next;
  destCount = 0;
  promoCount = 0;
  forceClean = true;
  afterMove();
}

// =================================================================== loop ===

void ChessActivity::loop() {
  // Atrás mantenido primero: se come la suelta, así no sale del juego.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    state = MODE_SELECT;
    aiPrepared = false;
    forceClean = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == PROMOTE) {
      state = PICK_MOVE;
      promoCount = 0;
      requestUpdate();
      return;
    }
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
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = MODE_SELECT;
      forceClean = true;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] { moveCursor(1); });
  buttonNavigator.onPrevious([this] { moveCursor(-1); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case MODE_SELECT:
        startGame(modeCursor == 0 ? VS_MACHINE_WHITE : modeCursor == 1 ? VS_MACHINE_BLACK : TWO_PLAYERS);
        break;
      case PICK_PIECE:
        selectPiece();
        break;
      case PICK_MOVE:
        confirmDestination();
        break;
      case PROMOTE:
        if (promoCount > 0) applyPlayerMove(legalMoves.items[promoMove[promoCursor]]);
        break;
      default:
        break;
    }
  }
}

// ================================================================= dibujo ===

void ChessActivity::drawCornerTicks(const int x, const int y, const int cell, const int arm,
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

// Blancas: relleno blanco con contorno negro grueso. Negras: silueta maciza.
// Las dos con halo blanco para que la trama de la casilla no las coma.
void ChessActivity::drawPiece(const int x, const int y, const int cell, const int8_t piece) const {
  if (piece == 0 || cell < 16) return;
  const int dim = cell > MAX_CELL ? MAX_CELL : cell;
  uint8_t bits[MASK_BYTES];
  uint8_t halo[MASK_BYTES];
  memset(bits, 0, sizeof(bits));
  memset(halo, 0, sizeof(halo));

  const Mask mask{bits, dim, 3, dim - 6};
  buildPieceMask(mask, typeOf(piece));

  // Halo: la máscara dilatada dos píxeles.
  const Mask haloMask{halo, dim, 3, dim - 6};
  for (int py = 0; py < dim; ++py) {
    for (int px = 0; px < dim; ++px) {
      if (!mask.get(px, py)) continue;
      for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) haloMask.put(px + dx, py + dy, true);
    }
  }

  const bool black = piece < 0;
  for (int py = 0; py < dim; ++py) {
    for (int px = 0; px < dim; ++px) {
      const bool inside = mask.get(px, py);
      if (!inside && !haloMask.get(px, py)) continue;
      bool ink = false;
      if (inside) {
        if (black) {
          ink = true;
        } else {
          // Contorno de 2 px: píxel de la máscara con algún vecino afuera.
          ink = !mask.get(px - 2, py) || !mask.get(px + 2, py) || !mask.get(px, py - 2) || !mask.get(px, py + 2) ||
                !mask.get(px - 2, py - 2) || !mask.get(px + 2, py + 2) || !mask.get(px - 2, py + 2) ||
                !mask.get(px + 2, py - 2);
        }
      }
      renderer.drawPixel(x + px, y + py, ink);
    }
  }
}

void ChessActivity::drawBoard(const int left, const int top, const int cell) const {
  const int size = cell * 8;

  renderer.drawRect(left - FRAME_GAP, top - FRAME_GAP, size + 2 * FRAME_GAP, size + 2 * FRAME_GAP, 4, true);
  renderer.drawRect(left - 2, top - 2, size + 4, size + 4, 2, true);

  int cursorSq = -1;
  int sourceSq = -1;
  if (state == PICK_PIECE && pieceCount > 0) cursorSq = pieceList[pieceCursor];
  if ((state == PICK_MOVE || state == PROMOTE) && destCount > 0) {
    const Move& m = legalMoves.items[destMove[destCursor]];
    cursorSq = m.to;
    sourceSq = m.from;
  }

  // Fondo: trama al 25 % en las oscuras y blanco en las claras. Van SIN trama la
  // casilla del cursor, la de la pieza elegida y las dos de la última movida:
  // destramarlas las hace inconfundibles, y el marco de 2 px de la última
  // movida sobre la trama no se leería.
  for (int dr = 0; dr < 8; ++dr) {
    for (int dc = 0; dc < 8; ++dc) {
      const int sq = squareFromDisplay(dr, dc);
      if (((rowOf(sq) + colOf(sq)) & 1) == 0) continue;
      if (sq == cursorSq || sq == sourceSq || sq == lastFrom || sq == lastTo) continue;
      gameui::shadeCell(renderer, left + dc * cell, top + dr * cell, cell);
    }
  }

  for (int i = 1; i < 8; ++i) {
    renderer.drawLine(left + i * cell, top, left + i * cell, top + size - 1, true);
    renderer.drawLine(left, top + i * cell, left + size - 1, top + i * cell, true);
  }

  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq) || pos.squares[sq] == 0) continue;
    drawPiece(left + displayCol(sq) * cell, top + displayRow(sq) * cell, cell, pos.squares[sq]);
  }

  // La última movida (la de la máquina, o la del otro jugador en el modo de a
  // dos): marco de 2 px en el origen y en el destino, DESPUÉS de las piezas (el
  // halo blanco de la pieza se comería el marco). Hasta 1.5.47 eran dos
  // cuadraditos de 6 px, invisibles a un palmo.
  for (int sq = 0; sq < 128; ++sq) {
    if (!onBoard(sq) || (sq != lastFrom && sq != lastTo)) continue;
    gameui::lastMoveFrame(renderer, left + displayCol(sq) * cell, top + displayRow(sq) * cell, cell);
  }

  // El rey en jaque: marco pegado al borde de la casilla, por fuera del de la
  // última movida (que va 2 px adentro), así los dos se ven si coinciden.
  if (state != GAME_OVER && inCheck(pos, pos.side)) {
    const int sq = pos.kingSquare[pos.side];
    renderer.drawRect(left + displayCol(sq) * cell, top + displayRow(sq) * cell, cell, cell, 2, true);
  }

  if (state == PICK_PIECE) {
    for (uint8_t i = 0; i < pieceCount; ++i) {
      const int sq = pieceList[i];
      if (sq == cursorSq) continue;
      drawCornerTicks(left + displayCol(sq) * cell, top + displayRow(sq) * cell, cell, cell / 3, 3);
    }
  } else if (state == PICK_MOVE || state == PROMOTE) {
    for (uint8_t i = 0; i < destCount; ++i) {
      const int sq = legalMoves.items[destMove[i]].to;
      if (sq == cursorSq) continue;
      drawCornerTicks(left + displayCol(sq) * cell, top + displayRow(sq) * cell, cell, cell / 3, 3);
    }
  }

  if (sourceSq >= 0) {
    const int x = left + displayCol(sourceSq) * cell, y = top + displayRow(sourceSq) * cell;
    renderer.drawRect(x + 1, y + 1, cell - 2, cell - 2, 2, true);
  }
  if (cursorSq >= 0)
    gameui::cursorFrame(renderer, left + displayCol(cursorSq) * cell, top + displayRow(cursorSq) * cell, cell);

  // Coordenadas afuera del marco: sirven para leer la última movida.
  char label[2] = {0, 0};
  for (int dc = 0; dc < 8; ++dc) {
    label[0] = static_cast<char>('a' + colOf(squareFromDisplay(0, dc)));
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, left + dc * cell + (cell - w) / 2, top + size + FRAME_GAP + 2, label);
  }
  for (int dr = 0; dr < 8; ++dr) {
    label[0] = static_cast<char>('8' - rowOf(squareFromDisplay(dr, 0)));
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, left - FRAME_GAP - 4 - w, top + dr * cell + cell / 2 - 7, label);
  }
}

void ChessActivity::drawModeSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int width = pageWidth - 2 * SIDE;
  const int top = metrics.topPadding + metrics.headerHeight + 2 * gameui::GAP;

  // Encabezado de sección: UI_14 alineado a la izquierda y una regla de 1 px al
  // pie. Nada de marcos: la jerarquía la hace la tipografía.
  renderer.drawText(UI_14_FONT_ID, SIDE, top, tr(STR_GAME_MODE));
  const int listTop = top + renderer.getLineHeight(UI_14_FONT_ID) + gameui::GAP;
  gameui::rule(renderer, SIDE, listTop - gameui::GAP / 2, width);

  static const StrId MODES[3] = {StrId::STR_GAME_VS_MACHINE_WHITE, StrId::STR_GAME_VS_MACHINE_BLACK,
                                 StrId::STR_GAME_TWO_PLAYERS};
  const int textLine = renderer.getLineHeight(UI_12_FONT_ID);
  for (int i = 0; i < 3; ++i) {
    const int y = listTop + i * ROW_H;
    if (i == modeCursor) {
      // Radio 0 y estilo Row: el resalte deja el centro BLANCO, así que el
      // renglón elegido se lee igual que los otros dos.
      drawSelectionRow(renderer, SIDE, y, width, ROW_H, 0);
    } else {
      gameui::rule(renderer, SIDE, y + ROW_H - 1, width);
    }
    // El texto arranca a 24 px del borde de la fila: por dentro de las franjas
    // tramadas que dibuja el resalte, nunca encima.
    const char* label = I18N.get(MODES[i]);
    const std::string shown = renderer.truncatedText(UI_12_FONT_ID, label, width - 48);
    renderer.drawText(UI_12_FONT_ID, SIDE + 24, y + (ROW_H - textLine) / 2, shown.c_str(), SELECTION_INK);
  }

  // Una muestra de las seis piezas, para que se vea cómo se distinguen.
  const int sampleY = listTop + 3 * ROW_H + 3 * gameui::GAP;
  const int cell = 44;
  const int startX = (pageWidth - 6 * cell) / 2;
  for (int i = 0; i < 6; ++i) {
    drawPiece(startX + i * cell, sampleY, cell, static_cast<int8_t>(i + 1));
    drawPiece(startX + i * cell, sampleY + cell + 4, cell, static_cast<int8_t>(-(i + 1)));
  }
}

// Las cuatro coronaciones, una al lado de la otra. La elegida usa el resalte de
// filas (estilo Row, radio 0): las franjas tramadas quedan en los costados de la
// celda y el texto, centrado, cae siempre sobre el blanco del medio.
void ChessActivity::drawPromotionBar(const int top) const {
  const int pageWidth = renderer.getScreenWidth();
  const int width = (pageWidth - 2 * SIDE) / 4;
  const int height = renderer.getLineHeight(UI_12_FONT_ID) + 2 * gameui::GAP;
  for (uint8_t i = 0; i < promoCount; ++i) {
    const int x = SIDE + i * width;
    if (i == promoCursor) drawSelectionRow(renderer, x + 2, top, width - 4, height, 0);
    const int8_t promo = legalMoves.items[promoMove[i]].promo;
    StrId name = StrId::STR_GAME_QUEEN;
    if (promo == ROOK) name = StrId::STR_GAME_ROOK;
    if (promo == BISHOP) name = StrId::STR_GAME_BISHOP;
    if (promo == KNIGHT) name = StrId::STR_GAME_KNIGHT;
    // 42 = las dos franjas tramadas de 16 px del resalte más sus márgenes: el
    // texto se queda con el blanco del medio y nunca cae sobre la trama.
    const std::string shown = renderer.truncatedText(UI_12_FONT_ID, I18N.get(name), width - 42);
    const int w = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str());
    renderer.drawText(UI_12_FONT_ID, x + (width - w) / 2, top + gameui::GAP, shown.c_str(), SELECTION_INK);
  }
}

void ChessActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentW = gameui::contentWidth(renderer);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_CHESS));

  // La ayuda reserva su alto SIEMPRE, en todos los estados: así el tablero no
  // se mueve cuando cambia el renglón de abajo.
  const int helpTop = bottom - gameui::helpHeight(renderer);

  if (state == MODE_SELECT) {
    drawModeSelect();
    gameui::help(renderer, helpTop, tr(STR_GAME_HELP_MODE));
    const auto labels =
        mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    partialCount = 0;
    forceClean = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // De abajo hacia arriba: ayuda, marcadores (o la barra de coronación, que va
  // en el mismo lugar), estado; lo que sobra es el tablero con su fila de
  // coordenadas.
  const int statsTop = helpTop - gameui::statsHeight(renderer);
  const int statusTop = statsTop - gameui::GAP - gameui::statusHeight(renderer);
  const int boardBand = statusTop - gameui::GAP - COORD_H - (headerBottom + gameui::GAP);
  int cell = std::min(contentW / 8, boardBand / 8);
  cell = std::min(cell, MAX_CELL);
  cell = std::max(cell, MIN_CELL);
  const int size = cell * 8;
  const int boardLeft = (pageWidth - size) / 2;
  int boardTop = headerBottom + gameui::GAP + (boardBand - size) / 2;
  if (boardTop < headerBottom + FRAME_GAP) boardTop = headerBottom + FRAME_GAP;

  drawBoard(boardLeft, boardTop, cell);

  // Estado: a quién le toca (y si está en jaque) a la izquierda, y a la derecha
  // qué está eligiendo y en qué lugar de la lista va.
  const char* title = pos.side == 0 ? tr(STR_GAME_WHITE) : tr(STR_GAME_BLACK);
  if (state == AI_TURN) title = tr(STR_GAME_THINKING);
  if (state == GAME_OVER) {
    if (endReason == END_MATE) {
      title = result == RES_WHITE ? tr(STR_GAME_WHITE_WINS) : tr(STR_GAME_BLACK_WINS);
    } else {
      title = tr(STR_GAME_DRAW);
    }
  }
  char titleBuf[96];
  if (state != GAME_OVER && inCheck(pos, pos.side)) {
    snprintf(titleBuf, sizeof(titleBuf), "%s · %s", title, tr(STR_GAME_CHECK));
    title = titleBuf;
  }

  char detail[96] = "";
  char counter[24];
  switch (state) {
    case PICK_PIECE:
      if (pieceCount > 0) {
        snprintf(counter, sizeof(counter), tr(STR_GAME_OF_COUNT), pieceCursor + 1, pieceCount);
        snprintf(detail, sizeof(detail), "%s · %s", tr(STR_GAME_SELECT_PIECE), counter);
      }
      break;
    case PICK_MOVE:
      if (destCount > 0) {
        snprintf(counter, sizeof(counter), tr(STR_GAME_OF_COUNT), destCursor + 1, destCount);
        snprintf(detail, sizeof(detail), "%s · %s", tr(STR_GAME_SELECT_MOVE), counter);
      }
      break;
    case PROMOTE:
      snprintf(detail, sizeof(detail), "%s", tr(STR_GAME_PROMOTE));
      break;
    case GAME_OVER:
      snprintf(detail, sizeof(detail), "%s",
               endReason == END_MATE        ? tr(STR_GAME_CHECKMATE)
               : endReason == END_STALEMATE ? tr(STR_GAME_STALEMATE)
                                            : tr(STR_GAME_DRAW));
      break;
    default:
      break;
  }
  gameui::status(renderer, SIDE, statusTop, contentW, title, detail);

  if (state == PROMOTE) {
    // Misma regla de 1 px que abre la franja de marcadores: la coronación ocupa
    // su lugar, no abre una superficie nueva.
    gameui::rule(renderer, SIDE, statsTop, contentW);
    drawPromotionBar(statsTop + gameui::GAP);
  } else {
    // Marcadores: el material de cada bando (peón 1, caballo y alfil 3, torre 5,
    // dama 9) y la última movida en notación. Van en UI_14 porque es lo único
    // que se mira entre jugada y jugada.
    int whitePoints = 0, blackPoints = 0;
    for (int sq = 0; sq < 128; ++sq) {
      if (!onBoard(sq)) continue;
      const int8_t p = pos.squares[sq];
      if (p == 0) continue;
      const int value = PIECE_VALUE[typeOf(p)] / 100;
      if (p > 0) whitePoints += value;
      else blackPoints += value;
    }
    char vWhite[8], vBlack[8], vMove[32];
    snprintf(vWhite, sizeof(vWhite), "%d", whitePoints);
    snprintf(vBlack, sizeof(vBlack), "%d", blackPoints);
    if (lastMoveText[0] != '\0') snprintf(vMove, sizeof(vMove), "%d. %s", fullMoveNumber, lastMoveText);
    else snprintf(vMove, sizeof(vMove), "%d", fullMoveNumber);
    // La columna de la movida pide el DOBLE de ancho: en tres columnas iguales
    // (144 px) toda coronación y hasta "42. O-O-O+" salían con puntos
    // suspensivos, o sea que lo que había que leer para entender la jugada era
    // justo lo que desaparecía.
    const gameui::Stat scoreboard[3] = {
        {vWhite, tr(STR_GAME_WHITE)}, {vBlack, tr(STR_GAME_BLACK)}, {vMove, tr(STR_GAME_MOVES), 2}};
    gameui::stats(renderer, SIDE, statsTop, contentW, scoreboard, 3);
  }

  // La ayuda: qué hace la palanca ahora y qué significan los marcos del
  // tablero. Repartida en renglones, nunca cortada.
  char helpText[192];
  const char* what = tr(STR_GAME_HELP_PIECE);
  if (state == PICK_MOVE) what = tr(STR_GAME_HELP_MOVE);
  else if (state == PROMOTE) what = tr(STR_GAME_HELP_PROMOTE);
  else if (state == AI_TURN) what = tr(STR_GAME_HELP_WAIT);
  else if (state == GAME_OVER) what = tr(STR_GAME_HELP_OVER);
  const char* extra = tr(STR_GAME_HELP_RESTART);
  if (state != GAME_OVER && lastFrom >= 0) extra = tr(STR_GAME_HELP_LAST_MOVE);
  snprintf(helpText, sizeof(helpText), "%s %s", what, extra);
  gameui::help(renderer, helpTop, helpText);

  // Las ayudas dicen siempre qué hace cada botón AHORA.
  const char* backLabel = state == PICK_MOVE || state == PROMOTE ? tr(STR_CANCEL) : tr(STR_GAME_QUIT);
  const char* confirmLabel = state == GAME_OVER ? tr(STR_GAME_NEW) : state == AI_TURN ? "" : tr(STR_SELECT);
  const bool navigable = (state == PICK_PIECE && pieceCount > 1) || (state == PICK_MOVE && destCount > 1) ||
                         (state == PROMOTE && promoCount > 1);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, navigable ? tr(STR_DIR_UP) : "",
                                            navigable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
