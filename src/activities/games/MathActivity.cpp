#include "MathActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 24;          // margen lateral
constexpr int NO_TRAP = -100000;  // "esta cuenta no tiene distractor especial"

// Dígitos de 7 segmentos: a b c d e f g (arriba, arriba-derecha, abajo-derecha,
// abajo, abajo-izquierda, arriba-izquierda, medio). Mismo truco que el
// temporizador: ninguna fuente del aparato llega a este tamaño.
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

bool isOperator(const char c) { return c == '+' || c == '-' || c == '*' || c == '/' || c == '='; }

int rnd(const int lo, const int hi) {
  if (hi <= lo) return lo;
  return lo + static_cast<int>(random(hi - lo + 1));
}
}  // namespace

uint16_t MathActivity::bestScore = 0;

// ----------------------------------------------------------------- partida --

void MathActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  state = State::READY;
  questionIndex = 0;
  score = 0;
  correctCount = 0;
  streak = 0;
  bestStreak = 0;
  expr[0] = '\0';
  exprSolved[0] = '\0';
  forceClean = true;
  requestUpdate();
}

void MathActivity::startGame() {
  questionIndex = 0;
  score = 0;
  correctCount = 0;
  streak = 0;
  bestStreak = 0;
  chosen = -1;
  gained = 0;
  makeQuestion(0);
}

// La escalera de dificultad: primero sumas y restas de dos cifras, después
// tablas, divisiones exactas, el número que falta y al final las combinadas.
void MathActivity::makeQuestion(const int index) {
  static const Kind LADDER[QUESTIONS] = {Kind::ADD,     Kind::SUB, Kind::ADD,     Kind::MUL,   Kind::MISSING,
                                         Kind::MUL,     Kind::DIV, Kind::MISSING, Kind::COMBO, Kind::COMBO};
  const int idx = index < 0 ? 0 : (index >= QUESTIONS ? QUESTIONS - 1 : index);
  kind = LADDER[idx];

  int a = 0;
  int b = 0;
  int c = 0;
  int trap = NO_TRAP;

  switch (kind) {
    case Kind::ADD:
      a = rnd(12, 89);
      b = rnd(11, 99 - (a > 60 ? 30 : 10));
      answer = a + b;
      snprintf(expr.data(), expr.size(), "%d+%d", a, b);
      break;

    case Kind::SUB:
      a = rnd(35, 99);
      b = rnd(12, a - 11);
      answer = a - b;
      trap = a + b;  // el error de signo de toda la vida
      snprintf(expr.data(), expr.size(), "%d-%d", a, b);
      break;

    case Kind::MUL:
      if (idx >= 5 && random(2) == 0) {
        a = rnd(11, 29);  // dos cifras por una, ya cuesta un poco más
        b = rnd(3, 9);
      } else {
        a = rnd(3, 12);
        b = rnd(3, 9);
      }
      answer = a * b;
      snprintf(expr.data(), expr.size(), "%d*%d", a, b);
      break;

    case Kind::DIV:
      b = rnd(3, 12);
      answer = rnd(3, 12);
      a = b * answer;  // siempre exacta
      trap = a - b;
      snprintf(expr.data(), expr.size(), "%d/%d", a, b);
      break;

    case Kind::COMBO:
      switch (random(4)) {
        case 0:
          a = rnd(3, 15);
          b = rnd(2, 12);
          c = rnd(2, 9);
          answer = (a + b) * c;
          trap = a + b * c;  // el error de precedencia
          snprintf(expr.data(), expr.size(), "(%d+%d)*%d", a, b, c);
          break;
        case 1:
          a = rnd(9, 19);
          b = rnd(2, a - 3);
          c = rnd(2, 9);
          answer = (a - b) * c;
          trap = a - b * c;
          snprintf(expr.data(), expr.size(), "(%d-%d)*%d", a, b, c);
          break;
        case 2:
          a = rnd(3, 12);
          b = rnd(2, 9);
          c = rnd(11, 49);
          answer = a * b + c;
          trap = a * (b + c);
          snprintf(expr.data(), expr.size(), "%d*%d+%d", a, b, c);
          break;
        default:
          a = rnd(4, 12);
          b = rnd(4, 9);
          c = rnd(11, 29);
          answer = a * b - c;
          trap = a * b + c;
          snprintf(expr.data(), expr.size(), "%d*%d-%d", a, b, c);
          break;
      }
      break;

    case Kind::MISSING:
    default:
      if (idx <= 5) {
        if (random(2) == 0) {
          answer = rnd(11, 49);
          b = rnd(11, 49);
          c = answer + b;
          trap = c + b;
          snprintf(expr.data(), expr.size(), "_+%d=%d", b, c);
        } else {
          a = rnd(35, 99);
          answer = rnd(12, a - 11);
          c = a - answer;
          trap = a + c;
          snprintf(expr.data(), expr.size(), "%d-_=%d", a, c);
        }
      } else if (random(2) == 0) {
        a = rnd(3, 12);
        answer = rnd(3, 12);
        c = a * answer;
        trap = c - a;
        snprintf(expr.data(), expr.size(), "%d*_=%d", a, c);
      } else {
        answer = rnd(2, 12);
        b = rnd(2, 12);
        a = answer * b;
        trap = a - b;
        snprintf(expr.data(), expr.size(), "%d/_=%d", a, b);
      }
      break;
  }

  // La misma cuenta ya resuelta, para mostrarla con el bien/mal.
  if (strchr(expr.data(), '_') != nullptr) {
    size_t out = 0;
    for (size_t i = 0; i < expr.size() && expr[i] != '\0' && out + 8 < exprSolved.size(); ++i) {
      if (expr[i] == '_') {
        out += static_cast<size_t>(snprintf(exprSolved.data() + out, exprSolved.size() - out, "%d", answer));
      } else {
        exprSolved[out++] = expr[i];
      }
    }
    exprSolved[out] = '\0';
  } else {
    snprintf(exprSolved.data(), exprSolved.size(), "%s=%d", expr.data(), answer);
  }

  buildOptions(answer, kind, a, b, trap);

  questionIndex = idx;
  cursor = 0;
  chosen = -1;
  state = State::QUESTION;
  questionAt = millis();
  frozenLeftMs = static_cast<long>(ANSWER_MS);
  lastShownSeconds = -1;
  forceClean = true;
  requestUpdate();
}

