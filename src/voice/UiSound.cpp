#include "UiSound.h"

#include <Arduino.h>
#include <AudioManager.h>
#include <BoardConfig.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "HubStore.h"
#include "util/WavHeader.h"

namespace {

// Cuánto se queda el puerto tomado después del último clic. Volver a arrancar
// el códec en cada tecla mete ~20 ms de retardo y un ciclo del amplificador,
// así que mientras el usuario navega seguido se deja caliente; en cuanto para,
// se suelta para que la voz, la música o el micrófono puedan usarlo.
constexpr uint32_t IDLE_MS = 250;
constexpr size_t WAV_BYTES = wav::HEADER_BYTES + uisound::MAX_SAMPLES * sizeof(int16_t);

// ¿Está libre el I2S? El SDK le da un AudioManager propio a cada clase (voz,
// música, pitido, micrófono) y el puerto es uno solo: si alguno lo tiene
// abierto, i2s_new_channel falla. Preguntarlo así no toca el códec — llamar a
// begin() para enterarse le bajaría el volumen a la música y apagaría el
// amplificador en medio de una frase.
bool i2sPortFree() {
  i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_chan_handle_t tx = nullptr;
  if (i2s_new_channel(&cfg, &tx, nullptr) != ESP_OK) return false;
  i2s_del_channel(tx);
  return true;
}

}  // namespace

void UiSound::play(const uisound::Sound sound) {
  const uint8_t level = HUB_STORE.uiSoundMode;
  if (level == uisound::OFF || level > uisound::NORMAL) return;
  if (!BoardConfig::hasAudio()) return;
  if (!ensureTask()) return;

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
  // Core 0, como la tarea de reproducción del SDK: el loop de Arduino (la UI)
  // vive en el 1 y no se lo frena. Prioridad por debajo de la de audio (10).
  if (xTaskCreatePinnedToCore(taskEntry, "ui_sound", 4096, this, 4, &handle, 0) != pdPASS) return false;
  task_ = handle;
  return true;
}

void UiSound::taskEntry(void* self) { static_cast<UiSound*>(self)->taskLoop(); }

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
  if (!hot_ && !i2sPortFree()) return false;

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
  if (!audio_->begin()) {
    release();
    return false;
  }
  hot_ = true;
  // Mismo volumen que el resto del aparato: lo bajo de estos sonidos ya está en
  // la muestra (gainFor), no en el registro del códec, que es compartido.
  audio_->setVolume(volume);
  if (!audio_->playBuffer(wav_, wav::HEADER_BYTES + samples * sizeof(int16_t), false)) {
    release();
    return false;
  }
  // playBuffer vuelve enseguida (el SDK reproduce en su propia tarea). Esperar
  // acá a que termine es lo que evita que el clic siguiente llame a play() en
  // medio del anterior, que lo cortaría y haría sonar el amplificador.
  for (int i = 0; i < 120 && audio_->isPlaying(); ++i) vTaskDelay(pdMS_TO_TICKS(5));
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
