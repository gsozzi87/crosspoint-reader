#include "VoiceRecorder.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Adpcm.h"
#include "util/WavHeader.h"
#include "music/MusicPlayer.h"
#include "HubStore.h"

namespace {
constexpr const char* TAG = "VOICE";
constexpr size_t BLOCK = 512;  // 32 ms at 16 kHz

// Pitido de apertura y cierre.
constexpr uint16_t TONE_OPEN_HZ = 1200;
constexpr uint16_t TONE_OPEN_MS = 90;
constexpr uint16_t TONE_CLOSE_HZ = 700;
constexpr uint16_t TONE_CLOSE_MS = 110;
// Descriptores DMA del RX que ya venían llenándose cuando arrancó el tono.
constexpr unsigned long RX_PREFILL_MS = 64;
// Cola del parlante/recinto después del tono.
constexpr unsigned long TONE_TAIL_MS = 40;
// Piso del recorte: 50 ms. Sin pitido igual hay que saltear el arranque del
// códec, y la cabecera RIFF reescrita necesita 22 muestras de lugar.
constexpr size_t PREROLL_MIN_SAMPLES = 800;
// hasSpeech(): ventanas de 15 ms.
constexpr size_t SPEECH_WINDOW = 240;
constexpr int16_t SPEECH_FLOOR = 700;

// Seno con 5 ms de entrada y de salida, para que el pitido no chasquee.
void fillTone(uint8_t* buf, size_t samples, uint16_t hz) {
  int16_t* pcm = reinterpret_cast<int16_t*>(buf + wav::HEADER_BYTES);
  const size_t edge = VoiceRecorder::SAMPLE_RATE / 200;
  for (size_t i = 0; i < samples; ++i) {
    float env = 1.0f;
    if (i < edge) env = i / static_cast<float>(edge);
    else if (samples - i < edge) env = (samples - i) / static_cast<float>(edge);
    pcm[i] = static_cast<int16_t>(9000 * env * std::sin(2 * M_PI * hz * i / VoiceRecorder::SAMPLE_RATE));
  }
}
}  // namespace

int VoiceRecorder::s_open = 0;
VoiceRecorder* VoiceRecorder::s_live[4] = {nullptr, nullptr, nullptr, nullptr};

void VoiceRecorder::registerLive() {
  for (VoiceRecorder*& slot : s_live) {
    if (!slot) {
      slot = this;
      return;
    }
  }
}

void VoiceRecorder::unregisterLive() {
  for (VoiceRecorder*& slot : s_live) {
    if (slot == this) slot = nullptr;
  }
}

void VoiceRecorder::abortAll() {
  for (VoiceRecorder* r : s_live) {
    if (r) r->abort();
  }
}

bool VoiceRecorder::start(StrId& why) {
  if (!BoardConfig::hasCodecMic()) {
    why = StrId::STR_ASK_NO_MIC;
    return false;
  }
  // El puerto I2S es uno solo: con música puesta beginCapture() falla y el
  // usuario veía "Falló la captura del micrófono".
  MUSIC.stop();
  release();
  const size_t bytes = wav::HEADER_BYTES + maxSamples * sizeof(int16_t);
  buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!buffer) {
    why = StrId::STR_AUDIO_NO_MEMORY;
    return false;
  }
  recorded = 0;
  // Los dos caminos de error liberan la toma: son cientos de KB de PSRAM
  // (VoiceRecorder{12} pide 384 KB) y quedaban vivos hasta el próximo start().
  if (!audio.begin()) {
    release();
    why = StrId::STR_AUDIO_CAPTURE_FAILED;
    return false;
  }
  // El micrófono abre ANTES del pitido, no después: el códec es full duplex y
  // el tono viaja por el mismo AudioManager sin esperar a que termine. Antes se
  // tocaba el pitido de corrido (240 ms bloqueando) y recién ahí abría la
  // captura, así que la primera palabra —que el usuario dice apenas escucha el
  // tono— no entraba en la toma. Lo que sí entra ahora es el propio tono, y eso
  // se recorta con `spokenStart`.
  if (!audio.beginCapture(SAMPLE_RATE)) {
    audio.end();
    release();
    why = StrId::STR_AUDIO_CAPTURE_FAILED;
    return false;
  }
  const unsigned long rxOpenedAt = millis();
  const unsigned long toneStartedAt = startBlip(TONE_OPEN_HZ, TONE_OPEN_MS);

  // Cuánto del principio de la toma NO es la voz del usuario:
  //   (tono - captura)  lo que tardó en arrancar el tono desde que abrió el RX
  //   RX_PREFILL_MS     los descriptores DMA que ya venían llenándose
  //   TONE_OPEN_MS      el tono en sí, que el micrófono escucha
  //   TONE_TAIL_MS      la cola del parlante y del recinto
  // El piso de PREROLL_MIN_SAMPLES cubre el caso sin pitido (setBlips(false)) y
  // deja siempre lugar para reescribir la cabecera RIFF delante de la voz.
  const unsigned long preRollMs =
      (toneStartedAt > rxOpenedAt ? toneStartedAt - rxOpenedAt : 0) + RX_PREFILL_MS +
      (toneStartedAt ? TONE_OPEN_MS + TONE_TAIL_MS : 0);
  spokenStart = preRollMs * (SAMPLE_RATE / 1000);
  if (spokenStart < PREROLL_MIN_SAMPLES) spokenStart = PREROLL_MIN_SAMPLES;
  if (spokenStart > maxSamples) spokenStart = maxSamples;

  recording = true;
  ++s_open;
  registerLive();
  LOG_DBG(TAG, "Recording (max %u s, pre-roll %u ms = %u samples)", (unsigned)(maxSamples / SAMPLE_RATE),
          (unsigned)preRollMs, (unsigned)spokenStart);
  return true;
}

