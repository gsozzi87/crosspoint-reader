#include "HalClock.h"
#include <sys/time.h>

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::getEpochUtc(time_t& out) const {
  if (!_available) return false;
  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    // Un reintento: el choque en el bus compartido es de una transacción, no
    // del chip. Si la bandera OS está puesta el segundo intento también falla,
    // que es lo correcto — ahí el RTC de verdad perdió la hora.
    delay(2);
    if (!_sdkRtc.now(dt)) {
      // Todavía nada: estirar el último epoch bueno antes de declarar que el
      // aparato no está en hora. Decir "no hay reloj" cuando hace un segundo
      // lo había manda a buscar el problema a la pila y no al bus.
      if (_cachedEpoch <= 0) return false;
      const unsigned long elapsed = millis() - _cachedEpochMs;
      if (elapsed > EPOCH_CACHE_MAX_MS) return false;
      out = _cachedEpoch + static_cast<time_t>(elapsed / 1000);
      return true;
    }
  }
  struct tm t = {};
  t.tm_year = dt.year - 1900;
  t.tm_mon = dt.month - 1;
  t.tm_mday = dt.day;
  t.tm_hour = dt.hour;
  t.tm_min = dt.minute;
  t.tm_sec = dt.second;
  // Days-from-civil (Howard Hinnant), so this does not depend on timegm() or the process TZ.
  int y = dt.year, m = dt.month;
  const int d = dt.day;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + static_cast<long>(doe) - 719468L;
  out = static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
  _cachedEpoch = out;
  _cachedEpochMs = millis();
  return true;
}

bool HalClock::setFromEpochUtc(const time_t epoch) {
  if (!_available) return false;
  struct tm timeinfo;
  gmtime_r(&epoch, &timeinfo);
  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
  dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
  dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
  dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
  dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
  dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
  dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
  if (!_sdkRtc.set(dt)) return false;
  _lastPollMs = 0;
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _hasCachedTime = true;
  // Y el reloj del SISTEMA con él. Son dos relojes distintos y hasta acá se
  // mantenía uno solo: poner en hora el RTC dejaba `time(nullptr)` en 1970
  // hasta el próximo arranque, que es cuando applyToSystemClock() lo lee. En
  // un aparato que acaba de sincronizar eso es justo al revés de lo que hace
  // falta — y ServerClient::ensureClockForTls() ya gastó su único intento de
  // la sesión, así que nadie lo vuelve a corregir.
  applySystemClock(epoch);
  LOG_INF("CLK", "RTC set from server to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour,
          dt.minute, dt.second);
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        applySystemClock(now);
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

// Año 2024 en epoch. Cualquier cosa por debajo es "el reloj no está puesto":
// el ESP arranca en 1970 y el RTC sin batería o recién soldado da valores
// igual de absurdos.
static constexpr time_t CREIBLE_DESDE = 1704067200;  // 2024-01-01

bool HalClock::systemClockLooksSet() { return time(nullptr) >= CREIBLE_DESDE; }

bool HalClock::applySystemClock(const time_t epoch) {
  if (epoch < CREIBLE_DESDE) return false;
  const struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  if (settimeofday(&tv, nullptr) != 0) {
    LOG_ERR("CLK", "settimeofday falló");
    return false;
  }
  return true;
}

bool HalClock::applyToSystemClock() const {
  // TRES motivos distintos, y hasta acá los tres salían con el mismo cartel.
  // Mandan a buscar el problema a lugares que no tienen nada que ver:
  //
  //   - el chip no contesta por I2C  -> bus, dirección o pines: es cableado.
  //   - contesta y la bandera OS está puesta -> el oscilador SE PARÓ, o sea
  //     que el RTC se quedó sin alimentación. Eso es hardware de respaldo, no
  //     software, y es lo que pasa en cada apagado si nada sostiene el chip.
  //   - contesta con una hora buena pero absurda (2000-01-01 es el arranque de
  //     fábrica del PCF85063) -> nunca se lo puso en hora.
  //
  // El aparato no puede arreglar ninguno solo, pero decir cuál es lo que separa
  // "hay que revisar la pila" de "hay que sincronizar una vez".
  if (!_available) {
    LOG_ERR("CLK", "el RTC no contesta por I2C: el reloj del sistema queda en 1970");
    return false;
  }
  time_t epoch = 0;
  if (!getEpochUtc(epoch)) {
    LOG_ERR("CLK", "el RTC se quedó sin alimentación (oscilador parado): perdió la hora");
    return false;
  }
  if (epoch < CREIBLE_DESDE) {
    LOG_ERR("CLK", "el RTC anda pero nunca se lo puso en hora (epoch %lld)", (long long)epoch);
    return false;
  }
  if (!applySystemClock(epoch)) return false;
  LOG_INF("CLK", "reloj del sistema en hora desde el RTC (epoch %lld)", (long long)epoch);
  return true;
}
