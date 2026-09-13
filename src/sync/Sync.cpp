#include "Sync.h"

#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <cstring>

#include "../HubStore.h"
#include "../activities/ActivityManager.h"
#include "../activities/home/HubSyncActivity.h"
#include "../news/NewsPack.h"

extern ActivityManager activityManager;

namespace {
constexpr const char* TAG = "SYNC";
unsigned long lastAt = 0;  // millis() de la última, 0 = nunca en esta sesión
bool ran = false;

// Pantallas que guardan ÍNDICES dentro de HubStore (`sectionIndex`,
// `itemIndex`, el recordatorio que están mostrando). Bajar el hub debajo de
// ellas reemplaza los vectores y el próximo repintado lee fuera de rango: no es
// una pantalla fea, es un cuelgue. Con estas en frente se sube y se bajan las
// noticias —que son archivos y no los referencia nadie— y el hub espera.
//
// Igual no se pierde nada: en esas pantallas los datos del hub ya están
// frescos, porque el hub sincroniza al entrar.
bool holdsHubState(const char* name) {
  if (name == nullptr) return true;  // sin saber quién está, lo prudente
  // Los nombres son los que pasa cada Activity a su constructor. Ojo: tienen
  // que existir, o la guardia queda de adorno. Los que de verdad guardan
  // índices son Agenda (sectionIndex/itemIndex sobre `lists` y `reminders`) y
  // Notes; Hub, Timer y ReminderAlert van por las dudas, que sale gratis.
  static const char* const NAMES[] = {"Hub", "Agenda", "Notes", "Timer", "ReminderAlert"};
  for (const char* n : NAMES) {
    if (strcmp(name, n) == 0) return true;
  }
  return false;
}
}  // namespace

void devicesync::markFresh() {
  lastAt = millis();
  ran = true;
}

bool devicesync::ifDue(const unsigned long idleMs) {
  if (WiFi.status() != WL_CONNECTED) return false;
  // Nunca justo después de que alguien apretó algo: esto bloquea el loop unos
  // segundos, y hacerlo encima de una pulsación se siente como que el aparato
  // se colgó. Se espera a que la pantalla esté quieta.
  if (idleMs < QUIET_MS) return false;
  if (ran && millis() - lastAt < static_cast<unsigned long>(MIN_GAP_MIN) * 60UL * 1000UL) return false;

  // Marcar ANTES de empezar: si algo de acá adentro falla o tarda, no se
  // reintenta en la pasada siguiente del loop.
  markFresh();

  // El orden es el de F03: primero SUBIR lo pendiente y recién después bajar, o
  // la instantánea que queda guardada es la de antes de aplicar la cola.
  const int subidos = SERVER_CLIENT.flushQueue();
  const int noticias = newspack::sync(/*budget=*/4);

  bool hub = false;
  const char* quien = activityManager.currentActivityName();
  if (!holdsHubState(quien)) {
    hub = HubSyncActivity::fetchNow();
    if (hub) HUB_STORE.saveToFile();
  }
  LOG_INF(TAG, "red arriba en %s: %d subidos, %d noticias, hub %s", quien ? quien : "?", subidos, noticias,
          hub ? "al día" : (holdsHubState(quien) ? "se deja para después" : "no se pudo"));
  return true;
}
