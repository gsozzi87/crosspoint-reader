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

bool wireReady = false;

void ensureWire() {
  if (wireReady) return;
  const auto& s = BoardConfig::ACTIVE.sensors;
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
}  // namespace

bool shtc3::read(float& celsius, float& humidity) {
  ensureWire();
  if (!command(CMD_WAKEUP)) return false;
  delayMicroseconds(300);
  if (!command(CMD_MEASURE)) {
    command(CMD_SLEEP);
    return false;
  }
  delay(13);  // medición normal: 12,1 ms como mucho
  uint8_t buf[6];
  const size_t got = Wire.requestFrom(static_cast<int>(ADDR), 6);
  if (got != 6) {
    command(CMD_SLEEP);
    return false;
  }
  for (uint8_t& b : buf) b = static_cast<uint8_t>(Wire.read());
  command(CMD_SLEEP);
  if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return false;
  const uint16_t rawT = static_cast<uint16_t>(buf[0] << 8 | buf[1]);
  const uint16_t rawH = static_cast<uint16_t>(buf[3] << 8 | buf[4]);
  celsius = -45.0f + 175.0f * static_cast<float>(rawT) / 65536.0f;
  humidity = 100.0f * static_cast<float>(rawH) / 65536.0f;
  return true;
}

float shtc3::cachedCelsius() {
  static float value = NAN;
  static unsigned long readAt = 0;
  const unsigned long now = millis();
  if (readAt != 0 && now - readAt < CACHE_MS) return value;
  readAt = now;
  float t = 0, h = 0;
  if (shtc3::read(t, h)) {
    value = t;
    LOG_DBG("SHTC3", "interior %.1f C, %.0f %% HR", t, h);
  } else {
    value = NAN;
  }
  return value;
}
