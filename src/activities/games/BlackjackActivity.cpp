#include "BlackjackActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "cardIcons.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/Selection.h"

namespace {
// Valores de la carta, en el orden del mazo (el 0 es el as, el 12 la K). Son
// bitmaps y no texto: hay que poder darlos vuelta en la esquina de abajo.
const freeink::Icon* const RANK_ICONS[13] = {
    &icon_rank_A_24, &icon_rank_2_24, &icon_rank_3_24, &icon_rank_4_24, &icon_rank_5_24,
    &icon_rank_6_24, &icon_rank_7_24, &icon_rank_8_24, &icon_rank_9_24, &icon_rank_T_24,
    &icon_rank_J_24, &icon_rank_Q_24, &icon_rank_K_24};

// Palo (0 pica, 1 corazón, 2 diamante, 3 trébol) en los dos tamaños.
const freeink::Icon* const SUIT_BIG[4] = {&icon_suit_spade_40, &icon_suit_heart_40, &icon_suit_diamond_40,
                                          &icon_suit_club_40};
const freeink::Icon* const SUIT_SMALL[4] = {&icon_suit_spade_16, &icon_suit_heart_16, &icon_suit_diamond_16,
                                            &icon_suit_club_16};

// Los bitmaps se pintan pixel por pixel por el renderer, así salen bien en
// cualquier orientación. `rotated` los da vuelta 180 grados (esquina de abajo).
void blitIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const bool rotated,
              const bool ink = true) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) != 0) continue;
      if (rotated) {
        renderer.drawPixel(x + icon.w - 1 - col, y + icon.h - 1 - row, ink);
      } else {
        renderer.drawPixel(x + col, y + row, ink);
      }
    }
  }
}
}  // namespace

// ---------------------------------------------------------------- mazo ------

// Fisher-Yates de verdad: de atrás para adelante y con j uniforme en [0, i].
void BlackjackActivity::shuffle() {
  for (int i = 0; i < DECK_SIZE; ++i) deck[i] = static_cast<uint8_t>(i);
  for (int i = DECK_SIZE - 1; i > 0; --i) {
    const int j = static_cast<int>(random(i + 1));
    const uint8_t tmp = deck[i];
    deck[i] = deck[j];
    deck[j] = tmp;
  }
  dealt = 0;
}

uint8_t BlackjackActivity::drawCardFromDeck() {
  if (dealt >= DECK_SIZE) shuffle();
  return deck[dealt++];
}

int BlackjackActivity::cardValue(const uint8_t card) {
  const int rank = rankOf(card);
  if (rank == 0) return 11;    // el as entra valiendo 11 y handTotal lo baja si hace falta
  if (rank >= 9) return 10;    // 10, J, Q, K
  return rank + 1;
}

int BlackjackActivity::handTotal(const Hand& hand) {
  int total = 0;
  int aces = 0;
  for (int i = 0; i < hand.count; ++i) {
    const int value = cardValue(hand.cards[i]);
    if (value == 11) ++aces;
    total += value;
  }
  while (total > BLACKJACK && aces > 0) {
    total -= 10;
    --aces;
  }
  return total;
}

bool BlackjackActivity::isNatural(const Hand& hand) { return hand.count == 2 && handTotal(hand) == BLACKJACK; }

// -------------------------------------------------------------- partida ----

void BlackjackActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  newGame();
}

void BlackjackActivity::newGame() {
  chips = START_CHIPS;
  best = START_CHIPS;
  bet = BET_STEP;
  chipsAtRoundStart = START_CHIPS;
  lastDelta = 0;
  doubled = false;
  naturalWin = false;
  holeHidden = true;
  outcome = Outcome::NONE;
  player.clear();
  dealer.clear();
  shuffle();
  state = State::BETTING;
  actionCount = 0;
  cursor = 0;
  buildActions();
  forceClean = true;
  requestUpdate();
}

