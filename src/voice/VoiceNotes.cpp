#include "VoiceNotes.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "Adpcm.h"
#include "CrossPointSettings.h"
#include "SpeechOut.h"

namespace {
constexpr const char* TAG = "VNOTE";
// Lo que cuesta un segundo de toma: 16 kHz mono a 16 bits (32 KB de PCM) más la
// copia en ADPCM, que vive al mismo tiempo (8 KB).
constexpr uint32_t PCM_BYTES_PER_S = 32000;
constexpr uint32_t TOTAL_BYTES_PER_S = 40000;
// Lo que hay que dejar libre para lo que viene después de la toma: TLS, el JSON
// de la respuesta y el decodificado de la reproducción.
constexpr uint32_t RESERVE_BYTES = 512 * 1024;
constexpr uint32_t MIN_SECONDS = 10;

bool endsWithExt(const std::string& name) {
  const std::string ext = voicenotes::EXT;
  return name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
}
}  // namespace

void voicenotes::ensureDir() {
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.ensureDirectoryExists(DIR);
}

std::string voicenotes::mmss(const int seconds) {
  const int s = seconds < 0 ? 0 : seconds;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return buf;
}

std::string voicenotes::when(const Note& note) {
  if (note.epoch <= 0) return "";
  // Mismo huso que el reloj del hub: el RTC guarda UTC y el ajuste es el
  // desplazamiento en cuartos de hora sesgado (48 = UTC+0).
  const time_t local = note.epoch + static_cast<time_t>(SETTINGS.clockUtcOffsetQ - 48) * 900;
  struct tm t;
  gmtime_r(&local, &t);
  char buf[24];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
  return buf;
}

std::vector<voicenotes::Note> voicenotes::list() {
  std::vector<Note> out;
  ensureDir();
  for (const String& entry : Storage.listFiles(DIR, 80)) {
    const std::string name = entry.c_str();
    if (!endsWithExt(name)) continue;
    Note n;
    n.path = std::string(DIR) + "/" + name;
    const std::string stem = name.substr(0, name.size() - std::string(EXT).size());
    if (!stem.empty() && stem[0] == 'n') {
      n.number = atoi(stem.c_str() + 1);
    } else {
      n.epoch = static_cast<time_t>(strtoll(stem.c_str(), nullptr, 10));
    }
    HalFile f;
    if (Storage.openFileForRead(TAG, n.path, f)) {
      const size_t size = f.size();
      f.close();
      if (size > adpcm::HEADER_BYTES) {
        // Un byte = dos muestras de 4 bits.
        n.seconds = static_cast<int>((size - adpcm::HEADER_BYTES) * 2 / adpcm::SAMPLE_RATE);
      }
    }
    out.push_back(std::move(n));
  }
  // Las más nuevas arriba; las numeradas (sin fecha) van después de las fechadas.
  std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) {
    if ((a.epoch > 0) != (b.epoch > 0)) return a.epoch > 0;
    if (a.epoch > 0) return a.epoch > b.epoch;
    return a.number > b.number;
  });
  return out;
}

bool voicenotes::save(const uint8_t* data, const size_t len, Note& out) {
  if (!data || len <= adpcm::HEADER_BYTES) return false;
  ensureDir();
  time_t now = 0;
  char name[24];
  if (halClock.getEpochUtc(now) && now > 1000000000) {
    snprintf(name, sizeof(name), "/%lld%s", static_cast<long long>(now), EXT);
    out.epoch = now;
  } else {
    // Sin reloj no hay fecha posible: se numeran a partir de la última.
    int next = 1;
    for (const Note& n : list()) next = std::max(next, n.number + 1);
    snprintf(name, sizeof(name), "/n%03d%s", next, EXT);
    out.epoch = 0;
    out.number = next;
  }
  out.path = std::string(DIR) + name;
  HalFile f;
  if (!Storage.openFileForWrite(TAG, out.path, f)) {
    LOG_ERR(TAG, "no se pudo crear %s", out.path.c_str());
    return false;
  }
  const size_t written = f.write(data, len);
  f.close();
  if (written != len) {
    LOG_ERR(TAG, "escritura corta: %u de %u", (unsigned)written, (unsigned)len);
    Storage.remove(out.path.c_str());
    return false;
  }
  out.seconds = static_cast<int>((len - adpcm::HEADER_BYTES) * 2 / adpcm::SAMPLE_RATE);
  LOG_INF(TAG, "nota de voz %s (%u bytes, %d s)", out.path.c_str(), (unsigned)len, out.seconds);
  return true;
}

bool voicenotes::remove(const Note& note) {
  if (note.path.empty()) return false;
  const bool ok = Storage.remove(note.path.c_str());
  LOG_INF(TAG, "borrar %s: %s", note.path.c_str(), ok ? "ok" : "falló");
  return ok;
}

bool voicenotes::play(const Note& note, SpeechOut& out) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, note.path, f)) return false;
  const size_t size = f.size();
  if (size <= adpcm::HEADER_BYTES) {
    f.close();
    return false;
  }
  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) raw = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
  if (!raw) {
    f.close();
    LOG_ERR(TAG, "sin memoria para %u bytes", (unsigned)size);
    return false;
  }
  const int got = f.read(raw, size);
  f.close();
  // playAdpcm decodifica a un WAV propio, así que el crudo se puede soltar acá.
  const bool ok = got == static_cast<int>(size) && out.playAdpcm(raw, size);
  heap_caps_free(raw);
  return ok;
}

uint32_t voicenotes::maxRecordSeconds(const uint32_t wanted) {
  // El WAV es una sola reserva grande: lo que manda es el bloque contiguo más
  // grande de PSRAM, no el total libre.
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (block <= RESERVE_BYTES || free <= RESERVE_BYTES) return MIN_SECONDS;
  const uint32_t byBlock = static_cast<uint32_t>((block - RESERVE_BYTES) / PCM_BYTES_PER_S);
  const uint32_t byFree = static_cast<uint32_t>((free - RESERVE_BYTES) / TOTAL_BYTES_PER_S);
  uint32_t fits = std::min(byBlock, byFree);
  if (fits > wanted) fits = wanted;
  if (fits < MIN_SECONDS) fits = MIN_SECONDS;
  if (fits != wanted) {
    LOG_INF(TAG, "tope de grabación %u s (pedidos %u): bloque %u KB, libre %u KB", (unsigned)fits, (unsigned)wanted,
            (unsigned)(block / 1024), (unsigned)(free / 1024));
  }
  return fits;
}
