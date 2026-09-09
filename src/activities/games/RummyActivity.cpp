#include "RummyActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "cardIcons.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/Selection.h"

namespace {

// Los mismos bitmaps que usa el blackjack: valores de DejaVu Sans Bold y palos
// macizos, que es lo único que se distingue sin color.
const freeink::Icon* const RANK_ICONS[13] = {
    &icon_rank_A_24, &icon_rank_2_24, &icon_rank_3_24, &icon_rank_4_24, &icon_rank_5_24,
    &icon_rank_6_24, &icon_rank_7_24, &icon_rank_8_24, &icon_rank_9_24, &icon_rank_T_24,
    &icon_rank_J_24, &icon_rank_Q_24, &icon_rank_K_24};

const freeink::Icon* const SUIT_SMALL[4] = {&icon_suit_spade_16, &icon_suit_heart_16, &icon_suit_diamond_16,
                                            &icon_suit_club_16};

// Los bitmaps se pintan pixel por pixel: así salen bien en cualquier orientación.
void blitIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) != 0) continue;
      renderer.drawPixel(x + col, y + row, true);
    }
  }
}

// ----------------------------------------------------------- combinaciones --

// Reparto de menor sobrante por programación dinámica sobre la máscara de
// cartas ya usadas. Los estados son a lo sumo 2^11 y la recursión no baja de
// cuatro niveles (cada combinación se lleva tres cartas o más), así que es
// exacto y sale instantáneo. El memo va estático: 2 KB en la pila del loop
// serían demasiados, y la UI es de un solo hilo.
struct MeldSolver {
  int values[RummyActivity::HAND_MAX] = {0};
  const uint16_t* melds = nullptr;
  uint8_t meldCount = 0;
  uint8_t handCount = 0;
  int8_t memo[1 << RummyActivity::HAND_MAX];

  int rawDeadwood(const uint16_t used) const {
    int total = 0;
    for (int i = 0; i < handCount; ++i)
      if ((used & (1u << i)) == 0) total += values[i];
    return total;
  }

  int solve(const uint16_t used) {
    if (memo[used] >= 0) return memo[used];
    int best = rawDeadwood(used);
    for (uint8_t m = 0; m < meldCount; ++m) {
      if ((melds[m] & used) != 0) continue;
      const int v = solve(static_cast<uint16_t>(used | melds[m]));
      if (v < best) best = v;
    }
    memo[used] = static_cast<int8_t>(best);
    return best;
  }
};

MeldSolver& solver() {
  static MeldSolver instance;
  return instance;
}

constexpr int ROW_H = 56;

}  // namespace

// =============================================================== reglas =====

int RummyActivity::cardValue(const uint8_t card) {
  const int rank = rankOf(card);
  if (rank == 0) return 1;    // el as vale 1 y va abajo en la escalera
  if (rank >= 9) return 10;   // 10, J, Q, K
  return rank + 1;
}

uint8_t RummyActivity::findMelds(const Hand& hand, std::array<uint16_t, MAX_MELDS>& out) {
  uint8_t count = 0;

  // Tríos y cuartetos: mismas posiciones por valor. De un cuarteto salen también
  // sus cuatro tríos, porque a veces conviene dejar una carta para una escalera.
  for (int rank = 0; rank < 13; ++rank) {
    int idx[4];
    int found = 0;
    for (int i = 0; i < hand.count && found < 4; ++i)
      if (rankOf(hand.cards[i]) == rank) idx[found++] = i;
    if (found < 3) continue;
    if (found == 3) {
      if (count < MAX_MELDS) out[count++] = static_cast<uint16_t>((1u << idx[0]) | (1u << idx[1]) | (1u << idx[2]));
      continue;
    }
    uint16_t all = 0;
    for (int i = 0; i < 4; ++i) all |= static_cast<uint16_t>(1u << idx[i]);
    if (count < MAX_MELDS) out[count++] = all;
    for (int skip = 0; skip < 4; ++skip) {
      const uint16_t mask = static_cast<uint16_t>(all & ~(1u << idx[skip]));
      if (count < MAX_MELDS) out[count++] = mask;
    }
  }

  // Escaleras: del mismo palo y seguidas. Se guardan TODAS las ventanas de tres
  // o más, no solo la más larga: partir una escalera larga a veces libera una
  // carta para un trío.
  for (int suit = 0; suit < 4; ++suit) {
    int pos[13];
    for (int r = 0; r < 13; ++r) pos[r] = -1;
    for (int i = 0; i < hand.count; ++i)
      if (suitOf(hand.cards[i]) == suit) pos[rankOf(hand.cards[i])] = i;
    for (int start = 0; start < 11; ++start) {
      if (pos[start] < 0) continue;
      uint16_t mask = 0;
      int len = 0;
      for (int r = start; r < 13 && pos[r] >= 0; ++r) {
        mask |= static_cast<uint16_t>(1u << pos[r]);
        ++len;
        if (len >= 3 && count < MAX_MELDS) out[count++] = mask;
      }
    }
  }
  return count;
}

