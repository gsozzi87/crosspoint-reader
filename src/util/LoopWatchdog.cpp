#include "LoopWatchdog.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <DrawScope.h>
#include <Logging.h>
#include <NetPump.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "../TaskConfig.h"

namespace {
constexpr const char* TAG = "LOOPWDT";
constexpr uint32_t CHECK_MS = 1000;
// Tope de reinicios por cuelgue. Un supervisor sin tope puede convertir un
// defecto que aparece en el arranque en un bucle de reinicios, que para el
// usuario es peor que un aparato lento: no hay forma de llegar a Ajustes ni a
// la OTA que lo arreglaría. A la cuarta se deja de reiniciar y sólo se anota.
constexpr uint32_t MAX_TRIPS = 3;
// Si el loop late sin problemas este rato, lo de antes fue un incidente y no un
// arranque roto: se olvida la racha para no llegar al tope con cuelgues
// separados por semanas.
constexpr uint32_t FORGET_AFTER_MS = 5 * 60 * 1000;

constexpr uint32_t TRIP_MAGIC = 0x4C575447u;  // "LWTG"
RTC_NOINIT_ATTR uint32_t tripMagic;
RTC_NOINIT_ATTR uint32_t tripCount;
RTC_NOINIT_ATTR uint32_t tripElapsedMs;
RTC_NOINIT_ATTR uint32_t tripRestarted;  // 1 = se reinició; 0 = se llegó al tope y se siguió
// Hasta qué número de cuelgue se contó ya en el log. Sin esto, el aviso del
// arranque volvía a salir en CADA arranque posterior —un reinicio pedido por el
// usuario incluido— y decía que a ése también lo había forzado el supervisor.
RTC_NOINIT_ATTR uint32_t tripReported;
RTC_NOINIT_ATTR char tripWhere[72];

volatile uint32_t lastBeat = 0;
volatile uint32_t pauseDepth = 1;  // el arranque entero cuenta como pausa
volatile bool armed = false;
bool reported = false;
uint32_t tripsAtBoot = 0;

bool recordValid() { return tripMagic == TRIP_MAGIC && tripCount > 0 && tripCount <= MAX_TRIPS + 1; }

// Este arranque SALIÓ de un cuelgue (y no de uno viejo ya contado).
bool freshTrip() { return recordValid() && tripReported != tripCount; }

void writeRecord(const uint32_t elapsed, const bool restarted) {
  const char* screen = gfxscope::activity();
  if (netpump::inFlight()) {
    snprintf(tripWhere, sizeof(tripWhere), "%s / red «%s»", screen ? screen : "?", netpump::what());
  } else {
    snprintf(tripWhere, sizeof(tripWhere), "%s", screen ? screen : "?");
  }
  tripElapsedMs = elapsed;
  tripRestarted = restarted ? 1u : 0u;
  // Acotado arriba: si el contador pudiera crecer sin límite terminaría
  // saliéndose del rango que `recordValid()` acepta, la racha se leería como
  // basura y el supervisor volvería a reiniciar como si fuera la primera vez.
  tripCount = recordValid() ? (tripCount < MAX_TRIPS + 1 ? tripCount + 1 : tripCount) : 1;
  tripMagic = TRIP_MAGIC;
  tripReported = 0;
}

void supervisor(void*) {
  tasks::attach(tasks::Id::LoopWatch);
  bool tripped = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
    const uint32_t now = millis();

    // Pausado a propósito (reposo, sueño, apagado): el latido no envejece. El
    // chip entero está detenido durante el light sleep y `millis()` no lo está,
    // así que sin esto una noche de reposo se leería como un cuelgue de horas.
    if (pauseDepth > 0 || !armed) {
      lastBeat = now;
      continue;
    }

    if (!tripped && tripsAtBoot > 0 && now >= FORGET_AFTER_MS && recordValid()) {
      tripMagic = 0;
      tripCount = 0;
      tripReported = 0;
      tripsAtBoot = 0;
    }

    const uint32_t quiet = now - lastBeat;
    if (quiet < loopwdt::BUDGET_MS) continue;

    // Nada de LOG_* ni de tarjeta acá: si el loop se colgó teniendo el mutex
    // del almacenamiento, escribir dejaría también al supervisor esperando y no
    // quedaría nadie para reiniciar. Al puerto serie sí, que no toma candados,
    // y a la RAM del RTC, que es lo que lee el arranque siguiente.
    const bool willRestart = !recordValid() || tripCount < MAX_TRIPS;
    writeRecord(quiet, willRestart);
    log_e("[LOOPWDT] el loop lleva %u ms sin latir en «%s»: %s", static_cast<unsigned>(quiet), tripWhere,
          willRestart ? "se reinicia" : "ya van demasiados, se deja como está");
    if (!willRestart) {
      tripped = true;
      armed = false;  // no reiniciar en bucle: lo que queda es el log del próximo arranque
      continue;
    }
    esp_restart();
  }
}
}  // namespace

void loopwdt::begin() {
  if (!BoardConfig::isWS397()) return;
  if (armed) return;
  tripsAtBoot = recordValid() ? tripCount : 0;
  lastBeat = millis();
  armed = true;
  if (pauseDepth > 0) pauseDepth = 0;  // el arranque terminó
  const tasks::Budget& b = tasks::budget(tasks::Id::LoopWatch);
  if (xTaskCreatePinnedToCore(&supervisor, b.name, b.stack, nullptr, b.prio, nullptr, b.core) != pdPASS) {
    armed = false;
    LOG_ERR(TAG, "no se pudo crear el supervisor del loop: un cuelgue queda sin salida automática");
    return;
  }
  LOG_INF(TAG, "supervisor del loop armado (%lu s sin latido = reinicio)",
          static_cast<unsigned long>(loopwdt::BUDGET_MS / 1000));
}

void loopwdt::beat() { lastBeat = millis(); }

void loopwdt::pause(const char* why) {
  (void)why;
  ++pauseDepth;
  lastBeat = millis();
}

void loopwdt::resume() {
  if (pauseDepth > 0) --pauseDepth;
  lastBeat = millis();
}

bool loopwdt::trippedLastBoot() { return freshTrip(); }

void loopwdt::reportBoot() {
  if (reported || !freshTrip()) return;
  reported = true;
  tripReported = tripCount;
  tripWhere[sizeof(tripWhere) - 1] = '\0';  // RTC_NOINIT: nunca confiar en que venga terminado
  LOG_ERR(TAG,
          "!!! el arranque anterior lo forzó el supervisor: el loop estuvo %lu ms sin latir en «%s» "
          "(van %lu cuelgues seguidos%s)",
          static_cast<unsigned long>(tripElapsedMs), tripWhere, static_cast<unsigned long>(tripCount),
          tripRestarted ? "" : "; ya no se reinicia por esto");
}