void BlackjackActivity::startRound() {
  if (chips < BET_STEP) return;
  if (bet > chips) bet = (chips / BET_STEP) * BET_STEP;
  if (bet < BET_STEP) bet = BET_STEP;
  // Rebarajar antes de repartir si queda poco mazo (nunca en medio de una mano).
  if (dealt > DECK_SIZE - RESHUFFLE_AT) shuffle();

  chipsAtRoundStart = chips;
  chips -= bet;  // la apuesta se va a la mesa
  doubled = false;
  naturalWin = false;
  outcome = Outcome::NONE;
  holeHidden = true;
  player.clear();
  dealer.clear();
  player.add(drawCardFromDeck());
  dealer.add(drawCardFromDeck());
  player.add(drawCardFromDeck());
  dealer.add(drawCardFromDeck());

  // Blackjack natural de cualquiera de los dos: se resuelve de una.
  if (isNatural(player) || isNatural(dealer)) {
    holeHidden = false;
    settle();
    return;
  }
  state = State::PLAYER;
  buildActions();
  forceClean = true;
  requestUpdate();
}

void BlackjackActivity::continueRound() {
  if (doubled) bet /= 2;  // la mano doblada no deja la apuesta al doble para siempre
  doubled = false;
  if (bet > chips) bet = (chips / BET_STEP) * BET_STEP;
  if (bet < BET_STEP) bet = BET_STEP;
  player.clear();
  dealer.clear();
  outcome = Outcome::NONE;
  naturalWin = false;
  holeHidden = true;
  state = State::BETTING;
  buildActions();
  forceClean = true;
  requestUpdate();
}

void BlackjackActivity::playerHit() {
  player.add(drawCardFromDeck());
  const int total = handTotal(player);
  if (total > BLACKJACK) {  // se pasó: no hace falta que juegue la banca
    holeHidden = false;
    settle();
    return;
  }
  if (total == BLACKJACK || player.count >= MAX_CARDS) {
    playerStand();
    return;
  }
  buildActions();  // con tres cartas ya no se puede doblar
  requestUpdate();
}

void BlackjackActivity::playerStand() {
  holeHidden = false;
  state = State::DEALER;
  dealerStepAt = millis() + DEALER_STEP_MS;
  actionCount = 0;
  cursor = 0;
  forceClean = true;
  requestUpdate();
}

void BlackjackActivity::playerDouble() {
  chips -= bet;  // se pone la misma apuesta encima
  bet *= 2;
  doubled = true;
  player.add(drawCardFromDeck());
  if (handTotal(player) > BLACKJACK) {
    holeHidden = false;
    settle();
    return;
  }
  playerStand();  // doblar da una sola carta
}

// La banca juega de a una carta por vez para que se pueda seguir en la pantalla.
void BlackjackActivity::dealerStep() {
  if (millis() < dealerStepAt) return;
  if (handTotal(dealer) < DEALER_STANDS && dealer.count < MAX_CARDS) {
    dealer.add(drawCardFromDeck());
    dealerStepAt = millis() + DEALER_STEP_MS;
    requestUpdate();
    return;
  }
  settle();
}

void BlackjackActivity::settle() {
  const int playerTotal = handTotal(player);
  const int dealerTotal = handTotal(dealer);
  const bool playerBj = isNatural(player);
  const bool dealerBj = isNatural(dealer);
  naturalWin = false;

  if (playerTotal > BLACKJACK) {
    outcome = Outcome::LOST;  // se pasó, la apuesta ya está en la mesa
  } else if (playerBj && dealerBj) {
    outcome = Outcome::PUSH;
    chips += bet;
  } else if (playerBj) {
    outcome = Outcome::WON;
    naturalWin = true;
    chips += bet + (bet * 3) / 2;  // el natural paga 3:2
  } else if (dealerBj) {
    outcome = Outcome::LOST;
  } else if (dealerTotal > BLACKJACK || playerTotal > dealerTotal) {
    outcome = Outcome::WON;
    chips += bet * 2;
  } else if (playerTotal == dealerTotal) {
    outcome = Outcome::PUSH;
    chips += bet;
  } else {
    outcome = Outcome::LOST;
  }

  if (chips > best) best = chips;
  lastDelta = chips - chipsAtRoundStart;
  holeHidden = false;
  state = State::RESULT;
  buildActions();
  forceClean = true;
  requestUpdate();
}