void RummyActivity::bestLayout(const Hand& hand, Layout& out) {
  out = Layout{};
  if (hand.count == 0) return;

  std::array<uint16_t, MAX_MELDS> melds{};
  const uint8_t meldCount = findMelds(hand, melds);

  MeldSolver& s = solver();
  s.melds = melds.data();
  s.meldCount = meldCount;
  s.handCount = hand.count;
  for (int i = 0; i < hand.count; ++i) s.values[i] = cardValue(hand.cards[i]);
  memset(s.memo, -1, static_cast<size_t>(1) << hand.count);

  out.deadwood = s.solve(0);

  // Reconstrucción: mientras exista una combinación disjunta, el óptimo baja sí
  // o sí (toda combinación se lleva por lo menos tres puntos), así que basta con
  // comparar contra el sobrante crudo para saber cuándo parar.
  uint16_t used = 0;
  while (out.meldCount < out.melds.size()) {
    if (s.memo[used] == s.rawDeadwood(used)) break;
    bool advanced = false;
    for (uint8_t m = 0; m < meldCount; ++m) {
      if ((melds[m] & used) != 0) continue;
      if (s.solve(static_cast<uint16_t>(used | melds[m])) != s.memo[used]) continue;
      out.melds[out.meldCount++] = melds[m];
      used = static_cast<uint16_t>(used | melds[m]);
      advanced = true;
      break;
    }
    if (!advanced) break;
  }
  out.used = used;
}

int RummyActivity::bestDiscard(const Hand& hand, const int forbidden, int& outIndex) {
  outIndex = -1;
  int best = 1000;
  int bestCardValue = -1;
  for (int i = 0; i < hand.count; ++i) {
    if (i == forbidden) continue;
    Hand test = hand;
    test.removeAt(i);
    Layout layout;
    bestLayout(test, layout);
    const int value = cardValue(hand.cards[i]);
    // A igual sobrante se tira la carta más gorda: es la que más cuesta si el
    // otro corta primero.
    if (layout.deadwood < best || (layout.deadwood == best && value > bestCardValue)) {
      best = layout.deadwood;
      bestCardValue = value;
      outIndex = i;
    }
  }
  if (outIndex < 0) {
    outIndex = 0;
    return 1000;
  }
  return best;
}

int RummyActivity::layOff(const Hand& knocker, const Layout& knockerLayout, const Hand& defender,
                          const Layout& defenderLayout) {
  // Las combinaciones del que cortó, en una forma que se pueda estirar.
  struct Meld {
    bool isSet = false;
    int rank = 0;   // valor del trío
    int size = 0;   // cuántas cartas tiene (un trío admite una cuarta)
    int suit = 0;   // palo de la escalera
    int lo = 0, hi = 0;
  };
  Meld melds[4];
  int meldCount = 0;
  for (uint8_t m = 0; m < knockerLayout.meldCount && meldCount < 4; ++m) {
    const uint16_t mask = knockerLayout.melds[m];
    int firstRank = -1, firstSuit = -1, lo = 99, hi = -1, size = 0;
    bool sameRank = true;
    for (int i = 0; i < knocker.count; ++i) {
      if ((mask & (1u << i)) == 0) continue;
      const int rank = rankOf(knocker.cards[i]);
      const int suit = suitOf(knocker.cards[i]);
      if (firstRank < 0) {
        firstRank = rank;
        firstSuit = suit;
      } else if (rank != firstRank) {
        sameRank = false;
      }
      if (rank < lo) lo = rank;
      if (rank > hi) hi = rank;
      ++size;
    }
    if (size == 0) continue;
    Meld& out = melds[meldCount++];
    out.isSet = sameRank;
    out.rank = firstRank;
    out.size = size;
    out.suit = firstSuit;
    out.lo = lo;
    out.hi = hi;
  }

  uint8_t loose[HAND_MAX];
  int looseCount = 0;
  for (int i = 0; i < defender.count; ++i)
    if ((defenderLayout.used & (1u << i)) == 0) loose[looseCount++] = defender.cards[i];

  // Se descarga de a una carta hasta que ya no entre ninguna: estirar una
  // escalera puede abrir lugar para la siguiente.
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < looseCount && !changed; ++i) {
      const int rank = rankOf(loose[i]);
      const int suit = suitOf(loose[i]);
      for (int m = 0; m < meldCount; ++m) {
        Meld& meld = melds[m];
        if (meld.isSet) {
          if (meld.size >= 4 || rank != meld.rank) continue;
          ++meld.size;
        } else {
          if (suit != meld.suit || (rank != meld.lo - 1 && rank != meld.hi + 1)) continue;
          if (rank == meld.lo - 1) --meld.lo;
          else ++meld.hi;
        }
        for (int j = i; j + 1 < looseCount; ++j) loose[j] = loose[j + 1];
        --looseCount;
        changed = true;
        break;
      }
    }
  }

  int total = 0;
  for (int i = 0; i < looseCount; ++i) total += cardValue(loose[i]);
  return total;
}