// Tres distractores creíbles: el error típico de cada tipo de cuenta (acarreo,
// tabla de al lado, signo, precedencia), nunca números al azar.
void MathActivity::buildOptions(const int correct, const Kind questionKind, const int a, const int b, const int trap) {
  std::array<int, 14> cand{};
  int n = 0;
  const auto push = [&](const int v) {
    if (n < static_cast<int>(cand.size())) cand[n++] = v;
  };

  if (trap != NO_TRAP) push(trap);
  switch (questionKind) {
    case Kind::ADD:
    case Kind::SUB:
      push(correct + 10);
      push(correct - 10);
      push(correct + 1);
      push(correct - 1);
      push(correct + 9);
      push(correct - 9);
      push(correct + 2);
      push(correct - 11);
      break;

    case Kind::MUL:
      push(correct + a);
      push(correct - a);
      push(correct + b);
      push(correct - b);
      push(correct + 10);
      push(correct - 10);
      push(correct + 1);
      push(correct - 2);
      break;

    case Kind::DIV:
      push(correct + 1);
      push(correct - 1);
      push(correct + 2);
      push(correct - 2);
      push(correct + 10);
      push(correct * 2);
      push(correct + 3);
      break;

    case Kind::COMBO:
      push(correct + 1);
      push(correct - 1);
      push(correct + 10);
      push(correct - 10);
      push(correct + 2);
      push(correct - 2);
      push(correct + 20);
      break;

    case Kind::MISSING:
    default:
      push(correct + 1);
      push(correct - 1);
      push(correct + 10);
      push(correct - 10);
      push(correct + 2);
      push(correct - 2);
      push(correct + 9);
      break;
  }

  // Mezcla los candidatos y toma los tres primeros que sirvan.
  for (int i = n - 1; i > 0; --i) {
    const int j = static_cast<int>(random(i + 1));
    const int tmp = cand[static_cast<size_t>(i)];
    cand[static_cast<size_t>(i)] = cand[static_cast<size_t>(j)];
    cand[static_cast<size_t>(j)] = tmp;
  }

  std::array<int, OPTIONS> picked{};
  int count = 0;
  picked[static_cast<size_t>(count++)] = correct;
  for (int i = 0; i < n && count < OPTIONS; ++i) {
    const int v = cand[static_cast<size_t>(i)];
    if (v < 0 || v > 9999) continue;
    bool dup = false;
    for (int k = 0; k < count; ++k) {
      if (picked[static_cast<size_t>(k)] == v) dup = true;
    }
    if (!dup) picked[static_cast<size_t>(count++)] = v;
  }
  // Por si los candidatos se repetían todos: rellena con vecinos.
  for (int step = 3; count < OPTIONS; ++step) {
    const int v = correct + step;
    bool dup = false;
    for (int k = 0; k < count; ++k) {
      if (picked[static_cast<size_t>(k)] == v) dup = true;
    }
    if (!dup) picked[static_cast<size_t>(count++)] = v;
  }

  // Mezcla las cuatro opciones y anota dónde quedó la correcta.
  answerIndex = 0;
  for (int i = OPTIONS - 1; i > 0; --i) {
    const int j = static_cast<int>(random(i + 1));
    const int tmp = picked[static_cast<size_t>(i)];
    picked[static_cast<size_t>(i)] = picked[static_cast<size_t>(j)];
    picked[static_cast<size_t>(j)] = tmp;
  }
  for (int i = 0; i < OPTIONS; ++i) {
    options[static_cast<size_t>(i)] = picked[static_cast<size_t>(i)];
    if (picked[static_cast<size_t>(i)] == correct) answerIndex = i;
  }
}

