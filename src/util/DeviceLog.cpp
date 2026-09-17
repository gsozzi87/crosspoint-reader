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
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* DIR = "/.crosspoint";
constexpr const char* CURRENT = "/.crosspoint/device.log";
constexpr const char* PREVIOUS = "/.crosspoint/device.prev.log";
// Hasta dónde se subió ya. Un número decimal y nada más; vive en su propio
// archivo (y no en hub.json) para que el log se pueda subir aunque la
// sincronización falle, que es justamente cuando más sirve.
constexpr const char* MARK = "/.crosspoint/log.sent";
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
  snprintf(
      line, sizeof(line), "bateria %u%% | heap %lu KB libre (bloque mayor %lu KB) | psram %lu KB libre\n",
      static_cast<unsigned>(powerManager.getBatteryPercentage()), static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
      static_cast<unsigned long>(ESP.getMaxAllocHeap() / 1024), static_cast<unsigned long>(ESP.getFreePsram() / 1024));
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

// La marca es un desplazamiento DENTRO de device.log, así que rotar la
// invalida: el archivo nuevo arranca en cero y una marca vieja apuntaría a la
// mitad de otra cosa.
void resetMark() { Storage.remove(MARK); }

size_t readMark() {
  HalFile f;
  if (!Storage.openFileForRead("LOG", MARK, f)) return 0;
  char buf[24] = {0};
  const int got = f.read(buf, sizeof(buf) - 1);
  f.close();
  if (got <= 0) return 0;
  buf[got] = '\0';
  const long v = atol(buf);
  return v > 0 ? static_cast<size_t>(v) : 0;
}

void openCurrent(const bool append) {
  if (file.isOpen()) file.close();
  if (!append && Storage.exists(CURRENT)) {
    Storage.remove(PREVIOUS);
    Storage.rename(CURRENT, PREVIOUS);
    resetMark();
  }
  // AGREGAR ES AGREGAR, Y `openFileForWrite` NO AGREGA: abre con O_TRUNC
  // (SDCardManager.cpp), así que el `seek(size())` de acá era un no-op sobre un
  // archivo que ya había quedado en cero. O sea que device.log se BORRABA en
  // cada arranque y lo que quedaba para subir era casi siempre device.prev.log,
  // que es viejo y no cambia: por eso en /board/log se veían las mismas tandas
  // con versiones de firmware de hace semanas en cada sincronización. El
  // arreglo de 1.5.55 (mandar CURRENT antes que PREVIOUS) atacó el orden, que
  // era la mitad; esto es la otra mitad.
  // Sin LOG_ERR: este archivo ES el sumidero del log y `emit()` llama acá.
  HalFile f = Storage.open(CURRENT, O_RDWR | O_CREAT);
  if (!f.isOpen()) return;
  const size_t n = f.size();
  if (append && n > 0) f.seek(n);
  written = append ? n : 0;
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
    // Lo que se escribió después de la última subida y todavía no viajó se va
    // con la rotación: queda en el archivo anterior, que nadie sube. Es poco
    // (hay que llenar 64 KB entre dos sincronizaciones) pero no es cero, así
    // que se dice en el archivo nuevo en vez de dejar un salto mudo.
    const size_t sent = readMark();
    const size_t perdido = written > sent ? written - sent : 0;
    openCurrent(false);
    if (!file.isOpen()) {
      ready = false;  // la SD dejó de aceptar el archivo: dejar de intentarlo en cada línea
      return;
    }
    writeHeader();
    if (perdido > 0) {
      char aviso[96];
      snprintf(aviso, sizeof(aviso), "(el log rotó: %lu bytes no llegaron a subirse)\n",
               static_cast<unsigned long>(perdido));
      rawLine(aviso);
    }
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

// ─────────────────────────────────────── lo que falta subir
//
// EL PROBLEMA QUE ESTO ARREGLA: el aparato mandaba en cada sincronización el
// final de su log, y el servidor APENDA. O sea que las mismas líneas subían una
// y otra vez, y en /board/log el usuario veía la misma tanda repetida seis
// veces con arranques de firmware de hace semanas en el medio. Buscar lo que
// acababa de pasar era imposible.
//
// Ahora se manda sólo lo que se escribió DESPUÉS de la última subida buena. La
// marca es un desplazamiento en device.log y se guarda en la tarjeta, así que
// vale entre arranques (el archivo se abre en modo agregar); rotar la borra.
std::string devlog::unsent(const size_t maxBytes, size_t& mark) {
  devlog::flush();
  mark = 0;
  HalFile f;
  if (!Storage.openFileForRead("LOG", CURRENT, f)) return "";
  const size_t n = f.size();
  mark = n;
  size_t from = readMark();
  if (from > n) {
    // La marca quedó adelante del archivo: rotó sin que nos enteráramos, o lo
    // vaciaron. Se manda el final y se vuelve a empezar.
    from = 0;
  }
  if (from >= n) {
    f.close();
    return "";  // nada nuevo
  }
  bool saltado = false;
  if (n - from > maxBytes) {
    // Más de lo que entra en una subida: se manda el final. El salto se avisa,
    // porque si no se lee como si en el medio no hubiera pasado nada.
    from = n - maxBytes;
    saltado = true;
  }
  const size_t want = n - from;
  std::string out;
  out.resize(want);
  f.seek(from);
  const int got = f.read(&out[0], want);
  f.close();
  if (got <= 0) {
    mark = 0;
    return "";
  }
  out.resize(got);
  // La marca es hasta donde LLEGÓ la lectura, no hasta donde queríamos llegar:
  // `read` puede devolver menos de lo pedido sin que sea un error, y confirmar
  // `n` ahí daría por subido un pedazo que nunca viajó. Silenciosamente, que es
  // la peor forma de perder un log.
  mark = from + static_cast<size_t>(got);
  if (saltado) out.insert(0, "--- (el log creció más de lo que entra en una subida: falta el medio) ---\n");
  return out;
}

void devlog::confirmSent(const size_t mark) {
  if (mark == 0) return;
  HalFile f;
  if (!Storage.openFileForWrite("LOG", MARK, f)) return;
  char buf[24];
  const int n = snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(mark));
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
  f.close();
}