// =============================================================== partida ====

void RummyActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = MODE_SELECT;
  modeCursor = 0;
  forceClean = true;
  requestUpdate();
}

void RummyActivity::newMatch(const Mode m) {
  mode = m;
  playerScore = 0;
  machineScore = 0;
  handNumber = 1;
  dealHand();
}

void RummyActivity::dealHand() {
  for (int i = 0; i < 52; ++i) deck[i] = static_cast<uint8_t>(i);
  for (int i = 51; i > 0; --i) {  // Fisher-Yates de verdad
    const int j = static_cast<int>(random(i + 1));
    const uint8_t tmp = deck[i];
    deck[i] = deck[j];
    deck[j] = tmp;
  }
  player.clear();
  machine.clear();
  for (int i = 0; i < 10; ++i) player.add(deck[i]);
  for (int i = 0; i < 10; ++i) machine.add(deck[10 + i]);
  discardCount = 0;
  discardPile[discardCount++] = deck[20];  // la carta a la vista
  stockTop = 21;

  handEnd = END_NONE;
  handPoints = 0;
  ginHand = false;
  undercut = false;
  playerWonHand = false;
  pendingDiscard = -1;
  forbiddenDiscard = -1;
  drawCursor = 0;
  knockCursor = 0;
  handCursor = 0;
  machineStep = 0;
  machineDidSomething = false;
  bestLayout(machine, machineLayout);
  rebuildPlayerView();
  state = PLAYER_DRAW;
  forceClean = true;
  requestUpdate();
}

void RummyActivity::nextHand() {
  ++handNumber;
  dealHand();
}

uint8_t RummyActivity::drawFromStock() {
  if (stockTop >= 52) return deck[51];
  return deck[stockTop++];
}

uint8_t RummyActivity::buildOrder(const Hand& hand, const Layout& layout, uint8_t* order, uint8_t* group) {
  uint8_t count = 0;
  for (uint8_t m = 0; m < layout.meldCount; ++m) {
    // Dentro de una combinación las cartas van ordenadas por valor: una escalera
    // desordenada no se lee.
    int idx[HAND_MAX];
    int found = 0;
    for (int i = 0; i < hand.count; ++i)
      if ((layout.melds[m] & (1u << i)) != 0) idx[found++] = i;
    for (int a = 1; a < found; ++a) {
      const int v = idx[a];
      int b = a - 1;
      while (b >= 0 && rankOf(hand.cards[idx[b]]) > rankOf(hand.cards[v])) {
        idx[b + 1] = idx[b];
        --b;
      }
      idx[b + 1] = v;
    }
    for (int a = 0; a < found; ++a) {
      order[count] = static_cast<uint8_t>(idx[a]);
      group[count] = m;
      ++count;
    }
  }
  // Las sueltas, por palo y valor.
  int loose[HAND_MAX];
  int looseCount = 0;
  for (int i = 0; i < hand.count; ++i)
    if ((layout.used & (1u << i)) == 0) loose[looseCount++] = i;
  for (int a = 1; a < looseCount; ++a) {
    const int v = loose[a];
    const int key = suitOf(hand.cards[v]) * 13 + rankOf(hand.cards[v]);
    int b = a - 1;
    while (b >= 0 && suitOf(hand.cards[loose[b]]) * 13 + rankOf(hand.cards[loose[b]]) > key) {
      loose[b + 1] = loose[b];
      --b;
    }
    loose[b + 1] = v;
  }
  for (int a = 0; a < looseCount; ++a) {
    order[count] = static_cast<uint8_t>(loose[a]);
    group[count] = static_cast<uint8_t>(LOOSE_GROUP);
    ++count;
  }
  return count;
}

void RummyActivity::rebuildPlayerView() {
  bestLayout(player, playerLayout);
  displayCount = buildOrder(player, playerLayout, displayOrder.data(), displayGroup.data());
  if (handCursor >= displayCount) handCursor = 0;
}

