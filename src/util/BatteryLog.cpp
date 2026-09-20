#include "BatteryLog.h"

#include <BatteryMonitor.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace batterylog {
namespace {
constexpr const char* TAG = "BATLOG";
constexpr const char* PATH = "/.crosspoint/battery.csv";

using Row = Sample;

unsigned long lastSampleMs = 0;
bool everSampled = false;

// Todo el archivo de una: son 300 líneas de ~30 bytes, menos de 12 KB.
std::vector<Row> readAll() {
  std::vector<Row> out;
  if (!Storage.exists(PATH)) return out;
  const String text = Storage.readFile(PATH);
  const char* p = text.c_str();
  while (p && *p) {
    const char* eol = strchr(p, '\n');
    Row r;
    if (parseLine(p, r)) out.push_back(r);
    if (!eol) break;
    p = eol + 1;
  }
  return out;
}

// Deja la mitad más nueva. Reescribir entero es más simple y más seguro que
// intentar recortar la cabeza de un archivo en la tarjeta.
void rotate(std::vector<Row>& rows) {
  const size_t keep = MAX_ROWS / 2;
  if (rows.size() <= MAX_ROWS) return;
  rows.erase(rows.begin(), rows.end() - static_cast<long>(keep));
  String out;
  char line[64];
  for (const Row& r : rows) {
    snprintf(line, sizeof(line), "%lld,%d,%d,%d\n", static_cast<long long>(r.epoch), r.pct, r.mv, r.charging ? 1 : 0);
    out += line;
  }
  Storage.writeFile(PATH, out);
  LOG_INF(TAG, "diario rotado a %u líneas", (unsigned)rows.size());
}

// Se reescribe el archivo entero en vez de agregar al final: con 300 líneas de
// 30 bytes son 12 KB, y así no hay que depender de las banderas de apertura de
// la tarjeta ni dejar a medias un archivo que se lee con sscanf.
void append(const Row& r) {
  std::vector<Row> rows = readAll();
  rows.push_back(r);
  if (rows.size() > MAX_ROWS) {
    rotate(rows);
    return;
  }
  String out;
  char line[64];
  for (const Row& row : rows) {
    snprintf(line, sizeof(line), "%lld,%d,%d,%d\n", static_cast<long long>(row.epoch), row.pct, row.mv,
             row.charging ? 1 : 0);
    out += line;
  }
  Storage.writeFile(PATH, out);
}
}  // namespace

void sampleNow(const char* why) {
  time_t now = 0;
  // `> 0` NO alcanza para decir "hay hora": el PCF85063 sin pila arranca en
  // 2000-01-01 y eso pasa esa prueba. Una sola muestra con esa fecha entre las
  // buenas hace una ventana de veinticinco años (ver credibleEpoch).
  if (!halClock.getEpochUtc(now) || !batterylog::credibleEpoch(now)) return;
  static const BatteryMonitor battery;
  uint16_t pct = 0;
  if (!battery.readPercentageChecked(pct)) return;
  Row r;
  r.epoch = now;
  r.pct = pct;
  r.mv = battery.readMillivolts();
  r.charging = battery.isCharging();
  append(r);
  lastSampleMs = millis();
  everSampled = true;
  LOG_INF(TAG, "%s: %u %% · %u mV%s", why, (unsigned)r.pct, (unsigned)r.mv, r.charging ? " (cargando)" : "");
}

void reportAfterSleep() {
  time_t now = 0;
  if (!halClock.getEpochUtc(now) || !batterylog::credibleEpoch(now)) return;
  const std::vector<Row> rows = readAll();
  if (rows.empty()) return;
  const Row& last = rows.back();
  if (!batterylog::credibleEpoch(last.epoch) || last.charging) return;
  const double secs = static_cast<double>(now - last.epoch);
  if (secs < 20 * 60 || secs > MAX_WINDOW_S)
    return;  // menos de 20 min no mide nada; más de dos semanas es la fecha rota
  static const BatteryMonitor battery;
  uint16_t pct = 0;
  if (!battery.readPercentageChecked(pct)) return;
  const int mv = battery.readMillivolts();
  const double hours = secs / 3600.0;
  const double perHour = (last.pct - static_cast<int>(pct)) / hours;
  // Un sueño profundo sano son décimas por hora (el S3 dormido son microamperios;
  // lo que queda son los rieles del PMIC con el códec, la tarjeta y el panel).
  // Arriba de esto hay algo encendido que no debería, y el log lo dice solo.
  const bool tooMuch = perHour >= 0.3;
  if (tooMuch) {
    LOG_ERR(
        TAG,
        "dormido %.1f h: %d -> %u %% (%.2f %%/h, %d -> %d mV) — DEMASIADO para un sueño profundo: algo quedó encendido",
        hours, last.pct, (unsigned)pct, perHour, last.mv, mv);
  } else {
    LOG_INF(TAG, "dormido %.1f h: %d -> %u %% (%.2f %%/h, %d -> %d mV)", hours, last.pct, (unsigned)pct, perHour,
            last.mv, mv);
  }
}

void tick() {
  const unsigned long every = SAMPLE_MIN * 60UL * 1000UL;
  if (everSampled && millis() - lastSampleMs < every) return;
  if (!everSampled && millis() < 30000) return;  // dejar que el RTC y la tarjeta estén arriba
  sampleNow("muestra");
}

size_t rows() { return readAll().size(); }

void reset() {
  Storage.remove(PATH);
  everSampled = false;
  lastSampleMs = 0;
}

Drain measure() { return analyze(readAll()); }

}  // namespace batterylog
