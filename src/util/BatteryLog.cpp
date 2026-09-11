#include "BatteryLog.h"

#include <BatteryMonitor.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <HalClock.h>

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
    // epoch,pct,mv,charging
    if (sscanf(p, "%ld,%d,%d,%d", reinterpret_cast<long*>(&r.epoch), &r.pct, &r.mv,
               reinterpret_cast<int*>(&r.charging)) >= 3 &&
        r.epoch > 0) {
      out.push_back(r);
    }
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
    snprintf(line, sizeof(line), "%ld,%d,%d,%d\n", static_cast<long>(r.epoch), r.pct, r.mv, r.charging ? 1 : 0);
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
    snprintf(line, sizeof(line), "%ld,%d,%d,%d\n", static_cast<long>(row.epoch), row.pct, row.mv, row.charging ? 1 : 0);
    out += line;
  }
  Storage.writeFile(PATH, out);
}
}  // namespace

void sampleNow(const char* why) {
  time_t now = 0;
  if (!halClock.getEpochUtc(now) || now <= 0) return;  // sin hora no hay pendiente posible
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