void RummyActivity::playerTakes(const bool fromDiscard) {
  if (fromDiscard) {
    if (discardCount == 0) return;
    const uint8_t card = discardPile[--discardCount];
    player.add(card);
    forbiddenDiscard = player.count - 1;  // esa carta no se puede tirar en el mismo turno
  } else {
    player.add(drawFromStock());
    forbiddenDiscard = -1;
  }
  rebuildPlayerView();
  // El cursor arranca sobre la primera carta suelta que se pueda tirar.
  handCursor = 0;
  for (int i = 0; i < displayCount; ++i) {
    if (displayGroup[i] == LOOSE_GROUP && displayOrder[i] != forbiddenDiscard) {
      handCursor = i;
      break;
    }
  }
  // Si no había ninguna suelta y la posición 0 cayó justo en la carta prohibida,
  // se corre a la primera que sí se pueda tirar: nunca arranca sobre algo vedado.
  if (displayCount > 0 && displayOrder[handCursor] == forbiddenDiscard) {
    for (int i = 0; i < displayCount; ++i) {
      if (displayOrder[i] != forbiddenDiscard) {
        handCursor = i;
        break;
      }
    }
  }
  state = PLAYER_DISCARD;
  forceClean = true;
  requestUpdate();
}

void RummyActivity::playerDiscards(const int handIndex, const bool knock) {
  if (handIndex < 0 || handIndex >= player.count) return;
  const uint8_t card = player.cards[handIndex];
  player.removeAt(handIndex);
  if (discardCount < discardPile.size()) discardPile[discardCount++] = card;
  forbiddenDiscard = -1;
  pendingDiscard = -1;
  rebuildPlayerView();

  if (knock) {
    resolveKnock(true, playerLayout.deadwood == 0);
    return;
  }
  startMachineTurn();
}

void RummyActivity::startMachineTurn() {
  if (stockLeft() <= STOCK_FLOOR) {
    endHandVoid();
    return;
  }
  state = MACHINE_TURN;
  machineStep = 0;
  machineDidSomething = false;
  machineStepAt = millis() + MACHINE_STEP_MS;
  forceClean = true;
  requestUpdate();
}

void RummyActivity::stepMachine() {
  if (millis() < machineStepAt) return;

  if (machineStep == 0) {
    // ¿Sirve la carta del descarte? Se mide con el sobrante que quedaría después
    // de tomarla y tirar la que menos duela.
    Layout current;
    bestLayout(machine, current);
    bool take = false;
    if (discardCount > 0) {
      Hand test = machine;
      test.add(discardPile[discardCount - 1]);
      int idx = 0;
      const int after = bestDiscard(test, test.count - 1, idx);
      take = after < current.deadwood;
    }
    if (take) {
      machine.add(discardPile[--discardCount]);
      machineTookDiscard = true;
    } else {
      machine.add(drawFromStock());
      machineTookDiscard = false;
    }
    machineDidSomething = true;
    machineStep = 1;
    machineStepAt = millis() + MACHINE_STEP_MS;
    requestUpdate();
    return;
  }

  if (machineStep == 1) {
    int idx = 0;
    const int forbidden = machineTookDiscard ? machine.count - 1 : -1;
    const int deadwood = bestDiscard(machine, forbidden, idx);
    machineDiscarded = machine.cards[idx];
    machine.removeAt(idx);
    if (discardCount < discardPile.size()) discardPile[discardCount++] = machineDiscarded;
    bestLayout(machine, machineLayout);
    machineStep = 2;
    machineStepAt = millis() + MACHINE_STEP_MS;
    if (deadwood <= KNOCK_LIMIT) {
      resolveKnock(false, deadwood == 0);
      return;
    }
    requestUpdate();
    return;
  }

  if (stockLeft() <= STOCK_FLOOR) {
    endHandVoid();
    return;
  }
  state = PLAYER_DRAW;
  drawCursor = 0;
  forceClean = true;
  requestUpdate();
}

void RummyActivity::resolveKnock(const bool byPlayer, const bool gin) {
  bestLayout(player, playerLayout);
  bestLayout(machine, machineLayout);

  const Hand& knocker = byPlayer ? player : machine;
  const Layout& knockerLayout = byPlayer ? playerLayout : machineLayout;
  const Hand& defender = byPlayer ? machine : player;
  const Layout& defenderLayout = byPlayer ? machineLayout : playerLayout;

  const int knockerDead = knockerLayout.deadwood;
  // Con gin el otro no puede descargar nada sobre las combinaciones del que cortó.
  const int defenderDead = gin ? defenderLayout.deadwood : layOff(knocker, knockerLayout, defender, defenderLayout);

  ginHand = gin;
  undercut = false;
  if (gin) {
    handPoints = defenderDead + GIN_BONUS;
    playerWonHand = byPlayer;
  } else if (defenderDead > knockerDead) {
    handPoints = defenderDead - knockerDead;
    playerWonHand = byPlayer;
  } else {
    undercut = true;  // contracorte: se lo lleva el que NO cortó
    handPoints = knockerDead - defenderDead + UNDERCUT_BONUS;
    playerWonHand = !byPlayer;
  }

  playerDeadwood = byPlayer ? knockerDead : defenderDead;
  machineDeadwood = byPlayer ? defenderDead : knockerDead;
  if (playerWonHand) {
    playerScore += handPoints;
  } else {
    machineScore += handPoints;
  }
  handEnd = byPlayer ? END_PLAYER_KNOCK : END_MACHINE_KNOCK;
  finishHand();
}

