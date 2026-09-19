#pragma once

// EL LOOP NO PUEDE QUEDARSE SORDO MIENTRAS LA RED ESPERA (ws397).
//
// Todas las llamadas de red de este firmware son SÍNCRONAS y salen del loop de
// Arduino, a propósito y desde siempre (el WiFi sólo está arriba adentro de una
// Activity de red, y el heap interno no da para volverlas asíncronas). El
// problema no es que tarden: es que MIENTRAS tardan no corre nadie. El log lo
// dijo solo:
//
//   [LOOP] New max loop duration: 90235 ms (activity: 90233 ms)   <- OTA entera
//   [LOOP] New max loop duration: 52152 ms                        <- OTA fallida
//   [LOOP] New max loop duration: 16377 ms                        <- 502 del servidor
//   [MOTION] golpe ... latcheado durante 34979 ms de loop ocupado: se descarta
//
// En esos 35, 52 o 90 segundos no corren POWER_KEY.pump(), ni los botones, ni
// MOTION, ni checkTimeAlarms(), ni devlog::tick(). Desde el vidrio es un
// aparato colgado: el dueño aprieta Atrás y PWR y no pasa nada, y peor, la
// pulsación de PWR que quedó latcheada en el PMIC se decodifica recién cuando
// el loop vuelve (o se descarta por vieja, ver STALE_EDGE_MS en PowerKey).
//
// La salida NO es volver la red asíncrona: es BOMBEAR en los lazos de espera
// que ya existen. El SDK ya ofrece el enganche justo (`AbortCallback` de
// `SecureHttpClient`, consultado en cada vuelta de readLine/readFixed/
// readChunked/readUntilClose, o sea cada 1-2 ms), igual que el panel ofrece
// `setBusyWaitSliceHook`. Esto es el punto único que esos lazos llaman.
//
// QUÉ HACE Y QUÉ NO. El bombeo hace lo mínimo que devuelve el aparato a la
// vida y nada más: decodifica PWR por el PMIC, mira el botón Atrás en crudo y
// alimenta el watchdog. NO pinta la pantalla (hay un candado de render y la
// regla del panel), NO duerme el aparato desde adentro de un lazo (eso lo
// decide main.cpp en un punto seguro, con la suelta de PWR que este bombeo
// dejó fresca) y NO vuelve a entrar a la red.
//
// El bombeo de verdad vive en src/util/NetPumpHooks.cpp, que es donde están
// POWER_KEY y BoardConfig; acá sólo queda el puntero. Header-only a propósito:
// lo incluye lib/ServerClient además de src/, y así no hay que pelear con el
// orden de enlace de las bibliotecas.

#include <Arduino.h>
#include <Logging.h>
#include <esp_err.h>
#include <esp_task_wdt.h>

#include <cstring>