// La lista que recorre la palanca: solo lo que se puede hacer en este momento.
void BlackjackActivity::buildActions() {
  const Action previous = actionCount > 0 && cursor < actionCount ? actions[cursor] : Action::DEAL;
  actionCount = 0;
  switch (state) {
    case State::BETTING:
      actions[actionCount++] = Action::DEAL;
      if (bet + BET_STEP <= chips) actions[actionCount++] = Action::BET_UP;
      if (bet - BET_STEP >= BET_STEP) actions[actionCount++] = Action::BET_DOWN;
      break;
    case State::PLAYER:
      if (player.count < MAX_CARDS) actions[actionCount++] = Action::HIT;
      actions[actionCount++] = Action::STAND;
      if (player.count == 2 && chips >= bet) actions[actionCount++] = Action::DOUBLE;
      break;
    case State::RESULT:
      actions[actionCount++] = chips >= BET_STEP ? Action::CONTINUE : Action::NEW_GAME;
      actions[actionCount++] = Action::QUIT;
      break;
    case State::DEALER:
    default:
      break;  // mientras juega la banca no hay nada que elegir
  }
  // Se conserva lo que estaba elegido si sigue estando (subir la apuesta varias
  // veces seguidas no mueve el cursor).
  cursor = 0;
  for (int i = 0; i < actionCount; ++i) {
    if (actions[i] == previous) {
      cursor = i;
      break;
    }
  }
}

void BlackjackActivity::runAction(const Action action) {
  switch (action) {
    case Action::DEAL:
      startRound();
      break;
    case Action::BET_UP:
      if (bet + BET_STEP <= chips) bet += BET_STEP;
      buildActions();
      requestUpdate();
      break;
    case Action::BET_DOWN:
      if (bet - BET_STEP >= BET_STEP) bet -= BET_STEP;
      buildActions();
      requestUpdate();
      break;
    case Action::HIT:
      playerHit();
      break;
    case Action::STAND:
      playerStand();
      break;
    case Action::DOUBLE:
      playerDouble();
      break;
    case Action::CONTINUE:
      continueRound();
      break;
    case Action::NEW_GAME:
      newGame();
      break;
    case Action::QUIT:
      finish();
      break;
  }
}

void BlackjackActivity::loop() {
  // Atrás mantenido primero: se come la suelta, así no sale del juego.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    newGame();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::DEALER) {
    dealerStep();
    return;
  }

  if (actionCount > 1) {
    buttonNavigator.onNext([this] {
      cursor = ButtonNavigator::nextIndex(cursor, actionCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      cursor = ButtonNavigator::previousIndex(cursor, actionCount);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && actionCount > 0 && cursor < actionCount) {
    runAction(actions[cursor]);
  }
}

// --------------------------------------------------------------- dibujo -----

// Círculo lleno por líneas: el renderer no trae uno y los palos lo necesitan.
void BlackjackActivity::fillDisc(const int cx, const int cy, const int r, const bool state) const {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; ++dy) {
    const int dx = static_cast<int>(sqrtf(static_cast<float>(r * r - dy * dy)));
    renderer.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, state);
  }
}

// Los cuatro palos, macizos y centrados en (cx, cy). En blanco y negro no hay
// rojo ni negro: lo que los distingue es la silueta.
void BlackjackActivity::drawSuitIcon(const int cx, const int cy, const int suit, const bool big) const {
  const int idx = suit >= 0 && suit < 4 ? suit : 0;
  const freeink::Icon& icon = *(big ? SUIT_BIG[idx] : SUIT_SMALL[idx]);
  blitIcon(renderer, icon, cx - icon.w / 2, cy - icon.h / 2, false);
}