void RummyActivity::endHandVoid() {
  bestLayout(player, playerLayout);
  bestLayout(machine, machineLayout);
  playerDeadwood = playerLayout.deadwood;
  machineDeadwood = machineLayout.deadwood;
  handEnd = END_VOID;
  handPoints = 0;
  ginHand = false;
  undercut = false;
  playerWonHand = false;
  finishHand();
}

void RummyActivity::finishHand() {
  rebuildPlayerView();
  const bool matchDone = mode == SINGLE_HAND || playerScore >= TARGET_SCORE || machineScore >= TARGET_SCORE;
  state = matchDone ? MATCH_OVER : HAND_OVER;
  forceClean = true;
  requestUpdate();
}

// ================================================================== loop ====

int RummyActivity::selectableCount() const {
  switch (state) {
    case MODE_SELECT: return 2;
    case PLAYER_DRAW: return discardCount > 0 ? 2 : 1;
    case PLAYER_DISCARD: return displayCount;
    case KNOCK_ASK: return 2;
    default: return 0;
  }
}

void RummyActivity::moveCursor(const int dir) {
  const int total = selectableCount();
  if (total <= 1) return;
  switch (state) {
    case MODE_SELECT:
      modeCursor = dir > 0 ? ButtonNavigator::nextIndex(modeCursor, total) : ButtonNavigator::previousIndex(modeCursor, total);
      break;
    case PLAYER_DRAW:
      drawCursor = dir > 0 ? ButtonNavigator::nextIndex(drawCursor, total) : ButtonNavigator::previousIndex(drawCursor, total);
      break;
    case PLAYER_DISCARD: {
      // La carta que se acaba de tomar del descarte no se puede tirar: el cursor
      // la saltea, así no hay forma de elegir algo prohibido.
      int next = handCursor;
      for (int guard = 0; guard < total; ++guard) {
        next = dir > 0 ? ButtonNavigator::nextIndex(next, total) : ButtonNavigator::previousIndex(next, total);
        if (displayOrder[next] != forbiddenDiscard) break;
      }
      handCursor = next;
      break;
    }
    case KNOCK_ASK:
      knockCursor = dir > 0 ? ButtonNavigator::nextIndex(knockCursor, 2) : ButtonNavigator::previousIndex(knockCursor, 2);
      break;
    default:
      return;
  }
  requestUpdate();
}

void RummyActivity::confirm() {
  switch (state) {
    case MODE_SELECT:
      newMatch(modeCursor == 0 ? MATCH_TO_100 : SINGLE_HAND);
      break;
    case PLAYER_DRAW:
      if (stockLeft() <= STOCK_FLOOR && drawCursor == 0) {
        endHandVoid();
        return;
      }
      playerTakes(drawCursor == 1 && discardCount > 0);
      break;
    case PLAYER_DISCARD: {
      if (displayCount == 0) return;
      const int index = displayOrder[handCursor];
      if (index == forbiddenDiscard) return;
      // ¿Queda para cortar? Se mira el sobrante SIN esa carta.
      Hand test = player;
      test.removeAt(index);
      Layout after;
      bestLayout(test, after);
      if (after.deadwood <= KNOCK_LIMIT) {
        pendingDiscard = index;
        knockCursor = 0;
        state = KNOCK_ASK;
        forceClean = true;
        requestUpdate();
        return;
      }
      playerDiscards(index, false);
      break;
    }
    case KNOCK_ASK:
      playerDiscards(pendingDiscard, knockCursor == 0);
      break;
    case HAND_OVER:
      nextHand();
      break;
    case MATCH_OVER:
      state = MODE_SELECT;
      forceClean = true;
      requestUpdate();
      break;
    default:
      break;
  }
}

