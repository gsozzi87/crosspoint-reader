#pragma once

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>

// El ES8311 a dormir antes del sueño profundo.
//
// El riel del códec (Audio_VCC, un ALDO del AXP2101) queda encendido toda la
// noche: el firmware no toca los rieles del PMIC. Y el códec nunca se apagaba:
// AudioManager::powerDown() sólo corta un riel por GPIO que en esta placa no
// existe (PIN_UNASSIGNED). O sea que el ES8311 pasaba la noche con el ADC, el
// DAC y la polarización analógica como los dejó la última reproducción: unos
// miliamperios de una batería que "estaba suspendida". Es la secuencia de
// suspensión del driver de Espressif (es8311_suspend en esp-adf): DAC y ADC a
// volumen cero, ADC y DAC apagados, sistema analógico apagado, y el GPIO del
// códec a bajo. Al despertar el aparato se reinicia y codecInit() lo vuelve a
// resetear (0x00 = 0x1F / 0x00 / 0x80), así que no hay nada que deshacer.
namespace codecsleep {

inline void es8311Suspend() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.output != BoardConfig::AudioOutput::I2sEs8311 || cfg.codecAddr == 0) return;
  static const uint8_t SEQ[][2] = {
      {0x32, 0x00},  // DAC volume
      {0x17, 0x00},  // ADC volume
      {0x0E, 0xFF},  // ADC power down
      {0x12, 0x02},  // DAC power down
      {0x14, 0x00},  // PGA / mic off
      {0x0D, 0xFA},  // analog system power down
      {0x15, 0x00},  // ADC
      {0x37, 0x08},  // DAC
      {0x45, 0x01},  // GP: codec GPIO low
  };
  int ok = 0;
  for (const auto& rv : SEQ) {
    Wire.beginTransmission(cfg.codecAddr);
    Wire.write(rv[0]);
    Wire.write(rv[1]);
    if (Wire.endTransmission() == 0) ++ok;
  }
  // Sin el bus arriba (los re-sleep del setup) las nueve fallan y no pasa nada.
  if (ok != static_cast<int>(sizeof(SEQ) / sizeof(SEQ[0]))) {
    LOG_INF("CODEC", "ES8311 a dormir: %d de %u registros escritos", ok, (unsigned)(sizeof(SEQ) / sizeof(SEQ[0])));
  }
}

}  // namespace codecsleep