long MathActivity::remainingMs() const {
  const long elapsed = static_cast<long>(millis() - questionAt);
  const long left = static_cast<long>(ANSWER_MS) - elapsed;
  return left < 0 ? 0 : left;
}

// index == -1: se acabó el tiempo, cuenta como error.
void MathActivity::answerWith(const int index) {
  frozenLeftMs = remainingMs();
  chosen = index;
  lastOk = index >= 0 && index == answerIndex;
  gained = 0;
  if (lastOk) {
    ++correctCount;
    ++streak;
    if (streak > bestStreak) bestStreak = streak;
    const int timeBonus = static_cast<int>(frozenLeftMs / 1000);
    const int streakBonus = (streak < 5 ? streak : 5) * 2;
    gained = 10 + timeBonus + streakBonus;
    score += gained;
  } else {
    streak = 0;
  }
  state = State::FEEDBACK;
  feedbackAt = millis();
  forceClean = true;
  requestUpdate();
}

void MathActivity::nextQuestion() {
  if (questionIndex + 1 >= QUESTIONS) {
    if (score > static_cast<int>(bestScore)) bestScore = static_cast<uint16_t>(score);
    state = State::OVER;
    forceClean = true;
    requestUpdate();
    return;
  }
  makeQuestion(questionIndex + 1);
}

// -------------------------------------------------------------------- loop --

