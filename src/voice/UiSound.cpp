#include "UiSound.h"

#include <Arduino.h>
#include <AudioManager.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "HubStore.h"
#include "util/WavHeader.h"
#include "music/MusicPlayer.h"
#include "TaskConfig.h"

namespace {

// Cuánto se queda el puerto tomado después del último clic. Volver a arrancar
// el códec en cada tecla mete ~20 ms de retardo y un ciclo del amplificador,
// así que mientras el usuario navega seguido se deja caliente; en cuanto para,
// se suelta para que la voz, la música o el micrófono puedan usarlo.
constexpr uint32_t IDLE_MS = 250;
constexpr size_t WAV_BYTES = wav::HEADER_BYTES + uisound::MAX_SAMPLES * sizeof(int16_t);

// POR QUE LOS CLICS DEJARON DE SONAR. Acá había un i2sPortFree() que le
// preguntaba al driver si se podía crear un canal, y con eso decidía si el clic
// entraba. Funcionó hasta que el puerto pasó a tener dueño explícito: stop() NO
// suelta los canales (sólo end() lo hace), así que en cuanto sonó el primer
// pitido o la primera frase de Piper, ESA instancia se quedó con I2S_NUM_0
// para siempre y el driver contestó "ocupado" de ahí en adelante. Resultado:
// después del primer sonido del sistema, ningún clic volvía a sonar nunca.
//
// Lo que hay que saber no es si el puerto está asignado —siempre lo está— sino
// si alguien lo está USANDO: Piper hablando, la música, el pitido del
// temporizador, el micrófono abierto. Si está quieto, ensureI2s() se lo pide al
// dueño y listo. Eso es AudioManager::portBusy().

}  // namespace

void UiSound::play(const uisound::Sound sound) {
  const uint8_t level = HUB_STORE.uiSoundMode;
  // Por qué no suena, una vez cada cinco segundos: "los sonidos no andan" puede
  // ser el ajuste apagado, la música tapándolos o el puerto ocupado, y desde el
  // aparato no hay forma de distinguirlos.
  static unsigned long lastWhyMs = 0;
  const auto why = [&](const char* reason) {
    const unsigned long now = millis();
    if (now - lastWhyMs < 5000) return;
    lastWhyMs = now;
    LOG_DBG("UISOUND", "sin sonido: %s (ajuste=%u)", reason, static_cast<unsigned>(level));
  };
  if (level == uisound::OFF || level > uisound::NORMAL) {
    why("apagado en Ajustes");
    return;
  }
  // "Cuando se reproduzca la música los demás sonidos no deben oírse, sólo la
  // música": con una canción puesta los clics se saltean sin más.
  if (MUSIC.isActive()) {
    why("hay música sonando");
    return;
  }
  if (!BoardConfig::hasAudio()) {
    why("la placa no tiene audio");
    return;
  }
  if (!ensureTask()) {
    why("no se pudo crear la tarea ui_sound");
    return;
  }

  int volume = HUB_STORE.musicVolume;
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;

  // Todo el pedido entra en la notificación (sonido, nivel y volumen), así la
  // tarea no toca HubStore ni hace falta un candado. Con eSetValueWithOverwrite
  // un clic que llega mientras suena el anterior lo reemplaza: es mejor perder
  // el del medio que encolar clics viejos que ya no dicen nada.
  const uint32_t msg = static_cast<uint32_t>(static_cast<uint8_t>(sound) + 1) | (static_cast<uint32_t>(level) << 8) |
                       (static_cast<uint32_t>(volume) << 16);
  xTaskNotify(static_cast<TaskHandle_t>(task_), msg, eSetValueWithOverwrite);
}

bool UiSound::ensureTask() {
  if (task_ != nullptr) return true;
  TaskHandle_t handle = nullptr;
  // Núcleo, prioridad y stack en src/TaskConfig.h (core 0 como la tarea de
  // reproducción del SDK, por debajo de su prioridad 10).
  if (xTaskCreatePinnedToCore(taskEntry, tasks::UI_SOUND_NAME, tasks::UI_SOUND_STACK, this, tasks::UI_SOUND_PRIO,
                              &handle, tasks::CORE_AUDIO) != pdPASS) {
    return false;
  }
  task_ = handle;
  return true;
}

