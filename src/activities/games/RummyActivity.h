#pragma once

#include <Arduino.h>

#include <array>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Gin rummy a dos manos contra la máquina.
//
// De las variantes de rummy es la que mejor entra en un aparato de dos botones:
// se juega de a dos por definición (el rummy 500 y el clásico se arman con tres
// o más), el turno es siempre el mismo — robar una carta y tirar una — y la mano
// se cierra sola cuando cortas, así que no hace falta bajar combinaciones ni
// pegarse a las del otro durante el juego. Una mano dura poco, que es lo que se
// le pide a un juego en un lector de tinta electrónica.
//
// Esquema de control (palanca de un solo eje, OK y Atrás; OK mantenido apaga):
//   - ARRIBA/ABAJO recorren lo que se pueda elegir AHORA: el modo, mazo o
//     descarte, las cartas de la mano, o sí/no al cortar.
//   - OK confirma.
//   - Atrás sale; Atrás mantenido (1 s) vuelve a la elección de modo (partida nueva).
//
// Reglas: mazo de 52, diez cartas cada uno, una carta a la vista y el resto de
// mazo. En tu turno robas del mazo o tomas la carta de arriba del descarte (esa
// no se puede tirar en el mismo turno) y después tiras una. Combinaciones:
// TRÍOS o cuartetos del mismo valor y ESCALERAS de tres o más del mismo palo
// (el as va abajo, no cierra con la K). Lo que queda sin combinar suma puntos
// (as 1, figuras 10, el resto su número). Con 10 puntos o menos puedes CORTAR al
// tirar: se comparan los sobrantes, el que corta se lleva la diferencia, y si el
// otro empata o queda por debajo hay CONTRACORTE (se lleva la diferencia más 25).
// Con cero sobrante es GIN: 25 de premio y el otro no puede descargar cartas
// sobre tus combinaciones. Si el mazo queda en dos cartas, la mano se anula.
//
// El acomodo de la mano no se hace "a ojo": se enumeran todas las combinaciones
// posibles y se busca la repartición de menor sobrante con programación dinámica
// sobre la máscara de cartas usadas (2^11 estados), así lo que se muestra en
// pantalla y lo que se cuenta al final es siempre el mejor acomodo.
class RummyActivity final : public Activity {
 public:
  explicit RummyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Rummy", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // -------------------------------------------------------------- reglas ---
  // Públicas y estáticas a propósito: así se pueden probar de escritorio con
  // g++ armando manos a mano, sin nada de la pantalla en el medio.

  static constexpr int HAND_MAX = 11;    // diez cartas más la que acabas de robar
  static constexpr int MAX_MELDS = 64;   // combinaciones distintas que puede tener una mano
  static constexpr int KNOCK_LIMIT = 10;
  static constexpr int GIN_BONUS = 25;
  static constexpr int UNDERCUT_BONUS = 25;
  static constexpr int TARGET_SCORE = 100;

  // Carta 0..51, igual que en el blackjack: palo = carta / 13 (0 pica,
  // 1 corazón, 2 diamante, 3 trébol) y valor = carta % 13 (0 as, 1..8 del 2 al
  // 9, 9 el 10, 10 J, 11 Q, 12 K). El orden del valor es el de la escalera.
  static int rankOf(const uint8_t card) { return card % 13; }
  static int suitOf(const uint8_t card) { return card / 13; }
  static int cardValue(uint8_t card);

  struct Hand {
    std::array<uint8_t, HAND_MAX> cards{};
    uint8_t count = 0;
    void clear() { count = 0; }
    void add(const uint8_t card) {
      if (count < HAND_MAX) cards[count++] = card;
    }
    void removeAt(const int index) {
      if (index < 0 || index >= count) return;
      for (int i = index; i + 1 < count; ++i) cards[i] = cards[i + 1];
      --count;
    }
  };

  // Mejor acomodo de una mano: hasta cuatro combinaciones (con once cartas no
  // entran más de tres) como máscaras de bits sobre las POSICIONES de la mano.
  struct Layout {
    std::array<uint16_t, 4> melds{};
    uint8_t meldCount = 0;
    uint16_t used = 0;
    int deadwood = 0;
  };

  // Todas las combinaciones que hay en la mano, como máscaras de bits.
  static uint8_t findMelds(const Hand& hand, std::array<uint16_t, MAX_MELDS>& out);
  // Reparto de menor sobrante.
  static void bestLayout(const Hand& hand, Layout& out);
  // Sobrante mínimo tirando UNA carta (nunca la de `forbidden`, que es la que se
  // acaba de tomar del descarte). Deja en `outIndex` la carta que conviene tirar.
  static int bestDiscard(const Hand& hand, int forbidden, int& outIndex);
  // El que no cortó descarga sus cartas sueltas sobre las combinaciones del que
  // cortó; devuelve el sobrante que le queda.
  static int layOff(const Hand& knocker, const Layout& knockerLayout, const Hand& defender,
                    const Layout& defenderLayout);

