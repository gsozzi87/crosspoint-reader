#include "DeviceLog.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <ws397_version.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* DIR = "/.crosspoint";
constexpr const char* CURRENT = "/.crosspoint/device.log";
constexpr const char* PREVIOUS = "/.crosspoint/device.prev.log";
constexpr size_t HEAD_BYTES = 3 * 1024;  // cabecera de la sesión que se manda siempre (ver tail)
constexpr size_t MAX_BYTES = 64 * 1024;
constexpr size_t FLUSH_EVERY = 2 * 1024;
// Huella de la línea para deduplicar. Larga a propósito: dos líneas distintas
// que solo se parecen en los primeros caracteres no se pueden confundir.
constexpr size_t KEY_LEN = 120;
constexpr uint32_t MAX_REPEATS = 999999;
// La hora sale del RTC por I²C: leerla en cada línea era un viaje al bus por
// línea de log. Se lee cada tanto y solo se anota cuando cambia el minuto.
constexpr unsigned long CLOCK_POLL_MS = 15000;

HalFile file;
size_t written = 0;
size_t sinceFlush = 0;
bool ready = false;
bool inWrite = false;

char lastKey[KEY_LEN] = "";
uint32_t repeatCount = 0;

uint8_t lastHour = 0xFF;
uint8_t lastMinute = 0xFF;
unsigned long lastClockPollMs = 0;
bool clockPolled = false;
bool wifiNoted = false;

void rawWrite(const char* text, const size_t len) {
  if (!file.isOpen() || len == 0) return;
  file.write(reinterpret_cast<const uint8_t*>(text), len);
  written += len;
  sinceFlush += len;
  if (sinceFlush >= FLUSH_EVERY) {
    file.flush();
    sinceFlush = 0;
  }
}

void rawLine(const char* text) { rawWrite(text, strlen(text)); }

// Forma de la línea, sin los números: "[12345] [ERR] [GFX] !! Outside range
// (3, 481)" y "[12348] [ERR] [GFX] !! Outside range (4, 481)" son la misma
// cosa repetida, y así se cuentan juntas.
void fingerprint(const char* line, char* out) {
  size_t o = 0;
  bool inNumber = false;
  for (const char* p = line; *p != '\0' && o + 1 < KEY_LEN; ++p) {
    if (*p >= '0' && *p <= '9') {
      if (!inNumber) out[o++] = '#';
      inNumber = true;
      continue;
    }
    inNumber = false;
    out[o++] = *p;
  }
  out[o] = '\0';
}

void flushRepeats() {
  if (repeatCount > 1) {
    char tail[24];
    snprintf(tail, sizeof(tail), "  (x%lu)\n", static_cast<unsigned long>(repeatCount));
    rawLine(tail);
  }
  repeatCount = 0;
  lastKey[0] = '\0';
}

const char* wakeReasonName() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
    case ESP_SLEEP_WAKEUP_GPIO:
      return "boton";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "temporizador";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ulp";
    default:
      return "encendido";
  }
}

// Cabecera de sesión: sin esto, un log rotado no decía ni qué versión lo
// escribió. Va también arriba del archivo nuevo cuando rota, para que el
// pedazo que sobrevive siga siendo legible.
void writeHeader() {
  char line[160];
  snprintf(line, sizeof(line), "\n=== %s | arranque por %s ===\n", CROSSPOINT_VERSION, wakeReasonName());
  rawLine(line);
  snprintf(line, sizeof(line), "bateria %u%% | heap %lu KB libre (bloque mayor %lu KB) | psram %lu KB libre\n",
           static_cast<unsigned>(powerManager.getBatteryPercentage()),
           static_cast<unsigned long>(ESP.getFreeHeap() / 1024), static_cast<unsigned long>(ESP.getMaxAllocHeap() / 1024),
           static_cast<unsigned long>(ESP.getFreePsram() / 1024));
  rawLine(line);
}

// El nombre de la red es dato privado (el log se sube al servidor): solo se
// anota la señal.
void noteWifiOnce() {
  if (wifiNoted || WiFi.status() != WL_CONNECTED) return;
  wifiNoted = true;
  const int rssi = WiFi.RSSI();
  const char* quality = rssi >= -60 ? "buena" : (rssi >= -70 ? "regular" : "debil");
  char line[80];
  snprintf(line, sizeof(line), "wifi conectado, senal %d dBm (%s)\n", rssi, quality);
  rawLine(line);
}

void noteClockChange() {
  const unsigned long now = millis();
  if (clockPolled && now - lastClockPollMs < CLOCK_POLL_MS) return;
  clockPolled = true;
  lastClockPollMs = now;
  uint8_t h = 0;
  uint8_t m = 0;
  if (!halClock.isAvailable() || !halClock.getTime(h, m)) return;
  if (h == lastHour && m == lastMinute) return;
  lastHour = h;
  lastMinute = m;
  char line[24];
  snprintf(line, sizeof(line), "--- %02u:%02u ---\n", static_cast<unsigned>(h), static_cast<unsigned>(m));
  rawLine(line);
}

void openCurrent(const bool append) {
  if (file.isOpen()) file.close();
  if (!append && Storage.exists(CURRENT)) {
    Storage.remove(PREVIOUS);
    Storage.rename(CURRENT, PREVIOUS);
  }
  HalFile f;
  if (!Storage.openFileForWrite("LOG", CURRENT, f)) return;
  if (append) f.seek(f.size());
  written = f.size();
  file = std::move(f);
}

