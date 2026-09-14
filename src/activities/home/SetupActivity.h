#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"

// Asistente de primer arranque: lo que hay que dejar hecho la primera vez y que
// nadie va a encontrar solo hurgando en Ajustes.
//
// Cinco pasos, y los tres del medio se pueden saltar: idioma, WiFi (la clave se
// escribe desde el teléfono), vincular con la cuenta, el lugar del clima, y una
// pantalla que enseña los tres gestos que no se adivinan (dos toques de Atrás =
// hablar, Atrás mantenido = sincronizar, PWR mantenido = suspender).
//
// Se abre UNA vez, en un aparato que se ve recién salido de la caja: sin
// `setupDone` guardado, sin redes WiFi cargadas y sin haber sincronizado nunca.
// Un aparato que ya se venía usando y se actualiza por OTA no lo ve.
class SetupActivity final : public Activity {
 public:
  explicit SetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Setup", renderer, mappedInput) {}

  // ¿Este aparato se ve recién sacado de la caja? Lo pregunta main.cpp para
  // decidir si la primera pantalla es el asistente o el hub.
  static bool pending();

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Step { LANGUAGE, WIFI, PAIR, PLACE, GESTURES };

  Step step = LANGUAGE;
  int langIndex = 0;   // índice dentro de los idiomas del producto
  int scroll = 0;      // primera fila visible de la lista de idiomas
  bool waiting = false;  // hay una Activity encima haciendo el paso

  void advance();
  void finishSetup();
  void openStepActivity();
  void renderLanguage() const;
  void renderCard(const char* title, const char* body) const;
  void renderGestures() const;
};