void RummyActivity::loop() {
  // Atrás mantenido primero: se come la suelta, así no sale del juego.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    state = MODE_SELECT;
    forceClean = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == KNOCK_ASK) {
      state = PLAYER_DISCARD;
      pendingDiscard = -1;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (state == MACHINE_TURN) {
    stepMachine();
    return;
  }

  buttonNavigator.onNext([this] { moveCursor(1); });
  buttonNavigator.onPrevious([this] { moveCursor(-1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) confirm();
}

// ================================================================ dibujo ====

void RummyActivity::drawSuit(const int cx, const int cy, const int suit) const {
  const int index = suit >= 0 && suit < 4 ? suit : 0;
  const freeink::Icon& icon = *SUIT_SMALL[index];
  blitIcon(renderer, icon, cx - icon.w / 2, cy - icon.h / 2);
}

void RummyActivity::drawRank(const int x, const int y, const uint8_t card) const {
  blitIcon(renderer, *RANK_ICONS[rankOf(card)], x, y);
}

// Carta chica: en 480 px no entran diez cartas del tamaño del blackjack, así que
// van en filas de a seis, con el valor arriba a la izquierda y el palo abajo a la
// derecha. La elegida se levanta y lleva marco grueso.
void RummyActivity::drawMiniCard(const int x, const int y, const uint8_t card, const bool selected) const {
  renderer.fillRoundedRect(x + 2, y + 2, CARD_W, CARD_H, 7, Color::Black);  // sombra
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 7, Color::White);
  renderer.drawRoundedRect(x, y, CARD_W, CARD_H, selected ? 3 : 2, 7, true);
  drawRank(x + 5, y + 6, card);
  drawSuit(x + CARD_W - 15, y + CARD_H - 15, suitOf(card));
  if (selected) {
    // Punta maciza arriba: se ve incluso de reojo cuál está elegida.
    const int cx = x + CARD_W / 2;
    const int xs[3] = {cx - 8, cx + 8, cx};
    const int ys[3] = {y - 14, y - 14, y - 4};
    renderer.fillPolygon(xs, ys, 3, true);
  }
}

void RummyActivity::drawCardBack(const int x, const int y, const int w, const int h) const {
  renderer.fillRoundedRect(x, y, w, h, 6, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 2, 6, true);
  for (int d = -h; d < w; d += 5) {  // trama en diagonal, recortada a mano
    const int t0 = d < 0 ? -d : 0;
    const int t1 = w - d < h ? w - d : h;
    if (t1 > t0) renderer.drawLine(x + d + t0 + 2, y + t0 + 2, x + d + t1 - 3, y + t1 - 3, true);
  }
}

void RummyActivity::drawMachineRow(const int top) const {
  const int pageWidth = renderer.getScreenWidth();
  char line[64];
  snprintf(line, sizeof(line), "%s  (%d)", tr(STR_GAME_MACHINE), machine.count);
  renderer.drawText(UI_10_FONT_ID, SIDE, top, line, true, EpdFontFamily::BOLD);
  snprintf(line, sizeof(line), "%s %d", tr(STR_GAME_SCORE), machineScore);
  const int width = renderer.getTextWidth(UI_10_FONT_ID, line);
  renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - width, top, line);

  const int stride = 20;
  const int totalWidth = BACK_W + (machine.count > 0 ? (machine.count - 1) * stride : 0);
  int x = (pageWidth - totalWidth) / 2;
  for (int i = 0; i < machine.count; ++i) {
    drawCardBack(x, top + 22, BACK_W, BACK_H);
    x += stride;
  }
}

void RummyActivity::drawCenterRow(const int top) const {
  const int pageWidth = renderer.getScreenWidth();
  const int gap = 40;
  const int stockX = pageWidth / 2 - CARD_W - gap / 2;
  const int discardX = pageWidth / 2 + gap / 2;
  const bool choosing = state == PLAYER_DRAW;

  drawCardBack(stockX, top, CARD_W, CARD_H);
  if (discardCount > 0) {
    drawMiniCard(discardX, top, discardPile[discardCount - 1], choosing && drawCursor == 1);
  } else {
    renderer.drawRoundedRect(discardX, top, CARD_W, CARD_H, 2, 7, true);
  }
  if (choosing && drawCursor == 0) {
    renderer.drawRoundedRect(stockX - 4, top - 4, CARD_W + 8, CARD_H + 8, 3, 9, true);
    const int cx = stockX + CARD_W / 2;
    const int xs[3] = {cx - 8, cx + 8, cx};
    const int ys[3] = {top - 18, top - 18, top - 8};
    renderer.fillPolygon(xs, ys, 3, true);
  }

  char line[48];
  snprintf(line, sizeof(line), "%s (%d)", tr(STR_GAME_STOCK), stockLeft());
  int width = renderer.getTextWidth(SMALL_FONT_ID, line);
  renderer.drawText(SMALL_FONT_ID, stockX + (CARD_W - width) / 2, top + CARD_H + 6, line);
  const char* label = tr(STR_GAME_DISCARD_PILE);
  width = renderer.getTextWidth(SMALL_FONT_ID, label);
  renderer.drawText(SMALL_FONT_ID, discardX + (CARD_W - width) / 2, top + CARD_H + 6, label);
}

