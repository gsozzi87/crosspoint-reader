#pragma once

#include <Arduino.h>

#include <array>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Blackjack (21) contra la banca, con fichas.
//
// Esquema de control, pensado para los cuatro botones de la placa (la palanca
// ARRIBA/ABAJO es un solo eje y OK no tiene pulsación larga):
//   - ARRIBA/ABAJO: recorren la lista de acciones que se pueden hacer AHORA.
//     Antes de repartir son repartir / +10 / -10 (la apuesta); con las cartas
//     en la mano son pedir carta / plantarse / doblar (x2); terminada la mano,
//     seguir o salir. La lista se arma sola, así que nunca hay una acción
//     imposible en pantalla.
//   - OK: ejecuta la acción resaltada.
//   - Atrás: sale del juego.
//   - Atrás mantenido (1 s): partida nueva con las fichas de arranque.
//
// Reglas: mazo de 52 cartas barajado con Fisher-Yates (se rebaraja cuando
// quedan pocas), el as vale 11 y baja a 1 cuando la mano se pasa, blackjack
// natural (dos cartas) paga 3:2, empate devuelve la apuesta, doblar da una sola
// carta más, y la banca da vuelta la tapada y pide hasta llegar a 17. El
// jugador arranca con 100 fichas y apuesta de a 10; sin fichas para la apuesta
// mínima se termina la partida y OK arranca otra.
//
// Las cartas se dibujan con primitivas (rectángulo redondeado, índice arriba a
// la izquierda y palo grande al medio). Como la pantalla no tiene color, los
// palos se distinguen por la FORMA: pica (punta arriba con pie), corazón (dos
// lóbulos y punta abajo), diamante (rombo) y trébol (tres círculos con pie).
class BlackjackActivity final : public Activity {
 public:
  explicit BlackjackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Blackjack", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int DECK_SIZE = 52;
  static constexpr int MAX_CARDS = 12;      // tope de cartas por mano (de sobra: A+A+...+ se pasa antes)
  static constexpr int START_CHIPS = 100;   // fichas de arranque
  static constexpr int BET_STEP = 10;       // la apuesta se mueve de a 10
  static constexpr int RESHUFFLE_AT = 15;   // rebarajar cuando quedan menos que esto
  static constexpr int DEALER_STANDS = 17;  // la banca pide hasta 17
  static constexpr int BLACKJACK = 21;
  static constexpr unsigned long DEALER_STEP_MS = 700;   // una carta de la banca por vez, para poder seguirla
  static constexpr unsigned long RESTART_HOLD_MS = 1000;  // Atrás mantenido: partida nueva
  static constexpr int PARTIALS_BEFORE_CLEAN = 10;        // regla del panel: limpio cada 10-15 parciales

  // Medidas del dibujo
  static constexpr int SIDE = 20;
  static constexpr int CARD_W = 70;
  static constexpr int CARD_H = 104;
  static constexpr int BADGE_W = 62;
  static constexpr int BADGE_H = 28;
  static constexpr int ACTION_ROW_H = 56;

  enum class State { BETTING, PLAYER, DEALER, RESULT };
  enum class Action { DEAL, BET_UP, BET_DOWN, HIT, STAND, DOUBLE, CONTINUE, NEW_GAME, QUIT };
  enum class Outcome { NONE, WON, LOST, PUSH };

  // Carta = 0..51: palo = carta / 13 (0 pica, 1 corazón, 2 diamante, 3 trébol),
  // valor = carta % 13 (0 as, 1..8 del 2 al 9, 9 el 10, 10 J, 11 Q, 12 K).
  struct Hand {
    std::array<uint8_t, MAX_CARDS> cards{};
    uint8_t count = 0;
    void clear() { count = 0; }
    void add(const uint8_t card) {
      if (count < MAX_CARDS) cards[count++] = card;
    }
  };

  // --- mazo y partida ---
  std::array<uint8_t, DECK_SIZE> deck{};
  int dealt = 0;  // cuántas cartas del mazo ya salieron
  Hand player;
  Hand dealer;
  int chips = START_CHIPS;
  int best = START_CHIPS;
  int bet = BET_STEP;
  int chipsAtRoundStart = START_CHIPS;
  int lastDelta = 0;  // fichas ganadas o perdidas en la mano que se acaba de jugar
  bool holeHidden = true;
  bool doubled = false;
  bool naturalWin = false;
  State state = State::BETTING;
  Outcome outcome = Outcome::NONE;
  unsigned long dealerStepAt = 0;

  // --- acciones (lo que recorre la palanca) ---
  std::array<Action, 3> actions{};
  int actionCount = 0;
  int cursor = 0;

  // --- pantalla ---
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;

  // Mazo y cartas
  void shuffle();
  uint8_t drawCardFromDeck();
  static int rankOf(const uint8_t card) { return card % 13; }
  static int suitOf(const uint8_t card) { return card / 13; }
  static int cardValue(uint8_t card);
  static int handTotal(const Hand& hand);
  static bool isNatural(const Hand& hand);
  static const char* rankLabel(uint8_t card);

  // Partida
  void newGame();
  void startRound();
  void continueRound();
  void playerHit();
  void playerStand();
  void playerDouble();
  void dealerStep();
  void settle();
  void buildActions();
  void runAction(Action action);

  // Dibujo
  void fillDisc(int cx, int cy, int r, bool state) const;
  void drawSuit(int cx, int cy, int size, int suit) const;
  void drawCardFace(int x, int y, uint8_t card) const;
  void drawCardBack(int x, int y) const;
  void drawHand(const Hand& hand, int y, bool hideSecond) const;
  void drawBadge(int x, int y, const char* text, bool filled) const;
  void drawBetChip(int cx, int cy, int r) const;
  void drawActions(int top) const;
};