// Escribe la línea ya formada, deduplicando: la primera sale entera y las
// iguales que vienen atrás solo suben el contador.
void emit(const char* line, const bool dedup) {
  char key[KEY_LEN];
  if (dedup) {
    fingerprint(line, key);
    if (repeatCount > 0 && strcmp(key, lastKey) == 0) {
      if (repeatCount < MAX_REPEATS) ++repeatCount;
      return;
    }
  }
  flushRepeats();
  noteClockChange();
  noteWifiOnce();
  if (dedup) {
    strncpy(lastKey, key, KEY_LEN - 1);
    lastKey[KEY_LEN - 1] = '\0';
    repeatCount = 1;
  }
  rawWrite(line, strlen(line));
  if (written >= MAX_BYTES) {
    flushRepeats();
    openCurrent(false);
    if (!file.isOpen()) {
      ready = false;  // la SD dejó de aceptar el archivo: dejar de intentarlo en cada línea
      return;
    }
    writeHeader();
  }
}
}  // namespace

void devlog::begin() {
  if (ready) return;
  Storage.ensureDirectoryExists(DIR);
  openCurrent(true);
  ready = file.isOpen();
  if (!ready) return;
  lastKey[0] = '\0';
  repeatCount = 0;
  wifiNoted = false;
  clockPolled = false;
  writeHeader();
  file.flush();
}

void devlog::write(const char* line) {
  if (!ready || !line || inWrite) return;
  inWrite = true;  // a failing write must not log itself
  emit(line, true);
  inWrite = false;
}

void devlog::event(const char* tag, const char* fmt, ...) {
  if (!ready || !fmt || inWrite) return;
  inWrite = true;
  char line[224];
  const int head = snprintf(line, sizeof(line), "* [%s] ", tag ? tag : "APP");
  if (head > 0 && static_cast<size_t>(head) < sizeof(line) - 2) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + head, sizeof(line) - head - 2, fmt, args);
    va_end(args);
    const size_t len = strlen(line);
    line[len] = '\n';
    line[len + 1] = '\0';
    emit(line, false);
  }
  inWrite = false;
}

void devlog::flush() {
  if (!ready || !file.isOpen()) return;
  flushRepeats();
  file.flush();
  sinceFlush = 0;
}

void devlog::close() {
  if (ready && file.isOpen()) {
    flushRepeats();
    file.flush();
    file.close();
  }
  ready = false;  // la SD se desmonta enseguida: no escribir más
}

size_t devlog::size() { return written; }

std::string devlog::tail(const size_t maxBytes) {
  devlog::flush();
  std::string out;
  auto readInto = [&](const char* path) {
    HalFile f;
    if (!Storage.openFileForRead("LOG", path, f)) return;
    const size_t n = f.size();
    const size_t want = std::min(n, maxBytes - std::min(out.size(), maxBytes));
    if (want == 0) {
      f.close();
      return;
    }
    std::string chunk;
    chunk.resize(want);
    f.seek(n - want);
    const int got = f.read(&chunk[0], want);
    f.close();
    if (got > 0) {
      chunk.resize(got);
      out += chunk;
    }
  };
  // EL ORDEN IMPORTA, y estaba al reves: se leia PREVIOUS primero y, como el
  // archivo rotado suele estar lleno (64 KB), se comia el presupuesto entero y
  // a CURRENT le quedaban cero bytes. O sea que el aparato subia SIEMPRE el
  // final del log VIEJO y nunca una linea de lo que acababa de pasar: en
  // /board/log se veia la misma tanda de hace semanas en cada sincronizacion,
  // y borrarla no servia de nada porque la siguiente subida repetia lo mismo.
  //
  // Ahora manda lo nuevo: primero el final de CURRENT y, solo si sobra lugar,
  // el final de PREVIOUS delante para dar contexto.
  readInto(CURRENT);
  // El ARRANQUE de este log también va siempre: la cabecera de la sesión
  // (versión, por qué arrancó, los registros del PMIC, la polaridad del PWR)
  // está en las primeras líneas, y cuando la sesión es larga el final de 24 KB
  // ya no la incluye. Un PWR "errático" sin esas líneas no se puede leer.
  {
    HalFile f;
    if (Storage.openFileForRead("LOG", CURRENT, f)) {
      const size_t n = f.size();
      if (n > out.size() && HEAD_BYTES < maxBytes) {
        // El tail no llegó al principio del archivo: se antepone el principio.
        const size_t want = std::min(n - out.size(), HEAD_BYTES);
        std::string head;
        head.resize(want);
        f.seek(0);
        const int got = f.read(&head[0], want);
        if (got > 0) {
          head.resize(got);
          head += "\n--- (... se saltó el medio del log ...) ---\n";
          if (out.size() + head.size() > maxBytes) out.erase(0, out.size() + head.size() - maxBytes);
          out = head + out;
        }
      }
      f.close();
    }
  }
  if (out.size() < maxBytes && Storage.exists(PREVIOUS)) {
    const size_t room = maxBytes - out.size();
    std::string current;
    current.swap(out);
    std::string tailOfPrev;
    {
      HalFile f;
      if (Storage.openFileForRead("LOG", PREVIOUS, f)) {
        const size_t n = f.size();
        const size_t want = std::min(n, room);
        if (want > 0) {
          tailOfPrev.resize(want);
          f.seek(n - want);
          const int got = f.read(&tailOfPrev[0], want);
          if (got > 0) tailOfPrev.resize(got);
          else tailOfPrev.clear();
        }
        f.close();
      }
    }
    out = tailOfPrev;
    if (!out.empty()) out += "\n--- (arriba: log anterior) ---\n";
    out += current;
  }
  return out;
}