namespace netpump {

// `prime` = primera llamada de una operación. Ahí NO se emite nada: se CEBA el
// estado de los botones, porque el que ya estaba apretado cuando la operación
// empezó no es un gesto (la misma lección que el "boca abajo" del IMU en
// 1.5.92: la posición en la que el aparato ya estaba nunca es un gesto). Sin
// esto, Atrás mantenido para sincronizar en el hub cancelaría la sincronización
// que acaba de pedir.
using SliceHook = void (*)(bool prime);

// Cada cuánto se bombea de verdad. Los lazos del SDK preguntan cada 1-2 ms;
// hacerles una lectura de I2C al PMIC a esa cadencia le sacaría ancho de banda
// a la descarga sin ganar nada. 25 ms es más rápido que cualquier dedo.
constexpr unsigned long TICK_MS = 25;

inline SliceHook g_hook = nullptr;
inline int g_depth = 0;
inline unsigned long g_startedAt = 0;
inline unsigned long g_lastTick = 0;
inline bool g_cancel = false;
inline const char* g_cancelWhy = nullptr;
inline uint32_t g_ticks = 0;         // cuántas veces se bombeó en esta pasada del loop
inline unsigned long g_passMs = 0;   // la operación de red más larga de esta pasada
inline char g_what[72] = {0};        // qué se está haciendo ahora
inline char g_passWhat[72] = {0};    // qué fue lo más largo de la pasada

inline void setSliceHook(const SliceHook hook) { g_hook = hook; }
inline bool inFlight() { return g_depth > 0; }
inline const char* what() { return g_what[0] ? g_what : "-"; }
inline unsigned long elapsedMs() { return g_depth > 0 ? millis() - g_startedAt : 0; }

// CANCELAR ES DE LA PASADA, NO DE LA LLAMADA. Una sincronización es una
// ristra de peticiones adentro de UNA sola pasada del loop (el paquete de
// noticias son decenas), así que si la bandera se limpiara al terminar cada
// petición, Atrás cancelaría una y la siguiente arrancaría igual. Se limpia en
// beginPass(), o sea cuando el loop volvió a estar vivo y la cancelación ya
// hizo su trabajo.
inline bool cancelRequested() { return g_cancel; }
inline const char* cancelReason() { return g_cancelWhy ? g_cancelWhy : "?"; }
inline void clearCancel() {
  g_cancel = false;
  g_cancelWhy = nullptr;
}

inline void requestCancel(const char* why) {
  if (g_cancel) return;
  g_cancel = true;
  g_cancelWhy = why;
  LOG_INF("NETPUMP", "CANCELAR por %s: «%s» llevaba %lu ms", why ? why : "?", what(), elapsedMs());
}

// Lo que los lazos de espera llaman. Barato: si no pasaron TICK_MS vuelve en
// el acto.
inline void tick() {
  if (!g_hook || g_depth <= 0) return;
  const unsigned long now = millis();
  if (now - g_lastTick < TICK_MS) return;
  g_lastTick = now;
  ++g_ticks;
  g_hook(false);
}

// Lo que va en el `AbortCallback` del SDK: bombea y contesta si hay que cortar.
inline bool pumpAndCheckCancel() {
  tick();
  return g_cancel;
}

// delay() que sigue atendiendo al usuario. Para las esperas propias (el
// backoff entre reintentos), no para las del socket.
inline void pumpDelay(const unsigned long ms) {
  const unsigned long until = millis() + ms;
  while (static_cast<int32_t>(millis() - until) < 0) {
    tick();
    if (g_cancel) return;
    delay(5);
  }
}

// Marca el trabajo de red en curso. Sólo la más externa manda: un anidamiento
// (una descarga adentro de una sincronización) no re-ceba los botones ni pisa
// la etiqueta.
class Scope {
 public:
  explicit Scope(const char* label) : outer_(g_depth == 0), startedAt_(millis()) {
    ++g_depth;
    if (!outer_) return;
    g_startedAt = startedAt_;
    g_lastTick = startedAt_;
    // La etiqueta se COPIA: casi siempre apunta a un std::string del llamador
    // (la ruta, la URL) que muere antes de que el resumen de la pasada se lea.
    if (label) {
      std::strncpy(g_what, label, sizeof(g_what) - 1);
      g_what[sizeof(g_what) - 1] = 0;
    } else {
      g_what[0] = 0;
    }
    if (g_hook) g_hook(true);
  }
  ~Scope() {
    --g_depth;
    if (!outer_) return;
    const unsigned long took = millis() - startedAt_;
    if (took > g_passMs) {
      g_passMs = took;
      std::strncpy(g_passWhat, g_what, sizeof(g_passWhat));
      g_passWhat[sizeof(g_passWhat) - 1] = 0;
    }
    g_what[0] = 0;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool outer_;
  unsigned long startedAt_;
};

// Una pasada del loop empieza: se olvida lo de la anterior y la cancelación ya
// cumplió (el loop está vivo otra vez).
inline void beginPass() {
  g_ticks = 0;
  g_passMs = 0;
  g_passWhat[0] = 0;
  clearCancel();
}

// Lo que main.cpp necesita para explicar una pasada larga sin que el dueño
// tenga que contarlo.
inline unsigned long passMs() { return g_passMs; }
inline const char* passWhat() { return g_passWhat[0] ? g_passWhat : "-"; }
inline uint32_t passTicks() { return g_ticks; }

}  // namespace netpump
