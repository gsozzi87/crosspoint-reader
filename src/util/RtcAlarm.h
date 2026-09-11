#pragma once

#include <Arduino.h>

#include <ctime>

// ws397: la alarma del PCF85063, por fin usable.
//
// El INT del RTC está en GPIO45, que NO es un RTC GPIO del ESP32-S3: por eso
// hasta 1.5.48 no servía para nada y los recordatorios dependían del timer del
// deep sleep. Lo que cambia con el reposo (`IdleSleep`) es que el light sleep
// del S3 despierta con CUALQUIER GPIO, sea RTC o no. Así que en reposo esta
// alarma sí despierta al aparato, y lo hace en el segundo exacto y sin que
// nadie tenga que estar recontando el tiempo que falta.
//
// Sigue sin servir para el deep sleep: eso lo arma `armReminderWake()` con el
// timer, y así queda.
//
// El chip no tiene mes en la alarma (segundo, minuto, hora, día y día de la
// semana, y nada más), así que una alarma armada vale para el próximo mes como
// mucho. Alcanza: se re-arma cada vez que cambia lo próximo que tiene que sonar.
class RtcAlarm {
 public:
  // Lee la identidad del chip y deja la alarma apagada y la bandera limpia.
  void begin();
  bool available() const { return available_; }

  // Arma para ese instante (epoch UTC, que es lo que guarda el RTC). `nowUtc`
  // es la hora de ahora, para medir qué tan lejos cae; 0 saltea ese control.
  // Re-armar con el mismo instante no toca el bus. Devuelve false si no se pudo
  // escribir o si la fecha cae a más de 27 días (no entra en los registros).
  bool armAt(time_t epochUtc, time_t nowUtc = 0);
  // Apaga la alarma y limpia la bandera. Idempotente.
  void disarm();
  // Para qué instante está armada (0 = ninguna).
  time_t armedAt() const { return armedAt_; }

  // La bandera AF del chip: la alarma venció. La línea INT se queda en bajo
  // hasta que se limpie, así que quien pregunte tiene que limpiar.
  bool fired();
  // Limpia AF (y suelta la línea INT) sin tocar el resto de Control_2.
  void clearFlag();

 private:
  bool readReg(uint8_t reg, uint8_t& out) const;
  bool writeReg(uint8_t reg, uint8_t val) const;
  void ensureWire();

  bool available_ = false;
  uint8_t addr_ = 0;
  time_t armedAt_ = 0;
};

extern RtcAlarm RTC_ALARM;