void MathActivity::loop() {
  // Atrás mantenido: abandona la partida y vuelve al inicio. Consume la suelta,
  // así que no dispara también la salida.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, RESTART_HOLD_MS)) {
    if (state != State::READY) {
      state = State::READY;
      forceClean = true;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  switch (state) {
    case State::READY:
      if (confirm) startGame();
      break;

    case State::QUESTION: {
      buttonNavigator.onNext([this] {
        cursor = ButtonNavigator::nextIndex(cursor, OPTIONS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        cursor = ButtonNavigator::previousIndex(cursor, OPTIONS);
        requestUpdate();
      });
      if (confirm) {
        answerWith(cursor);
        break;
      }
      const long left = remainingMs();
      if (left <= 0) {
        answerWith(-1);
        break;
      }
      // La barra baja sola: se repinta cada dos segundos y, en los últimos
      // cinco, cada segundo. Un parcial por segundo todo el tiempo sería una
      // paliza para el panel.
      const long secs = (left + 999) / 1000;
      if (secs != lastShownSeconds && (secs <= 5 || secs % 2 == 0)) requestUpdate();
      break;
    }

    case State::FEEDBACK:
      if (millis() - feedbackAt >= FEEDBACK_MS || (confirm && millis() - feedbackAt >= FEEDBACK_MIN_MS)) {
        nextQuestion();
      }
      break;

    case State::OVER:
      if (confirm) startGame();
      break;
  }
}

// ---------------------------------------------------- números y símbolos XL --

int MathActivity::glyphWidth(const char c, const int w) {
  if (c == '(' || c == ')') return w * 45 / 100;
  if (isOperator(c)) return w * 85 / 100;
  return w;  // dígitos y el casillero del que falta
}

int MathActivity::glyphGap(const char left, const char right, const int w) {
  return (isOperator(left) || isOperator(right)) ? w / 2 : w / 6;
}

int MathActivity::measureText(const char* text, const int w) {
  int total = 0;
  for (int i = 0; text[i] != '\0'; ++i) {
    if (i > 0) total += glyphGap(text[i - 1], text[i], w);
    total += glyphWidth(text[i], w);
  }
  return total;
}

void MathActivity::drawGlyph(const char c, const int x, const int y, const int w, const int h, const int t,
                             const bool ink) const {
  const int gw = glyphWidth(c, w);
  const int cx = x + gw / 2;
  const int cy = y + h / 2;

  if (c >= '0' && c <= '9') {
    const uint8_t s = SEGMENTS[static_cast<int>(c - '0')];
    const int half = h / 2;
    if (s & 0x01) renderer.fillRect(x + t, y, gw - 2 * t, t, ink);                 // a
    if (s & 0x02) renderer.fillRect(x + gw - t, y + t, t, half - t, ink);          // b
    if (s & 0x04) renderer.fillRect(x + gw - t, y + half, t, half - t, ink);       // c
    if (s & 0x08) renderer.fillRect(x + t, y + h - t, gw - 2 * t, t, ink);         // d
    if (s & 0x10) renderer.fillRect(x, y + half, t, half - t, ink);                // e
    if (s & 0x20) renderer.fillRect(x, y + t, t, half - t, ink);                   // f
    if (s & 0x40) renderer.fillRect(x + t, y + half - t / 2, gw - 2 * t, t, ink);  // g
    return;
  }

  const int arm = gw * 38 / 100;
  switch (c) {
    case '+':
      renderer.fillRect(cx - arm, cy - t / 2, 2 * arm, t, ink);
      renderer.fillRect(cx - t / 2, cy - arm, t, 2 * arm, ink);
      break;

    case '-':
      renderer.fillRect(cx - arm, cy - t / 2, 2 * arm, t, ink);
      break;

    case '=':
      renderer.fillRect(cx - arm, cy - t - t / 2, 2 * arm, t, ink);
      renderer.fillRect(cx - arm, cy + t / 2, 2 * arm, t, ink);
      break;

    case '*':  // por
      renderer.drawLine(cx - arm, cy - arm, cx + arm, cy + arm, t, ink);
      renderer.drawLine(cx - arm, cy + arm, cx + arm, cy - arm, t, ink);
      break;

    case '/': {  // dividido
      renderer.fillRect(cx - arm, cy - t / 2, 2 * arm, t, ink);
      const int dot = t + 2;
      renderer.fillRect(cx - dot / 2, cy - 3 * t - dot / 2, dot, dot, ink);
      renderer.fillRect(cx - dot / 2, cy + 3 * t - dot / 2, dot, dot, ink);
      break;
    }

    case '(':
    case ')': {
      const int q = h / 6;
      const int reach = c == '(' ? gw / 2 : -gw / 2;
      renderer.fillRect(cx - t / 2, y + q, t, h - 2 * q, ink);
      renderer.drawLine(cx, y + q, cx + reach, y, t, ink);
      renderer.drawLine(cx, y + h - q, cx + reach, y + h, t, ink);
      break;
    }

    case '_':  // el número que falta: un casillero vacío
      renderer.drawRoundedRect(x + 2, y + 2, gw - 4, h - 4, t, h / 8, ink);
      break;

    default:
      break;
  }
}

// Elige el tamaño más grande que entre en el hueco y dibuja la cuenta centrada.
void MathActivity::drawBigText(const char* text, const int cx, const int cy, const int maxWidth, const int maxHeight,
                               const bool ink) const {
  if (text == nullptr || text[0] == '\0') return;
  int w = 68;
  while (w > 14) {
    const int h = w * 17 / 10;
    if (h <= maxHeight && measureText(text, w) <= maxWidth) break;
    w -= 2;
  }
  const int h = w * 17 / 10;
  const int t = w / 6 < 4 ? 4 : w / 6;
  int x = cx - measureText(text, w) / 2;
  const int y = cy - h / 2;
  for (int i = 0; text[i] != '\0'; ++i) {
    if (i > 0) x += glyphGap(text[i - 1], text[i], w);
    drawGlyph(text[i], x, y, w, h, t, ink);
    x += glyphWidth(text[i], w);
  }
}

// ------------------------------------------------------------------ dibujo --

void MathActivity::drawInfoBar(const int y) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[48];

  snprintf(buf, sizeof(buf), "%s %d/%d", I18N.get(StrId::STR_GAME_LEVEL), questionIndex + 1, QUESTIONS);
  renderer.drawText(UI_10_FONT_ID, SIDE, y, buf);

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);

  snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestScore));
  renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(UI_10_FONT_ID, buf), y, buf);
}