// El valor en la esquina. Con `rotated`, (x, y) sigue siendo la esquina de
// arriba a la izquierda de la caja: el dibujo va girado adentro.
void BlackjackActivity::drawRankGlyph(const int x, const int y, const uint8_t card, const bool rotated) const {
  const freeink::Icon& icon = *RANK_ICONS[rankOf(card)];
  blitIcon(renderer, icon, x, y, rotated);
}

// Carta de verdad: sombra abajo a la derecha, cuerpo blanco con borde
// redondeado, el valor arriba a la izquierda y repetido dado vuelta abajo a la
// derecha, y el palo grande en el medio.
void BlackjackActivity::drawCardFace(const int x, const int y, const uint8_t card) const {
  renderer.fillRoundedRect(x + CARD_SHADOW, y + CARD_SHADOW, CARD_W, CARD_H, 9, Color::Black);
  // El relleno blanco tapa la sombra propia y la carta de abajo: así el abanico
  // se ve escalonado.
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 9, Color::White);
  renderer.drawRoundedRect(x, y, CARD_W, CARD_H, 2, 9, true);

  const freeink::Icon& rank = *RANK_ICONS[rankOf(card)];
  drawRankGlyph(x + CARD_PAD, y + CARD_PAD, card, false);
  drawRankGlyph(x + CARD_W - CARD_PAD - rank.w, y + CARD_H - CARD_PAD - rank.h, card, true);
  drawSuitIcon(x + CARD_W / 2, y + CARD_H / 2, suitOf(card), true);
}

void BlackjackActivity::drawCardBack(const int x, const int y) const {
  renderer.fillRoundedRect(x + CARD_SHADOW, y + CARD_SHADOW, CARD_W, CARD_H, 9, Color::Black);
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 9, Color::White);
  renderer.drawRoundedRect(x, y, CARD_W, CARD_H, 2, 9, true);
  // Rombos del dorso: dos familias de diagonales recortadas a mano contra el
  // rectángulo interior (drawLine no recorta por sí solo).
  const int ix = x + 7;
  const int iy = y + 7;
  const int iw = CARD_W - 14;
  const int ih = CARD_H - 14;
  renderer.drawRect(ix, iy, iw, ih, 1, true);
  for (int d = -ih; d < iw; d += 8) {
    const int t0 = d < 0 ? -d : 0;
    const int t1 = iw - d < ih ? iw - d : ih;
    if (t1 > t0) renderer.drawLine(ix + d + t0, iy + t0, ix + d + t1, iy + t1, true);
  }
  for (int d = 0; d <= iw + ih; d += 8) {
    const int t0 = d - iw > 0 ? d - iw : 0;
    const int t1 = d < ih ? d : ih;
    if (t1 > t0) renderer.drawLine(ix + d - t0, iy + t0, ix + d - t1, iy + t1, true);
  }
  // Escudo del medio: los cuatro palos chicos sobre un óvalo blanco, como en un
  // dorso de verdad. Además rompe la trama, que sola cansa la vista.
  constexpr int EMBLEM = 48;
  const int ex = x + (CARD_W - EMBLEM) / 2;
  const int ey = y + (CARD_H - EMBLEM) / 2;
  renderer.fillRoundedRect(ex, ey, EMBLEM, EMBLEM, 10, Color::White);
  renderer.drawRoundedRect(ex, ey, EMBLEM, EMBLEM, 2, 10, true);
  const int q = EMBLEM / 4;
  for (int i = 0; i < 4; ++i) {
    drawSuitIcon(ex + q + (i % 2) * 2 * q, ey + q + (i / 2) * 2 * q, i, false);
  }
}