void UiSound::taskEntry(void* self) {
  tasks::attach(tasks::Id::UiSound);
  static_cast<UiSound*>(self)->taskLoop();
}

void UiSound::taskLoop() {
  for (;;) {
    uint32_t msg = 0;
    // Con el puerto tomado se espera IDLE_MS y, si no llegó nada, se suelta;
    // con el puerto libre no hay nada que soltar y se duerme sin plazo.
    const TickType_t wait = hot_ ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY;
    if (xTaskNotifyWait(0, UINT32_MAX, &msg, wait) != pdTRUE) {
      release();
      continue;
    }
    const uint8_t id = msg & 0xFF;
    if (id == 0 || id > static_cast<uint8_t>(uisound::Sound::Error) + 1) continue;
    playNow(static_cast<uisound::Sound>(id - 1), (msg >> 8) & 0xFF, (msg >> 16) & 0xFF);
  }
}

bool UiSound::playNow(const uisound::Sound sound, const uint8_t level, const uint8_t volume) {
  // Otro tiene el puerto (Piper hablando, música, el pitido del temporizador):
  // el clic se saltea y listo. Nunca corta lo que está sonando ni espera a que
  // termine — un clic que llega tarde es peor que un clic que no suena.
  if (!hot_ && AudioManager::portBusy()) {
    LOG_DBG("UISOUND", "sin sonido: el I2S lo está usando otro");
    return false;
  }

  if (wav_ == nullptr) {
    wav_ = static_cast<uint8_t*>(heap_caps_malloc(WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (wav_ == nullptr) wav_ = static_cast<uint8_t*>(malloc(WAV_BYTES));
    if (wav_ == nullptr) return false;
  }
  const size_t samples = uisound::render(sound, uisound::gainFor(level),
                                         reinterpret_cast<int16_t*>(wav_ + wav::HEADER_BYTES), uisound::MAX_SAMPLES);
  if (samples == 0) return false;
  wav::writeHeader(wav_, uisound::RATE, samples * sizeof(int16_t));

  if (audio_ == nullptr) audio_ = new AudioManager();
  if (audio_ == nullptr) return false;
  // Cada etapa deja rastro: "los sonidos no suenan" ya se diagnosticó dos veces
  // leyendo el código en vez del log, y las dos veces se arregló otra cosa.
  static unsigned long lastTraceMs = 0;
  const bool trace = millis() - lastTraceMs >= 5000;
  if (trace) lastTraceMs = millis();
  if (!audio_->begin()) {
    LOG_ERR("UISOUND", "sin sonido: el códec no arrancó (begin)");
    release();
    return false;
  }
  hot_ = true;
  // Mismo volumen que el resto del aparato: lo bajo de estos sonidos ya está en
  // la muestra (gainFor), no en el registro del códec, que es compartido.
  audio_->setVolume(volume);
  const size_t bytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  if (!audio_->playBuffer(wav_, bytes, false)) {
    LOG_ERR("UISOUND", "sin sonido: playBuffer falló (%u bytes, %u Hz)", static_cast<unsigned>(bytes),
            static_cast<unsigned>(uisound::RATE));
    release();
    return false;
  }
  // playBuffer vuelve enseguida (el SDK reproduce en su propia tarea). Esperar
  // acá a que termine es lo que evita que el clic siguiente llame a play() en
  // medio del anterior, que lo cortaría y haría sonar el amplificador.
  int waited = 0;
  for (; waited < 120 && audio_->isPlaying(); ++waited) vTaskDelay(pdMS_TO_TICKS(5));
  if (trace) {
    LOG_INF("UISOUND", "sonó %d: %u muestras, nivel %u, vol %u, %d ms de reproducción", static_cast<int>(sound),
            static_cast<unsigned>(samples), static_cast<unsigned>(level), static_cast<unsigned>(volume), waited * 5);
  }
  return true;
}

void UiSound::release() {
  hot_ = false;
  // end() apaga el amplificador y el riel del códec y borra los canales I2S:
  // sin esto el puerto queda tomado y la voz o el micrófono no arrancan.
  if (audio_ != nullptr) audio_->end();
  if (wav_ != nullptr) {
    heap_caps_free(wav_);
    wav_ = nullptr;
  }
}