void MathActivity::drawTimeBar(const int y, const int height) const {
  const int pageWidth = renderer.getScreenWidth();
  const int barW = pageWidth - 2 * SIDE;
  char buf[32];

  const long left = state == State::FEEDBACK ? frozenLeftMs : remainingMs();
  const long secs = (left + 999) / 1000;

  // Etiqueta a la izquierda, segundos a la derecha y la racha en bolitas (sin
  // texto nuevo: los strings del producto son los que ya están traducidos).
  renderer.drawText(SMALL_FONT_ID, SIDE, y - 22, I18N.get(StrId::STR_GAME_TIME));
  snprintf(buf, sizeof(buf), "%ld", secs);
  renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, buf), y - 22, buf);
  const int dots = streak < 5 ? streak : 5;
  for (int i = 0; i < dots; ++i) {
    renderer.fillRect(pageWidth / 2 - dots * 8 + i * 16, y - 16, 10, 10);
  }

  renderer.drawRoundedRect(SIDE, y, barW, height, 2, height / 2, true);
  const int inner = barW - 8;
  const int fill = static_cast<int>(static_cast<long long>(inner) * left / static_cast<long>(ANSWER_MS));
  if (fill > 2) renderer.fillRoundedRect(SIDE + 4, y + 4, fill, height - 8, (height - 8) / 2, Color::Black);
}

void MathActivity::drawExpression(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  const char* text = state == State::FEEDBACK ? exprSolved.data() : expr.data();
  drawBigText(text, pageWidth / 2, (top + bottom) / 2, pageWidth - 2 * SIDE, bottom - top - 8, true);
}

void MathActivity::drawOptions(const int top, const int rowHeight, const int gap) const {
  const int pageWidth = renderer.getScreenWidth();
  const int boxW = pageWidth - 2 * SIDE;
  char buf[16];

  for (int i = 0; i < OPTIONS; ++i) {
    const int y = top + i * (rowHeight + gap);
    const bool cursorHere = state == State::QUESTION && i == cursor;
    const bool isAnswer = state == State::FEEDBACK && i == answerIndex;
    const bool filled = cursorHere || isAnswer;

    if (filled) {
      renderer.fillRoundedRect(SIDE, y, boxW, rowHeight, 12, Color::Black);
    } else {
      renderer.drawRoundedRect(SIDE, y, boxW, rowHeight, 2, 12, true);
    }
    // Lo que eligió, si estuvo mal: marco por fuera para que se vea el error.
    if (state == State::FEEDBACK && !lastOk && i == chosen) {
      renderer.drawRoundedRect(SIDE - 6, y - 5, boxW + 12, rowHeight + 10, 3, 16, true);
    }

    snprintf(buf, sizeof(buf), "%d", options[static_cast<size_t>(i)]);
    drawBigText(buf, pageWidth / 2, y + rowHeight / 2, boxW - 48, rowHeight - 16, !filled);
  }
}

