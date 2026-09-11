#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Sonidos de la interfaz: clics muy cortos (26-74 ms) y a bajo volumen para
// navegar, elegir, volver, pasar página y avisar de un error.
//
// Se sintetizan acá mismo, sample a sample: un WAV bajado del servidor sería
// flash (o tarjeta) y arranque para algo que son treinta líneas de seno y
// envolvente. Todos llevan una rampa de 3 ms al principio y al final, que es lo
// que evita el *pop* del parlante al arrancar y cortar la muestra en seco.
//
// Reglas de convivencia con el resto del audio (el I2S es UNO SOLO):
//   * si otro (la voz de Piper, la música, el pitido del temporizador) tiene el
//     puerto tomado, el clic NO suena y NO espera: se saltea en silencio;
//   * play() no bloquea: le avisa a una tarea propia y vuelve, así pasar página
//     no se frena por el sonido.
//
// La parte de síntesis (namespace uisound) no depende de Arduino ni del SDK a
// propósito: el mismo render() se compila en el escritorio para poder escuchar
// los WAV antes de subirlos al aparato.

namespace freeink {
class AudioManager;
}

namespace uisound {

enum class Sound : uint8_t {
  Nav = 0,   // moverse por una lista o un menú: el más suave, se oye a cada rato
  Select,    // OK / confirmar
  Back,      // atrás / cancelar
  Page,      // pasar página en el lector
  Error,     // algo salió mal: más grave que los demás
};

// 48 kHz: el clic es un transitorio corto y a esta tasa sale limpio.
//
// OJO: hasta 1.5.64 el silencio con el que el SDK ceba la línea antes y después
// de levantar el amplificador se contaba en BUFFERS, no en milisegundos, así que
// a 48 kHz duraba un tercio que a 16 kHz: el amplificador clase D recién estaba
// arrancando cuando el clic de 26 ms ya había terminado. Eso, más que el clic
// salía a -21 dBFS (la mitad de la mitad), es por qué "los sonidos del sistema
// están activados y no se oyen". El silencio ahora va en milisegundos
// (AMP_PRIME_MS / AMP_SETTLE_MS en AudioManager.cpp) y esta tasa ya no cambia
// nada del arranque.
constexpr uint32_t RATE = 48000;
constexpr uint32_t MAX_MS = 80;
constexpr size_t MAX_SAMPLES = RATE / 1000 * MAX_MS;

// Ajuste "Sonidos de la interfaz": apagados / suaves / normales.
enum Level : uint8_t { OFF = 0, SOFT = 1, NORMAL = 2 };
// Referencia: el pitido del recordatorio (AlertBeep) sale a 12000/32768 = 0,37
// de escala completa y ESO sí se oye. Con 0,30 de ganancia sobre picos de 0,30
// el clic de navegación quedaba en 0,09 — doce decibeles por debajo del pitido y
// además cinco veces más corto. "Normal" ahora deja los clics en el entorno del
// pitido y "suave" a la mitad.
inline float gainFor(const uint8_t level) { return level == NORMAL ? 1.0f : level == SOFT ? 0.45f : 0.0f; }

// Escribe el clic en `out` (16 bits, mono, RATE) y devuelve cuántas muestras
// escribió. `gain` es el factor propio de estos sonidos (gainFor), aparte del
// volumen del aparato: por eso quedan bastante por debajo de la música.
inline size_t render(const Sound sound, float gain, int16_t* out, const size_t maxSamples) {
  if (out == nullptr || maxSamples == 0) return 0;
  if (gain <= 0.0f) return 0;
  if (gain > 1.0f) gain = 1.0f;

  // Duración y pico de cada uno, relativos a la escala completa. Con la
  // ganancia en "normal" el más fuerte (error, 0,55) queda apenas por encima del
  // pitido del recordatorio y el de navegación (0,32) justo por debajo.
  float durS = 0.030f;
  float peak = 0.30f;
  switch (sound) {
    case Sound::Nav:
      durS = 0.038f;  // 26 ms era menos que lo que tarda el amplificador en arrancar
      peak = 0.32f;
      break;
    case Sound::Select:
      durS = 0.070f;
      peak = 0.45f;
      break;
    case Sound::Back:
      durS = 0.052f;
      peak = 0.40f;
      break;
    case Sound::Page:
      durS = 0.050f;
      peak = 0.45f;
      break;
    case Sound::Error:
      durS = 0.074f;
      peak = 0.55f;
      break;
  }

  size_t n = static_cast<size_t>(durS * RATE);
  if (n > maxSamples) n = maxSamples;
  if (n == 0) return 0;

  const float dt = 1.0f / RATE;
  const float total = n * dt;
  const float ramp = 0.003f;  // rampa antichasquido, entrada y salida
  const float twoPi = 6.2831853f;

  float phase = 0.0f;  // la fase sigue de largo cuando cambia el tono: sin saltos
  float lowpass = 0.0f;
  uint32_t noise = 0x1234ABCDu;  // ruido siempre igual, para que el clic no cambie

  for (size_t i = 0; i < n; ++i) {
    const float t = i * dt;

    // Envolvente propia de cada sonido.
    float env = 0.0f;
    float freq = 0.0f;
    switch (sound) {
      case Sound::Nav:
        freq = 2000.0f;
        env = std::exp(-t / 0.010f);
        break;
      case Sound::Select:  // dos tonos que suben: algo se eligio
        freq = t < 0.030f ? 1400.0f : 2100.0f;
        env = std::exp(-t / 0.050f);
        break;
      case Sound::Back:  // dos tonos que bajan: "me vuelvo"
        freq = t < 0.022f ? 1600.0f : 1050.0f;
        env = std::exp(-t / 0.040f);
        break;
      case Sound::Page:  // ruido apagado: el roce del papel, no un pitido
        env = t < 0.012f ? t / 0.012f : std::exp(-(t - 0.012f) / 0.018f);
        break;
      case Sound::Error:  // dos golpes graves y separados
        freq = t < 0.036f ? 520.0f : 390.0f;
        env = (t < 0.030f) ? 1.0f : (t < 0.042f ? 0.0f : std::exp(-(t - 0.042f) / 0.040f));
        // los bordes del corte también van con rampa, si no chasquea
        if (t > 0.027f && t < 0.030f) env = (0.030f - t) / 0.003f;
        if (t >= 0.042f && t < 0.045f) env *= (t - 0.042f) / 0.003f;
        break;
    }

    float v;
    if (sound == Sound::Page) {
      // Ruido blanco (LFSR) pasado por un filtro de un polo: sin el filtro es
      // un "chhh" de radio mal sintonizada, con él es una hoja.
      noise ^= noise << 13;
      noise ^= noise >> 17;
      noise ^= noise << 5;
      const float white = static_cast<float>(static_cast<int32_t>(noise >> 8) - 0x800000) / 8388608.0f;
      lowpass += 0.22f * (white - lowpass);
      v = lowpass * 2.1f;  // el filtro se come el nivel; esto lo devuelve
    } else {
      phase += twoPi * freq * dt;
      if (phase > twoPi) phase -= twoPi;
      v = std::sin(phase);
    }

    // Rampas de entrada y salida sobre todo lo anterior.
    float shape = env;
    if (t < ramp) shape *= t / ramp;
    const float left = total - t;
    if (left < ramp) shape *= left / ramp;

    float s = v * shape * peak * gain;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    out[i] = static_cast<int16_t>(s * 32000.0f);
  }
  return n;
}

}  // namespace uisound

// Singleton: hay un solo parlante, así que hay un solo emisor de clics.
class UiSound {
 public:
  static UiSound& getInstance() {
    static UiSound instance;
    return instance;
  }

  // Dispara el clic y vuelve enseguida. No suena si el ajuste está apagado, si
  // la placa no tiene audio o si el puerto I2S lo tiene otro.
  void play(uisound::Sound sound);

 private:
  UiSound() = default;
  ~UiSound() = default;
  UiSound(const UiSound&) = delete;
  UiSound& operator=(const UiSound&) = delete;

  static void taskEntry(void* self);
  void taskLoop();
  bool ensureTask();
  bool playNow(uisound::Sound sound, uint8_t level, uint8_t volume);
  void release();

  void* task_ = nullptr;                  // TaskHandle_t, sin arrastrar FreeRTOS al header
  freeink::AudioManager* audio_ = nullptr;
  uint8_t* wav_ = nullptr;
  bool hot_ = false;  // tenemos el puerto tomado (se suelta solo, ver IDLE_MS)
};

#define UI_SOUND UiSound::getInstance()
