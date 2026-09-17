#include "ReminderAlertActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "input/MotionInput.h"
#include "util/SleepRequest.h"
#include "voice/SpeechCache.h"

namespace {
constexpr const char* TAG = "REMIND";

}  // namespace

void ReminderAlertActivity::onEnter() {
  Activity::onEnter();
  startedAt = millis();
  // Si el aparato ya venía boca abajo, el gesto arranca desarmado: lo arma el
  // "boca arriba" de alguien que lo levanta. Y si el IMU todavía no leyó
  // ninguna muestra (es lo normal en un arranque por temporizador: esta
  // pantalla se abre antes de que el loop llegue a MOTION.poll()), tampoco se
  // arma acá: se decide en el loop con la primera posición conocida.
  gestureArmed = MOTION.primed() && !MOTION.faceDown();
  snoozesAtOpen = HUB_STORE.snoozeCount(reminderId);
  // "Reminder: <title>" from the SD (cached at sync), then the beeps.
  const std::string clip = speechcache::clipPath(std::string(tr(STR_HUB_REMINDERS)) + ": " + title);
  spoken = !speech.playFile(clip.c_str());
  if (spoken) {
    speech.stop();  // el I2S es uno solo: soltarlo antes de que lo abra el pitido
    beep.start();
  }
  requestUpdate();
}

void ReminderAlertActivity::onExit() {
  Activity::onExit();
  speech.stop();
  beep.stop();
}

void ReminderAlertActivity::done() {
  attended = true;
  // El dueAt de ESTA ocurrencia, leido ANTES de correr la fecha: viaja como
  // `at` para que un reintento del mismo tilde no le coma otro ciclo.
  time_t at = 0;
  for (const HubStore::Reminder& r : HUB_STORE.reminders) {
    if (r.id == reminderId) {
      at = r.dueAt;
      break;
    }
  }
  // Si repite, la cache se queda con la proxima ocurrencia: sin WiFi, borrarlo
  // dejaba al aparato sin nada que armar y el diario no volvia a sonar nunca.
  time_t now = 0;
  halClock.getEpochUtc(now);
  HUB_STORE.completeReminder(reminderId, now);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = reminderId;
    if (at > 0) doc["at"] = static_cast<int64_t>(at);
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "done %d: %s", reminderId, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/done", body)));
  leave();
}

void ReminderAlertActivity::snooze(const bool byUser) {
  if (byUser) attended = true;
  time_t now = 0;
  // SIN RELOJ NO SE TOCA EL `dueAt`. El retorno se ignoraba, así que un fallo
  // del RTC (el bus I²C es compartido y acá encima está sonando audio) dejaba
  // `now` en 0 y el posponer escribía `dueAt = 600`: enero de 1970, o sea
  // vencido para siempre. Con eso `nextWakeInstant()` devuelve "ahora", el deep
  // sleep se arma al piso de 5 s y el aparato arranca en loop hasta quedarse sin
  // batería — y el tope de postergaciones no lo corta, porque `giveUp()` reinicia
  // la racha. Mejor dejar la alarma como está y que suene de nuevo.
  if (!halClock.getEpochUtc(now) || now <= 0) {
    LOG_ERR(TAG, "sin reloj: no se posterga (quedaría vencido en 1970)");
    leave();
    return;
  }
  // El tope se mira ANTES de postergar otra vez: con MAX_SNOOZES ya cumplidas,
  // esta vez se descarta en vez de correr la alarma diez minutos más.
  if (HUB_STORE.snoozeCount(reminderId) >= HubStore::MAX_SNOOZES) {
    giveUp();
    return;
  }
  const int veces = HUB_STORE.snoozeReminder(reminderId, now + SNOOZE_S);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = reminderId;
    doc["snooze"] = static_cast<int>(SNOOZE_S);
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "snooze %d (%d de %d, %s): %s", reminderId, veces, HubStore::MAX_SNOOZES,
          byUser ? "a mano" : "sin atender",
          ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/done", body)));
  leave();
}

// Tres postergaciones sin que nadie la diera por hecha: se descarta. Si el
// recordatorio repite, la ocurrencia de hoy se da por perdida y queda armada la
// del próximo ciclo; si no repite, se borra. En los dos casos es lo mismo que
// tildarlo —completeReminder() ya hace exactamente eso—, con la diferencia de
// que el aparato avisa al servidor que se dio por vencido y no que el usuario
// lo hizo (`dismissed`), para que la Pizarra pueda decir la verdad.
void ReminderAlertActivity::giveUp() {
  time_t now = 0;
  halClock.getEpochUtc(now);
  const bool repite = HUB_STORE.completeReminder(reminderId, now);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = reminderId;
    doc["dismissed"] = true;
    // EL DESCARTE NO MANDA `at`, A PROPÓSITO. El servidor usa `at` como
    // guardia antirreplay comparándolo con SU `dueAt`: si no coinciden, da el
    // pedido por repetido y no hace nada. Y después de tres postergaciones no
    // pueden coincidir — el nuestro es "ahora + 600" con segundos, el del
    // servidor está truncado al minuto (`epochToLocal`) y corrido por el
    // desfase de reloj que el propio firmware tolera hasta 120 s. O sea que el
    // descarte se perdía entero y la sincronización siguiente resucitaba la
    // alarma con otras cuatro sonadas.
    //
    // La idempotencia del descarte no se resuelve con la marca de tiempo sino
    // con el estado: el servidor sólo cierra la ocurrencia si SIGUE VENCIDA
    // (ver `markDone` con `dismissed`), así que un reintento que llega después
    // no encuentra nada que cerrar. Eso es cierto aunque los relojes no
    // coincidan, que es justamente lo que acá falla.
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "descartado %d tras %d postergaciones (%s): %s", reminderId, snoozesAtOpen,
          repite ? "queda el proximo ciclo" : "no repite, se borra",
          ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/done", body)));
  leave();
}