void MathActivity::drawReady(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[48];
  const int cy = (top + bottom) / 2;

  // Una cuenta de muestra, para que se vea de qué va.
  drawBigText("7*8", pageWidth / 2, cy - 60, pageWidth - 2 * SIDE, 150, true);

  renderer.fillRoundedRect(SIDE, cy + 50, pageWidth - 2 * SIDE, 56, 12, Color::Black);
  renderer.drawCenteredText(UI_12_FONT_ID, cy + 66, I18N.get(StrId::STR_GAME_NEW), false, EpdFontFamily::BOLD);

  if (bestScore > 0) {
    snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestScore));
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 130, buf);
  }
}

void MathActivity::drawSummary(const int top, const int bottom) const {
  const int pageWidth = renderer.getScreenWidth();
  char buf[48];
  int y = top + (bottom - top) / 2 - 190;

  const bool perfect = correctCount >= QUESTIONS;
  renderer.fillRoundedRect(SIDE, y, pageWidth - 2 * SIDE, 58, 12, Color::Black);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 18, I18N.get(perfect ? StrId::STR_GAME_WON : StrId::STR_GAME_OVER),
                            false, EpdFontFamily::BOLD);
  y += 96;

  // Aciertos, enormes.
  snprintf(buf, sizeof(buf), "%d/%d", correctCount, QUESTIONS);
  drawBigText(buf, pageWidth / 2, y + 60, pageWidth - 2 * SIDE, 130, true);
  y += 150;

  snprintf(buf, sizeof(buf), "%s %d", I18N.get(StrId::STR_GAME_SCORE), score);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
  y += 40;

  snprintf(buf, sizeof(buf), "%s %u", I18N.get(StrId::STR_GAME_BEST), static_cast<unsigned>(bestScore));
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 40;

  // La mejor racha, en bolitas.
  const int dots = bestStreak < 10 ? bestStreak : 10;
  for (int i = 0; i < dots; ++i) {
    renderer.fillRect(pageWidth / 2 - dots * 9 + i * 18, y, 12, 12);
  }
}

void MathActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(StrId::STR_GAME_MATH));

  const int top = metrics.topPadding + metrics.headerHeight;
  // Regla de la casa: abajo siempre queda libre el alto de los hints más el espaciado.
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  if (state == State::READY) {
    drawReady(top, bottom);
  } else if (state == State::OVER) {
    drawSummary(top, bottom);
  } else {
    constexpr int ROW_H = 62;
    constexpr int ROW_GAP = 10;
    const int optionsH = OPTIONS * ROW_H + (OPTIONS - 1) * ROW_GAP;
    const int optionsTop = bottom - optionsH - 4;
    const int barY = top + 56;
    constexpr int BAR_H = 18;

    drawInfoBar(top + 6);
    drawTimeBar(barY, BAR_H);
    drawExpression(barY + BAR_H + 10, optionsTop - 46);

    if (state == State::FEEDBACK) {
      char buf[64];
      if (lastOk) {
        snprintf(buf, sizeof(buf), "%s  +%d", I18N.get(StrId::STR_GAME_CORRECT), gained);
      } else {
        snprintf(buf, sizeof(buf), "%s", I18N.get(StrId::STR_GAME_WRONG));
      }
      renderer.drawCenteredText(UI_12_FONT_ID, optionsTop - 36, buf, true, EpdFontFamily::BOLD);
    }

    drawOptions(optionsTop, ROW_H, ROW_GAP);
    lastShownSeconds = (remainingMs() + 999) / 1000;
  }

  switch (state) {
    case State::READY: {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::OVER: {
      const auto labels = mappedInput.mapLabels(tr(STR_GAME_QUIT), tr(STR_GAME_NEW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::FEEDBACK: {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_CONTINUE), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    default: {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  // Regla del panel: parciales rápidos para la barra y el cursor, y uno limpio
  // en cada cambio de pantalla o cada tantos parciales, que si no fantasmea.
  ++partialCount;
  const bool clean = forceClean || partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
