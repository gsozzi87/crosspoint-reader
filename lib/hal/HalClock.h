#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  // Último epoch que el RTC contestó de verdad, y cuándo. El bus de sensores es
  // UNO SOLO (RTC 0x51, SHTC3 0x70, IMU 0x6B) y el IMU se consulta cada 80 ms,
  // así que una transacción puede perderse. `getTime()` ya tenía red —se queda
  // con la última hora buena—, `getEpochUtc()` no tenía ninguna: un choque y
  // todo lo que la usa (la agenda, el diario de batería, `cp.time()`, la
  // pantalla de sueño) contestaba "el aparato no está en hora" con el RTC
  // andando perfecto.
  mutable time_t _cachedEpoch = 0;
  mutable unsigned long _cachedEpochMs = 0;
  // Cuánto se puede estirar ese epoch con `millis()`. Un minuto alcanza de
  // sobra para tapar un choque de bus y es poco para que el error importe:
  // más que eso ya no es un choque, es el RTC que dejó de contestar, y eso
  // hay que decirlo y no disimularlo.
  static constexpr unsigned long EPOCH_CACHE_MAX_MS = 60000;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // UTC epoch seconds from the RTC. False when there is no RTC or it was never set.
  bool getEpochUtc(time_t& out) const;
  // Sets the RTC from a UTC epoch (e.g. the server's clock when NTP is not reachable).
  bool setFromEpochUtc(time_t epoch);

  // Le pasa la hora del RTC al RELOJ DEL SISTEMA (`settimeofday`).
  //
  // Son dos relojes distintos y hasta 1.5.79 sólo se mantenía uno. El RTC
  // guardaba la hora bien, pero `time(nullptr)` arrancaba en 1970 en cada
  // arranque porque nadie se la pasaba. Eso no se notaba en ningún lado... y
  // es exactamente contra lo que se valida la fecha de un certificado TLS: sin
  // esto, verificar el certificado del servidor es imposible, porque todo
  // certificado del mundo parece "todavía no válido" en 1970.
  // Devuelve false si el RTC no contesta o si lo que tiene no es creíble.
  // Loguea CUÁL de los tres motivos fue: el chip no contesta por I2C, el
  // oscilador se paró (se quedó sin alimentación) o nunca se lo puso en hora.
  bool applyToSystemClock() const;

  // `settimeofday` con un epoch ya validado. La usan applyToSystemClock() y
  // los dos caminos que ponen el RTC en hora (servidor y NTP): poner uno de
  // los dos relojes y no el otro es lo que dejaba `time(nullptr)` en 1970
  // durante toda la sesión justo después de sincronizar.
  static bool applySystemClock(time_t epoch);

  // ¿El reloj del sistema está en una fecha creíble? (después de 2024).
  static bool systemClockLooksSet();
};