void ReminderAlertActivity::leave() {
  speech.stop();
  beep.stop();
  // HAY PANTALLA DEBAJO: se vuelve a ella. `checkTimeAlarms()` abre esta
  // pantalla con pushActivity() desde donde sea que esté el usuario, y eso NO
  // deja resultHandler, así que hasta acá el `else` mandaba a TODAS al hub: una
  // alarma que sonaba en Notas o en la agenda te dejaba en el hub al
  // atenderla. Es el mismo defecto que tenía Hablar y se arregla igual.
  if (resultHandler || activityManager.hasStackedActivities()) {
    finish();
    return;
  }
  // Nada debajo: esto se abrió desde el arranque por el temporizador del
  // recordatorio, o sea que el aparato se encendió solo para esto. Si además
  // nadie la atendió, mandarla al hub deja el aparato despierto los diez
  // minutos del auto-sleep (y el hub encima puede levantar WiFi para
  // sincronizar) justo para volver a dormirse cuando la alarma vuelve a sonar:
  // eso es estar encendido el 100 % de la noche, que es como se vació la
  // batería en 1.5.91.
  if (!attended) {
    LOG_INF(TAG, "nadie la atendio y no hay pantalla debajo: a dormir");
    sleepreq::request();
    return;
  }
  activityManager.goHome();  // launched from the boot path after a timer wake
}

void ReminderAlertActivity::loop() {
  if (!spoken && !speech.isPlaying()) {
    spoken = true;
    speech.stop();
    beep.start();
  }
  if (beep.isPlaying() && millis() - startedAt > BEEP_MS) beep.stop();
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    done();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    snooze(/*byUser=*/true);
    return;
  }
  // El gesto se arma cuando se sabe que el aparato NO está boca abajo: o
  // porque la primera muestra del IMU llegó y lo dice, o porque alguien lo
  // levantó (FaceUp). Hasta entonces, un "boca abajo" es la mesa, no una mano.
  if (!gestureArmed) {
    if (MOTION.take(MotionInput::Event::FaceUp)) {
      gestureArmed = true;
    } else if (MOTION.primed() && !MOTION.faceDown()) {
      gestureArmed = true;
    } else {
      // Se descarta el "boca abajo" que pudiera haber quedado pendiente de
      // antes de armar: si no, se consume recién al armarse y vale igual.
      MOTION.take(MotionInput::Event::FaceDown);
    }
  }
  // Y LO MISMO CON LA SACUDIDA, que no necesita armado pero sí ser de AHORA.
  // `checkMotionGestures()` sólo consume Shake mientras se graba, así que una
  // sacudida dada en Notas un segundo antes queda pendiente y esta pantalla la
  // tomaba en su primera pasada: alarma postergada sin que nadie reaccionara a
  // la alarma, y una de las tres gastada. Lo que pasó antes de que la pantalla
  // existiera no es una respuesta a la pantalla.
  if (!startupGesturesDropped) {
    startupGesturesDropped = true;
    MOTION.take(MotionInput::Event::Shake);
  }
  // Darlo vuelta es posponer sin buscar ningún botón: es el gesto de tapar el
  // despertador. Sacudirlo también lo pospone (es "pará"), que es lo primero
  // que hace cualquiera con un aparato que suena en la mano. La sacudida no
  // necesita armado: nadie sacude una mesa.
  if ((gestureArmed && MOTION.take(MotionInput::Event::FaceDown)) || MOTION.take(MotionInput::Event::Shake)) {
    LOG_INF(TAG, "gesto: se pospone el recordatorio");
    snooze(/*byUser=*/true);
    return;
  }
  // Se acabó el minuto y nadie contestó: se posterga sola. Dejarla en pantalla
  // sin postergar deja el recordatorio VENCIDO, y con un vencido el aparato se
  // despierta cada cinco segundos hasta quedarse sin batería.
  if (millis() - startedAt > BEEP_MS) {
    LOG_INF(TAG, "nadie contesto en %lu s", BEEP_MS / 1000);
    snooze(/*byUser=*/false);
    return;
  }
}

void ReminderAlertActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_REMINDERS));
  BaseTheme::drawIconBitmap(renderer, icon_hub_reminder_24, pageWidth / 2 - 12, pageHeight / 2 - 110);
  // Title wrapped by the paged-text helper is overkill: two truncated lines.
  const std::string line1 = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 60, line1.c_str(), true, EpdFontFamily::BOLD);
  if (!when.empty()) renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, when.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_REMINDER_SNOOZE), tr(STR_AGENDA_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