int RummyActivity::drawHandRows(const Hand& hand, const uint8_t* order, const uint8_t* group, const uint8_t count,
                                const int top, const int cursor) const {
  if (count == 0) return 0;
  const int pageWidth = renderer.getScreenWidth();

  // Reparto en filas: una combinación no se parte si entra entera.
  int px[HAND_MAX] = {0};
  int py[HAND_MAX] = {0};
  int rows = 0;
  int index = 0;
  while (index < count) {
    int length = 0;
    while (index + length < count) {
      const uint8_t g = group[index + length];
      int groupLength = 1;
      if (g != LOOSE_GROUP) {
        while (index + length + groupLength < count && group[index + length + groupLength] == g) ++groupLength;
      }
      if (length > 0 && length + groupLength > MAX_PER_ROW) break;
      length += groupLength;
      if (length >= MAX_PER_ROW) break;
    }
    if (length == 0) length = 1;
    const int rowWidth = length * CARD_W + (length - 1) * CARD_GAP;
    int x = (pageWidth - rowWidth) / 2;
    const int y = top + rows * ROW_PITCH;
    for (int i = 0; i < length; ++i) {
      px[index + i] = x;
      py[index + i] = y;
      x += CARD_W + CARD_GAP;
    }
    index += length;
    ++rows;
  }

  // Primero los marcos de las combinaciones, después las cartas: así la carta
  // elegida, que se levanta, pisa el marco en vez de quedar debajo.
  int i = 0;
  while (i < count) {
    const uint8_t g = group[i];
    int j = i;
    while (j + 1 < count && group[j + 1] == g && py[j + 1] == py[i]) ++j;
    if (g != LOOSE_GROUP) {
      const int x0 = px[i] - 5, y0 = py[i] - 5;
      const int w = px[j] + CARD_W + 5 - x0, h = CARD_H + 10;
      renderer.drawRoundedRect(x0, y0, w, h, 2, 9, true);
    }
    i = j + 1;
  }
  for (int k = 0; k < count; ++k) {
    const bool selected = cursor >= 0 && k == cursor;
    drawMiniCard(px[k], selected ? py[k] - 8 : py[k], hand.cards[order[k]], selected);
  }
  return rows * ROW_PITCH;
}

int RummyActivity::drawPlayerHand(const int top) const {
  const int cursor = state == PLAYER_DISCARD || state == KNOCK_ASK ? handCursor : -1;
  return drawHandRows(player, displayOrder.data(), displayGroup.data(), displayCount, top, cursor);
}

void RummyActivity::drawOptionRow(const int x, const int y, const int w, const char* label,
                                  const bool selected) const {
  if (selected) {
    drawSelectionRow(renderer, x, y, w, ROW_H - 10, 12);
  } else {
    renderer.drawRoundedRect(x, y, w, ROW_H - 10, 2, 12, true);
  }
  const std::string shown = renderer.truncatedText(UI_12_FONT_ID, label, w - 24, EpdFontFamily::BOLD);
  const int width = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - width) / 2, y + 11, shown.c_str(), SELECTION_INK, EpdFontFamily::BOLD);
}

void RummyActivity::drawModeSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + 40;
  renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_GAME_MODE), true, EpdFontFamily::BOLD);

  const int width = pageWidth - 2 * SIDE;
  drawOptionRow(SIDE, top + 48, width, tr(STR_GAME_RUMMY_MATCH), modeCursor == 0);
  drawOptionRow(SIDE, top + 48 + ROW_H, width, tr(STR_GAME_RUMMY_SINGLE), modeCursor == 1);

  const auto lines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_GAME_RUMMY_RULES), width, 6);
  for (size_t i = 0; i < lines.size(); ++i) {
    renderer.drawText(SMALL_FONT_ID, SIDE, top + 48 + 2 * ROW_H + 24 + static_cast<int>(i) * 22, lines[i].c_str());
  }
}

