#include "Shtc3.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>

#include <cmath>

namespace {
constexpr uint8_t ADDR = 0x70;
constexpr uint16_t CMD_WAKEUP = 0x3517;
constexpr uint16_t CMD_SLEEP = 0xB098;
// Temperatura primero, precisión normal, sin clock stretching.
constexpr uint16_t CMD_MEASURE = 0x7866;
constexpr unsigned long CACHE_MS = 60000;
constexpr unsigned long MEASURE_MS = 15;  // el datasheet pide 12,1 como máximo
constexpr unsigned long RETRY_MS = 10000;

bool wireReady = false;
bool measuring = false;
unsigned long startedAt = 0;
unsigned long lastReadAt = 0;
unsigned long lastTryAt = 0;
float celsius = NAN;
float humidity = NAN;

void ensureWire() {
  if (wireReady) return;
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.i2cSda < 0 || s.i2cScl < 0) return;
  Wire.begin(s.i2cSda, s.i2cScl, s.i2cHz);
  wireReady = true;
}

bool command(const uint16_t cmd) {
  Wire.beginTransmission(ADDR);
  Wire.write(static_cast<uint8_t>(cmd >> 8));
  Wire.write(static_cast<uint8_t>(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

// CRC-8 del datasheet (polinomio 0x31, inicial 0xFF).
uint8_t crc8(const uint8_t* data, const size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

// Segundo tiempo: los seis bytes ya están listos en el sensor.
void collect() {
  measuring = false;
  uint8_t buf[6];
  const size_t got = Wire.requestFrom(static_cast<int>(ADDR), 6);
  command(CMD_SLEEP);
  if (got != 6) return;
  for (uint8_t& b : buf) b = static_cast<uint8_t>(Wire.read());
  if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
    LOG_ERR("SHTC3", "CRC mal: se descarta la medición");
    return;
  }
  const uint16_t rawT = static_cast<uint16_t>(buf[0] << 8 | buf[1]);
  const uint16_t rawH = static_cast<uint16_t>(buf[3] << 8 | buf[4]);
  celsius = -45.0f + 175.0f * static_cast<float>(rawT) / 65536.0f;
  humidity = 100.0f * static_cast<float>(rawH) / 65536.0f;
  lastReadAt = millis();
  if (lastReadAt == 0) lastReadAt = 1;
  LOG_DBG("SHTC3", "interior %.1f C, %.0f %% HR", celsius, humidity);
}
}  // namespace

void shtc3::tick() {
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.i2cSda < 0 || s.i2cScl < 0) return;
  const unsigned long now = millis();

  if (measuring) {
    if (now - startedAt >= MEASURE_MS) collect();
    return;
  }
  if (lastReadAt != 0 && now - lastReadAt < CACHE_MS) return;
  // Un sensor que no contesta no se consulta en cada pasada: cada intento son
  // dos transacciones I²C en el mismo bus que el RTC y el IMU.
  if (lastTryAt != 0 && now - lastTryAt < RETRY_MS) return;
  lastTryAt = now;

  ensureWire();
  if (!wireReady) return;
  if (!command(CMD_WAKEUP)) return;
  delayMicroseconds(300);  // el datasheet pide 240 us para despertar
  if (!command(CMD_MEASURE)) {
    command(CMD_SLEEP);
    return;
  }
  measuring = true;
  startedAt = now;
}

float shtc3::cachedCelsius() { return celsius; }
float shtc3::cachedHumidity() { return humidity; }
unsigned long shtc3::readAt() { return lastReadAt; }
