#pragma once

#include <string>

#include "activities/Activity.h"
#include "voice/AlertBeep.h"
#include "voice/SpeechOut.h"

// A reminder came due: title and time big on screen, a beep pattern through
// the speaker (looped, up to a minute), OK = done, Back = snooze 10 minutes.
// Reached from the hub while awake, or from the boot path after the deep-sleep
// timer wake that armReminderWake() set for it.
class ReminderAlertActivity final : public Activity {
 public:
  ReminderAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int reminderId, std::string title,
                        std::string when)
      : Activity("ReminderAlert", renderer, mappedInput),
        reminderId(reminderId),
        title(std::move(title)),
        when(std::move(when)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Sólo mientras suena, como el temporizador. Con `true` fijo el aparato
  // quedaba despierto toda la noche: `preventAutoSleep()` reinicia el contador
  // de ocio en el loop, así que una alarma a las 3 AM que nadie atiende nunca
  // llegaba al deep sleep y se comía la batería hasta la mañana.
  bool preventAutoSleep() override { return millis() - openedAt < BEEP_MS; }

 private:
  // Cuánto suena, y también cuánto se espera a que alguien conteste: pasado
  // ese minuto la alarma se posterga SOLA. Dejarla en pantalla sin postergar
  // era peor de lo que parece: el recordatorio seguía vencido, así que
  // nextWakeInstant() devolvía "ahora", el aparato se dormía y el temporizador
  // lo levantaba cinco segundos después, a repetir el ciclo para siempre.
  static constexpr unsigned long BEEP_MS = 60000;
  unsigned long openedAt = millis();
  static constexpr time_t SNOOZE_S = 10 * 60;

  int reminderId;
  std::string title;
  std::string when;
  AlertBeep beep;
  SpeechOut speech;
  bool spoken = false;  // the cached "Reminder: <title>" clip was played (or absent)
  unsigned long startedAt = 0;
  // EL GESTO DE BOCA ABAJO ES DAR VUELTA EL APARATO, NO ESTAR DADO VUELTA. Si
  // cuando se abre la alarma el aparato ya está boca abajo (apoyado sobre la
  // mesa, que es donde pasa la noche), el gesto no cuenta hasta que alguien lo
  // levante: si no, el aparato se posterga la alarma a sí mismo en cada
  // repique, cada diez minutos, para siempre.
  bool gestureArmed = false;
  // Los gestos que ya estaban pendientes cuando esta pantalla se abrió se tiran
  // en la primera pasada: son de antes, no una respuesta a la alarma.
  bool startupGesturesDropped = false;
  // Alguien apretó algo o dio vuelta el aparato a propósito. Lo que se resuelve
  // solo (el plazo de BEEP_MS) no cuenta como atendido y se va a dormir sin
  // pasar por el hub.
  bool attended = false;
  // Cuántas postergaciones traía esta ocurrencia al abrirse. Sirve para dos
  // cosas: decir en el log el número de verdad (y no la constante del tope), y
  // saber si el `dueAt` que tenemos sigue siendo el original — que es lo único
  // que el servidor puede reconocer como `at`.
  int snoozesAtOpen = 0;
  void done();
  void snooze(bool byUser);
  void giveUp();
  void leave();
};