bool VoiceRecorder::pump() {
  if (!recording) return true;
  // El tono ya sonó: el buffer se puede soltar sin esperar al final de la toma.
  if (tone && !audio.isPlaying()) freeTone();
  int16_t* samples = reinterpret_cast<int16_t*>(buffer + wav::HEADER_BYTES);
  size_t want = maxSamples - recorded;
  if (want > BLOCK) want = BLOCK;
  const int n = audio.readCapture(samples + recorded, want, 50);
  if (n < 0) {
    abort();
    return false;
  }
  recorded += n;
  if (recorded >= maxSamples) stop();
  return true;
}

void VoiceRecorder::stop() {
  if (!recording) return;
  recording = false;
  if (s_open > 0) --s_open;
  unregisterLive();
  audio.endCapture();
  freeTone();
  blip(TONE_CLOSE_HZ, TONE_CLOSE_MS);
  audio.end();  // release I2S + codec before WiFi/TLS need the heap
  // Dos cabeceras: la del buffer entero (para depurar la toma cruda) y la que
  // vale, escrita justo delante de la primera muestra hablada. wav() devuelve
  // esa segunda, así que lo que se sube no lleva el pitido adelante.
  wav::writeHeader(buffer, SAMPLE_RATE, recorded * sizeof(int16_t));
  if (recorded > spokenStart) {
    wav::writeHeader(buffer + spokenStart * sizeof(int16_t), SAMPLE_RATE, spokenSamples() * sizeof(int16_t));
  }
  LOG_DBG(TAG, "Take: %u samples, %u hablados (%.1f s)", (unsigned)recorded, (unsigned)spokenSamples(),
          spokenSeconds());
}

void VoiceRecorder::abort() {
  if (recording) {
    recording = false;
    if (s_open > 0) --s_open;
    unregisterLive();
    audio.endCapture();
    freeTone();
    audio.end();
  }
  stopPlayback();
  release();
}

void VoiceRecorder::release() {
  if (buffer) {
    heap_caps_free(buffer);
    buffer = nullptr;
  }
  if (packed) {
    heap_caps_free(packed);
    packed = nullptr;
  }
  freeTone();
  recorded = 0;
  spokenStart = 0;
}

void VoiceRecorder::freeTone() {
  if (!tone) return;
  heap_caps_free(tone);
  tone = nullptr;
}

const uint8_t* VoiceRecorder::adpcm() {
  if (packed || !buffer || spokenSamples() == 0) return packed;
  const size_t bytes = adpcm::encodedSize(spokenSamples());
  packed = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!packed) packed = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!packed) return nullptr;
  adpcm::encode(reinterpret_cast<const int16_t*>(buffer + wav::HEADER_BYTES) + spokenStart, spokenSamples(), packed);
  LOG_DBG(TAG, "upload: %u bytes (was %u)", (unsigned)bytes, (unsigned)wavBytes());
  return packed;
}

// Solo el tamaño de lo que realmente se codificó: si el malloc de `packed`
// falló, adpcm() devuelve nullptr y el que sube no puede postear N bytes de un
// puntero nulo.
size_t VoiceRecorder::adpcmBytes() const { return packed ? adpcm::encodedSize(spokenSamples()) : 0; }

size_t VoiceRecorder::wavBytes() const {
  return buffer && spokenSamples() ? wav::HEADER_BYTES + spokenSamples() * sizeof(int16_t) : 0;
}

// La cabecera de la toma recortada vive DENTRO del buffer, pisando el final del
// pitido: 44 bytes antes de la primera muestra hablada. Por eso el piso de
// PREROLL_MIN_SAMPLES (800 muestras = 1600 bytes) nunca puede bajar de 22.
const uint8_t* VoiceRecorder::wav() const {
  if (!buffer) return nullptr;
  if (recorded <= spokenStart) return buffer;
  return buffer + spokenStart * sizeof(int16_t);
}