// Mano centrada en abanico. Con muchas cartas se superponen, pero el valor de
// arriba a la izquierda de cada una queda siempre a la vista (por eso el paso
// nunca baja del ancho de ese valor más su margen).
void BlackjackActivity::drawHand(const Hand& hand, const int y, const bool hideSecond) const {
  if (hand.count == 0) return;
  const int pageWidth = renderer.getScreenWidth();
  const int maxWidth = pageWidth - 2 * SIDE;
  constexpr int MIN_STRIDE = 34;  // ancho del valor más ancho ("10") más el margen
  int stride = CARD_W + 10;
  if (hand.count > 1 && CARD_W + (hand.count - 1) * stride > maxWidth) {
    stride = (maxWidth - CARD_W) / (hand.count - 1);
    if (stride < MIN_STRIDE) stride = MIN_STRIDE;
  }
  const int totalWidth = CARD_W + (hand.count - 1) * stride;
  int x = (pageWidth - totalWidth) / 2;
  for (int i = 0; i < hand.count; ++i) {
    if (hideSecond && i == 1) {
      drawCardBack(x, y);
    } else {
      drawCardFace(x, y, hand.cards[i]);
    }
    x += stride;
  }
}

void BlackjackActivity::drawBadge(const int x, const int y, const char* text, const bool filled) const {
  if (filled) {
    renderer.fillRoundedRect(x, y, BADGE_W, BADGE_H, 8, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, BADGE_W, BADGE_H, 2, 8, true);
  }
  const int width = renderer.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x + (BADGE_W - width) / 2, y + 4, text, !filled, EpdFontFamily::BOLD);
}