 private:
  static constexpr unsigned long RESTART_HOLD_MS = 1000;
  static constexpr unsigned long MACHINE_STEP_MS = 900;  // para poder seguir lo que hace la máquina
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;       // regla del panel: limpio cada 12 parciales
  static constexpr int STOCK_FLOOR = 2;                  // con dos cartas en el mazo la mano se anula

  static constexpr int SIDE = 18;
  static constexpr int CARD_W = 56;
  static constexpr int CARD_H = 78;
  static constexpr int CARD_GAP = 6;
  static constexpr int ROW_PITCH = 94;
  static constexpr int MAX_PER_ROW = 6;
  static constexpr int BACK_W = 26;
  static constexpr int BACK_H = 38;
  static constexpr int LOOSE_GROUP = 255;

  enum State { MODE_SELECT, PLAYER_DRAW, PLAYER_DISCARD, KNOCK_ASK, MACHINE_TURN, HAND_OVER, MATCH_OVER };
  enum Mode { MATCH_TO_100, SINGLE_HAND };
  enum HandEnd { END_NONE, END_PLAYER_KNOCK, END_MACHINE_KNOCK, END_VOID };

  // --- mazo y manos ---
  std::array<uint8_t, 52> deck{};
  int stockTop = 0;  // próxima carta del mazo
  Hand player;
  Hand machine;
  std::array<uint8_t, 52> discardPile{};
  uint8_t discardCount = 0;
  Layout playerLayout;
  Layout machineLayout;

  // --- partida ---
  Mode mode = MATCH_TO_100;
  State state = MODE_SELECT;
  HandEnd handEnd = END_NONE;
  int playerScore = 0;
  int machineScore = 0;
  int handPoints = 0;
  int playerDeadwood = 0;
  int machineDeadwood = 0;
  bool ginHand = false;
  bool undercut = false;
  bool playerWonHand = false;
  int handNumber = 1;

  // --- cursores ---
  int modeCursor = 0;
  int drawCursor = 0;   // 0 mazo, 1 descarte
  int handCursor = 0;   // posición dentro de displayOrder
  int knockCursor = 0;  // 0 sí, 1 no
  int pendingDiscard = -1;
  int forbiddenDiscard = -1;  // la carta recién tomada del descarte no se puede tirar

  // --- orden de la mano en pantalla (combinaciones primero) ---
  std::array<uint8_t, HAND_MAX> displayOrder{};
  std::array<uint8_t, HAND_MAX> displayGroup{};  // índice de combinación, LOOSE_GROUP si va suelta
  uint8_t displayCount = 0;

  // --- turno de la máquina ---
  int machineStep = 0;
  unsigned long machineStepAt = 0;
  bool machineTookDiscard = false;
  uint8_t machineDiscarded = 0;
  bool machineDidSomething = false;

  // --- pantalla ---
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;

  // Partida
  void newMatch(Mode m);
  void dealHand();
  void nextHand();
  void rebuildPlayerView();
  int topDiscard() const { return discardCount > 0 ? discardPile[discardCount - 1] : -1; }
  int stockLeft() const { return 52 - stockTop; }
  uint8_t drawFromStock();
  void playerTakes(bool fromDiscard);
  void playerDiscards(int handIndex, bool knock);
  void startMachineTurn();
  void stepMachine();
  void resolveKnock(bool byPlayer, bool gin);
  void endHandVoid();
  void finishHand();
  void moveCursor(int dir);
  void confirm();
  int selectableCount() const;

  // Dibujo
  void drawMiniCard(int x, int y, uint8_t card, bool selected) const;
  void drawCardBack(int x, int y, int w, int h) const;
  void drawSuit(int cx, int cy, int suit) const;
  void drawRank(int x, int y, uint8_t card) const;
  void drawMachineRow(int top) const;
  void drawCenterRow(int top) const;
  // Ordena la mano para mostrarla: primero las combinaciones (cada una con su
  // número de grupo) y después las cartas sueltas por palo y valor.
  static uint8_t buildOrder(const Hand& hand, const Layout& layout, uint8_t* order, uint8_t* group);
  // Dibuja la mano en filas, con un marco alrededor de cada combinación.
  // Devuelve el alto que usó. `cursor` < 0 = sin selección.
  int drawHandRows(const Hand& hand, const uint8_t* order, const uint8_t* group, uint8_t count, int top,
                   int cursor) const;
  int drawPlayerHand(int top) const;
  void drawModeSelect() const;
  void drawHandOver(int top) const;
  void drawOptionRow(int x, int y, int w, const char* label, bool selected) const;
};