bool VoiceRecorder::hasSpeech() const {
  const size_t total = spokenSamples();
  if (!buffer || total < SPEECH_WINDOW * 3) return false;
  const int16_t* pcm = reinterpret_cast<const int16_t*>(buffer + wav::HEADER_BYTES) + spokenStart;
  const size_t n = total / SPEECH_WINDOW;
  // Pico por ventana, en el heap: 12 s de toma son 800 ventanas y esto corre en
  // la tarea del loop, que no tiene pila para 3 KB de arrays.
  int16_t* peak = static_cast<int16_t*>(malloc(n * sizeof(int16_t) * 2));
  if (!peak) return true;  // sin memoria para medir, se manda igual
  int16_t* sorted = peak + n;
  for (size_t w = 0; w < n; ++w) {
    int32_t p = 0;
    for (size_t i = 0; i < SPEECH_WINDOW; ++i) {
      int32_t v = pcm[w * SPEECH_WINDOW + i];
      if (v < 0) v = -v;
      if (v > p) p = v;
    }
    peak[w] = static_cast<int16_t>(p > 32767 ? 32767 : p);
  }
  // Mediana = piso de ruido de la sala (más de la mitad de las ventanas son
  // silencio en cualquier dictado normal).
  memcpy(sorted, peak, n * sizeof(int16_t));
  std::sort(sorted, sorted + n);
  const int32_t median = sorted[n / 2];
  const int32_t threshold = std::max<int32_t>(SPEECH_FLOOR, median * 4);
  size_t loud = 0, first = 0, last = 0;
  for (size_t w = 0; w < n; ++w) {
    if (peak[w] < threshold) continue;
    if (!loud) first = w;
    last = w;
    ++loud;
  }
  const size_t spanMs = loud ? (last - first + 1) * SPEECH_WINDOW * 1000 / SAMPLE_RATE : 0;
  LOG_DBG(TAG, "speech: %u/%u ventanas sobre %d (piso %d), %u ms", (unsigned)loud, (unsigned)n, (int)threshold,
          (int)median, (unsigned)spanMs);
  free(peak);
  return loud >= 3 && spanMs >= 500;
}

// --- Reproducción de la toma (revisión de la nota de voz) --------------------
bool VoiceRecorder::playSpoken() {
  if (recording || !buffer || spokenSamples() == 0) return false;
  stopPlayback();
  MUSIC.stop();
  if (!audio.begin()) return false;
  audio.setVolume(std::max<uint8_t>(HUB_STORE.musicVolume, 40));
  if (!audio.playBuffer(wav(), wavBytes(), false)) {
    audio.end();
    return false;
  }
  playingBack = true;
  return true;
}

bool VoiceRecorder::isPlayingBack() {
  if (!playingBack) return false;
  if (audio.isPlaying()) return true;
  stopPlayback();
  return false;
}

void VoiceRecorder::stopPlayback() {
  if (!playingBack) return;
  playingBack = false;
  audio.stop();
  audio.end();
}

// Arma el tono y lo suelta SIN esperarlo: el buffer es miembro (`tone`) porque
// el AudioManager lo sigue leyendo desde su tarea después de volver de acá, y
// uno en la pila se lo llevaría el retorno. Devuelve el instante en que empezó
// a sonar (0 si no hay pitido), que es lo que ancla el recorte del pre-roll.
unsigned long VoiceRecorder::startBlip(const uint16_t hz, const uint16_t ms) {
  if (!blips) return 0;
  freeTone();
  const size_t samples = SAMPLE_RATE * ms / 1000;
  const size_t bytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  tone = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!tone) return 0;
  fillTone(tone, samples, hz);
  wav::writeHeader(tone, SAMPLE_RATE, samples * sizeof(int16_t));
  // El pitido se escucha con el volumen del aparato, pero nunca tan bajo que el
  // usuario no sepa que el micrófono abrió.
  audio.setVolume(std::max<uint8_t>(HUB_STORE.musicVolume, 25));
  if (!audio.playBuffer(tone, bytes, false)) {
    freeTone();
    return 0;
  }
  return millis();
}

// One sine tone, played to completion (short, so a blocking wait is fine).
// Solo para el pitido de cierre: ahí la toma ya terminó y no hay nada que
// recortar, así que esperarlo es lo más simple y no pierde audio.
void VoiceRecorder::blip(const uint16_t hz, const uint16_t ms) {
  if (!blips) return;
  const size_t samples = SAMPLE_RATE * ms / 1000;
  const size_t bytes = wav::HEADER_BYTES + samples * sizeof(int16_t);
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!buf) return;
  fillTone(buf, samples, hz);
  wav::writeHeader(buf, SAMPLE_RATE, samples * sizeof(int16_t));
  audio.setVolume(std::max<uint8_t>(HUB_STORE.musicVolume, 25));
  if (audio.playBuffer(buf, bytes, false)) {
    const unsigned long until = millis() + ms + 150;
    while (audio.isPlaying() && millis() < until) delay(5);
    audio.stop();
  }
  heap_caps_free(buf);
}