// La apuesta, como una ficha de casino sobre la mesa.
void BlackjackActivity::drawBetChip(const int cx, const int cy, const int r) const {
  fillDisc(cx, cy, r, true);
  fillDisc(cx, cy, r - 5, false);
  // Marquitas del borde, cada 45 grados.
  static constexpr int DX[8] = {100, 71, 0, -71, -100, -71, 0, 71};
  static constexpr int DY[8] = {0, 71, 100, 71, 0, -71, -100, -71};
  const int ring = r - 9;
  for (int i = 0; i < 8; ++i) {
    renderer.fillRect(cx + (DX[i] * ring) / 100 - 3, cy + (DY[i] * ring) / 100 - 3, 6, 6, true);
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", bet);
  const int width = renderer.getTextWidth(UI_10_FONT_ID, buf, EpdFontFamily::BOLD);
  const int height = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, cx - width / 2, cy - height / 2, buf, true, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawActions(const int top) const {
  const int pageWidth = renderer.getScreenWidth();
  const int width = pageWidth - 2 * SIDE;
  char buf[40];
  for (int i = 0; i < actionCount; ++i) {
    const int y = top + i * ACTION_ROW_H;
    const bool selected = i == cursor;
    if (selected) {
      drawSelectionRow(renderer, SIDE, y, width, ACTION_ROW_H - 10, 12);
    } else {
      renderer.drawRoundedRect(SIDE, y, width, ACTION_ROW_H - 10, 2, 12, true);
    }
    const char* label = "";
    switch (actions[i]) {
      case Action::DEAL:
        label = tr(STR_GAME_NEW);
        break;
      case Action::BET_UP:
        snprintf(buf, sizeof(buf), "+%d", BET_STEP);
        label = buf;
        break;
      case Action::BET_DOWN:
        snprintf(buf, sizeof(buf), "-%d", BET_STEP);
        label = buf;
        break;
      case Action::HIT:
        label = tr(STR_GAME_HIT);
        break;
      case Action::STAND:
        label = tr(STR_GAME_STAND);
        break;
      case Action::DOUBLE:
        snprintf(buf, sizeof(buf), "x2 (%d)", bet * 2);
        label = buf;
        break;
      case Action::CONTINUE:
        label = tr(STR_GAME_CONTINUE);
        break;
      case Action::NEW_GAME:
        label = tr(STR_GAME_NEW);
        break;
      case Action::QUIT:
        label = tr(STR_GAME_QUIT);
        break;
    }
    const std::string shown = renderer.truncatedText(UI_12_FONT_ID, label, width - 24, EpdFontFamily::BOLD);
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, SIDE + (width - textWidth) / 2, y + 11, shown.c_str(), SELECTION_INK,
                      EpdFontFamily::BOLD);
  }
}

void BlackjackActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  char buf[64];

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_BLACKJACK));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomLimit = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // --- mano de la banca (arriba, con el total en un marco vacío) ---
  const int dealerRowY = contentTop;
  const int dealerCardsY = dealerRowY + BADGE_H + 6;
  if (dealer.count > 0) {
    if (holeHidden) {
      snprintf(buf, sizeof(buf), "%d ?", cardValue(dealer.cards[0]));
    } else {
      snprintf(buf, sizeof(buf), "%d", handTotal(dealer));
    }
    drawBadge(SIDE, dealerRowY, buf, false);
    drawHand(dealer, dealerCardsY, holeHidden);
  } else {
    // Antes de repartir: el mazo esperando sobre la mesa.
    drawCardBack(pageWidth / 2 - CARD_W / 2 - 6, dealerCardsY);
    drawCardBack(pageWidth / 2 - CARD_W / 2 + 6, dealerCardsY);
  }

  // --- fichas, apuesta y mejor marca ---
  const int chipY = dealerCardsY + CARD_H + 38;
  snprintf(buf, sizeof(buf), "%s %d", tr(STR_GAME_SCORE), chips);
  std::string line = renderer.truncatedText(UI_10_FONT_ID, buf, pageWidth / 2 - 60, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, SIDE, chipY - 10, line.c_str(), true, EpdFontFamily::BOLD);
  snprintf(buf, sizeof(buf), "%s %d", tr(STR_GAME_BEST), best);
  line = renderer.truncatedText(UI_10_FONT_ID, buf, pageWidth / 2 - 60);
  renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(UI_10_FONT_ID, line.c_str()), chipY - 10,
                    line.c_str());
  drawBetChip(pageWidth / 2, chipY, 30);

  // --- mano del jugador (abajo, con el total en un marco lleno) ---
  const int playerRowY = chipY + 36;
  const int playerCardsY = playerRowY + BADGE_H + 6;
  if (player.count > 0) {
    snprintf(buf, sizeof(buf), "%d", handTotal(player));
    drawBadge(SIDE, playerRowY, buf, true);
    drawHand(player, playerCardsY, false);
  }

  // --- acciones abajo de todo, y el mensaje centrado en lo que sobra ---
  const int actionsTop = bottomLimit - actionCount * ACTION_ROW_H;
  drawActions(actionsTop);

  const int handBottom = playerCardsY + (player.count > 0 ? CARD_H : 0);
  const int messageCenter = (handBottom + actionsTop) / 2;
  const char* message = nullptr;
  const char* detail = nullptr;
  char detailBuf[40] = "";
  switch (state) {
    case State::PLAYER:
      message = tr(STR_GAME_YOUR_TURN);
      break;
    case State::DEALER:
      message = tr(STR_GAME_THINKING);
      break;
    case State::RESULT:
      message = outcome == Outcome::WON ? tr(STR_GAME_WON)
                : outcome == Outcome::LOST ? tr(STR_GAME_LOST)
                                           : tr(STR_GAME_DRAW);
      if (chips < BET_STEP) {
        detail = tr(STR_GAME_OVER);
      } else {
        snprintf(detailBuf, sizeof(detailBuf), naturalWin ? "%+d  (21  3:2)" : "%+d", lastDelta);
        detail = detailBuf;
      }
      break;
    default:
      break;
  }
  if (message != nullptr) {
    renderer.drawCenteredText(UI_12_FONT_ID, messageCenter - 30, message, true, EpdFontFamily::BOLD);
  }
  if (detail != nullptr) {
    renderer.drawCenteredText(UI_10_FONT_ID, messageCenter + 2, detail);
  }

  // Atrás sale del juego en cualquier estado; mientras reparte la banca no hay
  // nada que elegir, así que la palanca y OK no anuncian nada.
  const auto labels = state == State::DEALER || actionCount == 0
                          ? mappedInput.mapLabels(tr(STR_GAME_QUIT), "", "", "")
                          : mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_SELECT),
                                                  actionCount > 1 ? tr(STR_DIR_UP) : "",
                                                  actionCount > 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Casi todo parcial; uno limpio en los cambios grandes y cada tantos, que si
  // no el panel fantasmea con tanta carta dibujada encima.
  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
