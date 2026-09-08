#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Cálculo mental: diez cuentas contra reloj, con la dificultad subiendo de a
// poco (sumas y restas de dos cifras, multiplicaciones, divisiones exactas,
// cuentas combinadas con paréntesis y "encontrá el número que falta"). Cada
// pregunta trae cuatro respuestas: la correcta y tres distractores creíbles
// (error de acarreo, de signo, de tabla o de precedencia), mezcladas.
//
// La cuenta se dibuja enorme en el centro con primitivas propias (dígitos de 7
// segmentos y símbolos hechos con barras y diagonales, como el temporizador):
// ninguna fuente del aparato llega a ese tamaño. El número que falta es un
// casillero vacío.
//
// Control (cuatro botones, sin teclado ni pantalla táctil):
//   - Pantalla de inicio y resumen final: OK arranca una partida nueva.
//   - Durante la pregunta: la palanca (ARRIBA/ABAJO) recorre las cuatro
//     opciones en círculo y OK contesta la que está marcada.
//   - Después de contestar se ve si estuvo bien o mal con la cuenta resuelta;
//     sigue sola a los 1,7 s o antes con OK.
//   - Atrás sale del juego; Atrás mantenido 1 s abandona la partida y vuelve a
//     la pantalla de inicio. OK no tiene pulsación larga en esta placa (apaga).
//
// Tiempo: 10 s por pregunta, dibujados como una barra que baja. Si se acaba,
// cuenta como error. Puntos = 10 + los segundos que sobraron + premio por
// racha; el mejor puntaje de la sesión vive en un miembro estático, así
// sobrevive a salir y volver a entrar sin tocar la SD.
class MathActivity final : public Activity {
 public:
  explicit MathActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Math", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t {
    READY,     // pantalla de inicio: OK arranca
    QUESTION,  // corriendo el reloj, esperando la respuesta
    FEEDBACK,  // bien/mal con la cuenta resuelta
    OVER       // resumen de la partida
  };

  // Tipo de cuenta. MISSING resuelve por el operando que falta.
  enum class Kind : uint8_t { ADD, SUB, MUL, DIV, COMBO, MISSING };

  static constexpr int QUESTIONS = 10;
  static constexpr int OPTIONS = 4;
  static constexpr unsigned long ANSWER_MS = 10000;      // reloj por pregunta
  static constexpr unsigned long FEEDBACK_MS = 1700;     // cuánto queda el bien/mal
  static constexpr unsigned long FEEDBACK_MIN_MS = 400;  // no saltearlo de rebote
  static constexpr unsigned long RESTART_HOLD_MS = 1000;
  static constexpr int PARTIALS_BEFORE_CLEAN = 10;  // regla del panel: limpio cada 10-15 parciales

  // Partida
  State state = State::READY;
  int questionIndex = 0;
  int score = 0;
  int correctCount = 0;
  int streak = 0;
  int bestStreak = 0;

  // Pregunta actual
  Kind kind = Kind::ADD;
  std::array<char, 24> expr{};        // la cuenta ('_' = el casillero del que falta)
  std::array<char, 32> exprSolved{};  // la misma cuenta ya resuelta, para el bien/mal
  std::array<int, OPTIONS> options{};
  int answer = 0;
  int answerIndex = 0;
  int cursor = 0;
  int chosen = -1;  // opción elegida, -1 si se acabó el tiempo
  bool lastOk = false;
  int gained = 0;  // puntos de la última respuesta

  unsigned long questionAt = 0;
  unsigned long feedbackAt = 0;
  long frozenLeftMs = 0;  // la barra queda quieta mientras se muestra el resultado
  long lastShownSeconds = -1;

  int partialCount = 0;
  bool forceClean = true;
  ButtonNavigator buttonNavigator;

  static uint16_t bestScore;  // mejor puntaje de la sesión

  // Partida
  void startGame();
  void makeQuestion(int index);
  void buildOptions(int correct, Kind questionKind, int a, int b, int trap);
  void answerWith(int index);
  void nextQuestion();
  long remainingMs() const;

  // Dibujo
  void drawInfoBar(int y) const;
  void drawTimeBar(int y, int height) const;
  void drawExpression(int top, int bottom) const;
  void drawOptions(int top, int rowHeight, int gap) const;
  void drawReady(int top, int bottom) const;
  void drawSummary(int top, int bottom) const;

  // Números y símbolos enormes, hechos con primitivas
  void drawBigText(const char* text, int cx, int cy, int maxWidth, int maxHeight, bool ink) const;
  void drawGlyph(char c, int x, int y, int w, int h, int t, bool ink) const;
  static int glyphWidth(char c, int w);
  static int glyphGap(char left, char right, int w);
  static int measureText(const char* text, int w);
};
