#include "NetPumpHooks.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <NetPump.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "DeviceLog.h"
#include "PowerKey.h"
#include "TaskWatchdog.h"

namespace {
constexpr const char* TAG = "NETPUMP";

TaskHandle_t s_loopTask = nullptr;

// Antirrebote del botón Atrás, de dos muestras, igual que el del InputManager
// del SDK. Acá se lee el pin EN CRUDO y no se toca el InputManager a propósito:
// sus flancos los calcula update() y los consume la Activity de turno una vez
// por pasada; si el bombeo llamara a update() se comería la pulsación que la
// pantalla espera. Leyendo el pin no le sacamos nada a nadie: la misma
// pulsación cancela la red acá y, si el dedo sigue abajo cuando el loop vuelve
// (lo normal cuando uno aprieta un aparato que parece muerto), la pantalla la
// ve igual y hace lo suyo — que es justamente "volver a la pantalla anterior".
bool s_backCommitted = false;
bool s_backLastRaw = false;
bool s_pwrWasDown = false;

bool backDown() {
  const int8_t pin = BoardConfig::ACTIVE.input.back;
  if (pin < 0) return false;
  // Los cuatro botones de esta placa son activos en bajo con pull-up; el
  // InputManager ya dejó el pin configurado en su begin().
  return digitalRead(pin) == LOW;
}

void slice(const bool prime) {
  // El watchdog hay que alimentarlo venga de donde venga la llamada: una
  // descarga desde otra tarea también puede pasarse del plazo.
  resetTaskWatchdogIfSubscribed();

  // Lo demás es I2C al PMIC y GPIO de los botones: sólo desde el loop. Si
  // alguna vez una tarea propia hace red, que no se meta con el decodificador
  // de PWR por debajo del loop.
  if (s_loopTask == nullptr || xTaskGetCurrentTaskHandle() != s_loopTask) return;
  if (!BoardConfig::isWS397()) return;

  // PWR es del PMIC y se decodifica por I2C desde el loop: si el loop no corre,
  // el flanco se queda latcheado y cuando vuelve ya es viejo (STALE_EDGE_MS).
  // Bombearlo acá es lo que hace que apretar PWR en el medio de una descarga
  // valga como lo que es.
  POWER_KEY.pump();
  const bool pwr = POWER_KEY.pressed();
  const bool back = backDown();

  if (prime) {
    s_pwrWasDown = pwr;
    s_backCommitted = back;
    s_backLastRaw = back;
    return;
  }

  // PWR: el flanco de bajada corta el trabajo de red. La suspensión NO se hace
  // desde acá (ver la cabecera de NetPump.h): la suelta queda latcheada en
  // POWER_KEY, fresca porque este mismo bombeo la decodificó a tiempo, y la
  // atiende handlePowerHold() en la pasada siguiente, que es el único lugar
  // donde el aparato está en un estado consistente para dormir. También vale
  // una suelta pendiente: un toque corto entero puede caber entre dos bombeos.
  if ((pwr && !s_pwrWasDown) || POWER_KEY.releasePending()) netpump::requestCancel("PWR");
  s_pwrWasDown = pwr;

  if (back == s_backLastRaw && back != s_backCommitted) {
    s_backCommitted = back;
    if (s_backCommitted) netpump::requestCancel("Atrás");
  }
  s_backLastRaw = back;

  // Que lo escrito durante la espera llegue a la tarjeta. Sin esto, una
  // descarga de 90 s se lleva consigo todo lo que se logueó adentro: el flush
  // por quietud de devlog nunca corre porque el loop no corre, y si el aparato
  // se cae ahí el final del log no existe (la lección de 1.5.99). `tick()` ya
  // se protege sola: no hace nada si hay una escritura en curso o si no pasó
  // el plazo de quietud.
  devlog::tick();
}
}  // namespace

void netpumphooks::begin() {
  s_loopTask = xTaskGetCurrentTaskHandle();
  netpump::setSliceHook(&slice);
  LOG_INF(TAG, "bombeo de red instalado (cada %lu ms: PWR, Atrás y el watchdog)",
          static_cast<unsigned long>(netpump::TICK_MS));
}
