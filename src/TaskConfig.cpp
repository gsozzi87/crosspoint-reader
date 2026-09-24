#include "TaskConfig.h"

#include "util/LoopWatchdog.h"

namespace tasks {
namespace {
constexpr const char* TAG = "TASK";

struct BoundedJob {
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
  SemaphoreHandle_t done = nullptr;
  uint32_t stackBytes = 0;
  uint32_t used = 0;
};

void boundedEntry(void* param) {
  auto* job = static_cast<BoundedJob*>(param);
  attach(Id::Worker);
  job->fn(job->arg);
  // Medir ANTES de salir: después de vTaskDelete el handle no sirve para nada.
  const uint32_t freeBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
  job->used = job->stackBytes > freeBytes ? job->stackBytes - freeBytes : 0;
  detach(Id::Worker);
  xSemaphoreGive(job->done);
  vTaskDelete(nullptr);
}
}  // namespace

bool runBounded(const char* name, const uint32_t stackBytes, void (*fn)(void*), void* arg, uint32_t* usedOut,
                const UBaseType_t prio, const BaseType_t core) {
  if (!fn) return false;
  BoundedJob job;
  job.fn = fn;
  job.arg = arg;
  job.stackBytes = stackBytes;
  job.done = xSemaphoreCreateBinary();
  if (!job.done) return false;

  TaskHandle_t handle = nullptr;
  const BaseType_t ok = xTaskCreatePinnedToCore(boundedEntry, name, stackBytes, &job, prio, &handle, core);
  if (ok != pdPASS) {
    vSemaphoreDelete(job.done);
    LOG_ERR(TAG, "%s: no se pudo crear el worker de %u B", name, (unsigned)stackBytes);
    return false;
  }
  // REV-089: EL PLAZO, y por qué al vencerse se reinicia en vez de volver.
  //
  // Acá decía "el worker siempre termina (el trabajo es acotado)", y el
  // contrato no lo garantiza: la guardia de instrucciones de Lua no puede
  // interrumpir una función de C que no vuelve, y `cp.*` entra a Storage y a
  // otras rutas nativas. Con `portMAX_DELAY` el que se colgaba no era la app:
  // era el llamador —el loop de Arduino, o la tarea de RENDER con el
  // `RenderLock` tomado, que deja la pantalla muerta para siempre.
  //
  // Y no se puede "vencer y devolver false": el `BoundedJob` de arriba vive en
  // ESTE stack y el worker lo sigue usando; volver sería dejarle una referencia
  // a un marco que ya no existe. `vTaskDelete()` tampoco: abandonaría los
  // candados que el worker tenga tomados. La única salida honesta es dejar el
  // motivo anotado y reiniciar.
  if (xSemaphoreTake(job.done, pdMS_TO_TICKS(WORKER_DEADLINE_MS)) != pdTRUE) {
    loopwdt::workerStalled(name, WORKER_DEADLINE_MS);  // no vuelve
  }
  vSemaphoreDelete(job.done);
  if (usedOut) *usedOut = job.used;
  LOG_INF(TAG, "%s: usó %u B de %u declarados", name, (unsigned)job.used, (unsigned)stackBytes);
  return true;
}

}  // namespace tasks
