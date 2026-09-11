#include "TaskConfig.h"

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
  const BaseType_t ok =
      xTaskCreatePinnedToCore(boundedEntry, name, stackBytes, &job, prio, &handle, core);
  if (ok != pdPASS) {
    vSemaphoreDelete(job.done);
    LOG_ERR(TAG, "%s: no se pudo crear el worker de %u B", name, (unsigned)stackBytes);
    return false;
  }
  // Se espera sin tope: el worker siempre termina (el trabajo es acotado) y un
  // tope acá sólo serviría para seguir con el stack ajeno todavía vivo.
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
  if (usedOut) *usedOut = job.used;
  LOG_INF(TAG, "%s: usó %u B de %u declarados", name, (unsigned)job.used, (unsigned)stackBytes);
  return true;
}

}  // namespace tasks