void RummyActivity::drawHandOver(const int top) const {
  char line[96];

  const char* title = tr(STR_GAME_DRAW);
  if (handEnd == END_VOID) {
    title = tr(STR_GAME_HAND_VOID);
  } else {
    title = playerWonHand ? tr(STR_GAME_WON) : tr(STR_GAME_LOST);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, top, title, true, EpdFontFamily::BOLD);

  if (handEnd != END_VOID) {
    const char* reason = ginHand ? tr(STR_GAME_GIN) : undercut ? tr(STR_GAME_UNDERCUT) : tr(STR_GAME_KNOCK);
    snprintf(line, sizeof(line), "%s  +%d", reason, handPoints);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 30, line);
  }

  // La mano de la máquina, a la vista con sus combinaciones: es lo único que el
  // jugador no vio durante la mano.
  snprintf(line, sizeof(line), "%s — %s %d", tr(STR_GAME_MACHINE), tr(STR_GAME_DEADWOOD), machineDeadwood);
  renderer.drawText(UI_10_FONT_ID, SIDE, top + 58, line, true, EpdFontFamily::BOLD);

  uint8_t order[HAND_MAX];
  uint8_t group[HAND_MAX];
  const uint8_t count = buildOrder(machine, machineLayout, order, group);
  const int used = drawHandRows(machine, order, group, count, top + 88, -1);

  snprintf(line, sizeof(line), "%s — %s %d", tr(STR_GAME_YOU), tr(STR_GAME_DEADWOOD), playerDeadwood);
  renderer.drawText(UI_10_FONT_ID, SIDE, top + 96 + used, line, true, EpdFontFamily::BOLD);

  snprintf(line, sizeof(line), "%s  %d - %d", tr(STR_GAME_SCORE), playerScore, machineScore);
  renderer.drawCenteredText(UI_12_FONT_ID, top + 124 + used, line, true, EpdFontFamily::BOLD);
}

void RummyActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_RUMMY));

  const char* backLabel = state == KNOCK_ASK ? tr(STR_CANCEL) : tr(STR_GAME_QUIT);
  const char* confirmLabel = tr(STR_SELECT);

  if (state == MODE_SELECT) {
    drawModeSelect();
    const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    partialCount = 0;
    forceClean = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  if (state == HAND_OVER || state == MATCH_OVER) {
    drawHandOver(headerBottom + 20);
    if (state == MATCH_OVER) {
      char line[96];
      snprintf(line, sizeof(line), "%s", playerScore > machineScore ? tr(STR_GAME_WON) : tr(STR_GAME_LOST));
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight - metrics.buttonHintsHeight - 60, line, true,
                                EpdFontFamily::BOLD);
    }
    const auto labels = mappedInput.mapLabels(
        tr(STR_GAME_QUIT), state == MATCH_OVER ? tr(STR_GAME_NEW) : tr(STR_GAME_NEXT_HAND), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    partialCount = 0;
    forceClean = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // --- mesa ---
  const int machineTop = headerBottom + 8;
  drawMachineRow(machineTop);
  const int centerTop = machineTop + 22 + BACK_H + 16;
  drawCenterRow(centerTop);

  // Qué está pasando y qué hay que hacer.
  const int messageTop = centerTop + CARD_H + 28;
  char line[96];
  const char* prompt = "";
  switch (state) {
    case PLAYER_DRAW:
      prompt = drawCursor == 0 ? tr(STR_GAME_DRAW_STOCK) : tr(STR_GAME_TAKE_DISCARD);
      break;
    case PLAYER_DISCARD:
      prompt = tr(STR_GAME_PICK_DISCARD);
      break;
    case KNOCK_ASK:
      prompt = tr(STR_GAME_KNOCK_ASK);
      break;
    case MACHINE_TURN:
      prompt = !machineDidSomething ? tr(STR_GAME_THINKING)
               : machineTookDiscard ? tr(STR_GAME_TOOK_DISCARD)
                                    : tr(STR_GAME_DREW_STOCK);
      break;
    default:
      break;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, messageTop, prompt, true, EpdFontFamily::BOLD);

  // Tus cartas y tu sobrante: la cuenta tiene que estar siempre a la vista.
  snprintf(line, sizeof(line), "%s %d   %s %d", tr(STR_GAME_DEADWOOD), playerLayout.deadwood, tr(STR_GAME_SCORE),
           playerScore);
  renderer.drawText(UI_10_FONT_ID, SIDE, messageTop + 28, line, true, EpdFontFamily::BOLD);

  const int handTop = messageTop + 72;
  const int used = drawPlayerHand(handTop);

  if (state == KNOCK_ASK) {
    const int width = (pageWidth - 2 * SIDE) / 2 - 6;
    const int y = handTop + used + 10;
    drawOptionRow(SIDE, y, width, tr(STR_YES), knockCursor == 0);
    drawOptionRow(SIDE + width + 12, y, width, tr(STR_NO), knockCursor == 1);
  }

  const bool navigable = selectableCount() > 1;
  const auto labels = mappedInput.mapLabels(backLabel, state == MACHINE_TURN ? "" : confirmLabel,
                                            navigable ? tr(STR_DIR_UP) : "", navigable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
