#pragma once

#include <Arduino.h>

#include <cstdint>

// ws397: reposo en tres etapas.
//
// Hasta 1.5.48 el aparato tenía dos estados y nada en el medio: despierto
// (~40 mA, el loop girando cada 10 ms) o deep sleep (un reset con arranque
// completo de por medio). Un aparato de tinta electrónica se pasa la vida en
// el hueco entre esos dos: la imagen ya está en el vidrio y no hay nada que
// hacer hasta que alguien lo toque.
//
// Esa es la etapa que agrega este módulo, copiada del `device_sleep_service` de
// folloup (el firmware de notas de voz para esta misma placa):
//
//   despierto ....... el loop normal
//   REPOSANDO ....... esp_light_sleep_start() en ciclos; ~240 uA, la RAM y el
//                     estado siguen vivos y la pantalla queda como estaba
//                     (el panel es biestable: retener no cuesta nada)
//   dormido ......... deep sleep, que sigue siendo un reset al despertar
//
// La diferencia con el deep sleep es que de acá se vuelve en menos de 10 ms y
// sin perder nada: ni la música que estaba armada, ni el libro abierto, ni la
// cuenta del cronómetro. Por eso se puede entrar mucho antes (45 s de quietud
// contra los minutos del deep sleep) y por eso vale la pena.
//
// Qué lo despierta:
//   - los cuatro botones (arriba 4, OK 5, abajo 6, BOOT 0), por nivel bajo
//   - la IRQ del PMIC (GPIO38): el botón PWR entra por ahí
//   - el INT del RTC (GPIO45) si la placa lo deja usable, para la alarma
//   - el timer, para mirar el acelerómetro cada REST_POLL_MS y para no pasarse
//     del próximo recordatorio
//
// A diferencia del deep sleep, acá NO hace falta que el pin sea RTC GPIO: el
// light sleep del ESP32-S3 despierta con cualquier GPIO. Eso es justamente lo
// que destraba la alarma del RTC (GPIO45, que no es RTC GPIO y por eso no
// servía para el deep sleep) y el botón PWR por la IRQ del PMIC (GPIO38).
class IdleSleep {
 public:
  // Cuánto se queda quieto antes de reposar. No es un ajuste del usuario a
  // propósito: es corto porque volver no cuesta nada, y si se nota es un error.
  static constexpr unsigned long REST_AFTER_MS = 45000;
  // Cada cuánto se despierta a mirar el acelerómetro mientras reposa. A 2 s el
  // ciclo despierto dura ~3 ms (una lectura I2C), o sea menos del 0,2 % del
  // tiempo: no mueve la aguja del consumo y hace que levantar el aparato lo
  // encienda solo.
  static constexpr unsigned long REST_POLL_MS = 2000;
  // Cuánto tiene que moverse para contar como "lo levantaron", en g sumando los
  // tres ejes contra la muestra anterior. Más fino que esto y lo despierta el
  // ruido del propio sensor (el piso del QMI8658 son ~5 mg por eje).
  static constexpr float WAKE_DELTA_G = 0.06f;

  enum class Woke : uint8_t {
    NotSlept,  // no se durmió (bloqueado, apagado, o todavía no toca)
    Button,    // un botón, el PWR o el RTC: hay que atender
    Motion,    // lo movieron
    Timer,     // vencimiento del ciclo: se sigue reposando
  };

  // Después de que los botones y el IMU estén arriba. Prueba los pines de
  // despertar y loguea cuáles quedaron.
  void begin();

  // Del loop de main.cpp. `idleMs` es cuánto hace que no pasa nada y `blocked`
  // lo que impide reposar ahora (música, grabación, red, USB...). Devuelve qué
  // lo despertó; el llamador decide si eso cuenta como actividad.
  Woke tick(unsigned long idleMs, bool blocked);

  // Que el próximo ciclo no dure más que esto (el próximo recordatorio o el fin
  // del temporizador). 0 = sin tope. Se pisa en cada tick.
  void capNextRest(unsigned long ms) { capMs_ = ms; }

  bool resting() const { return resting_; }
  // Cuántos ciclos de reposo lleva y cuánto tiempo estuvo reposando, para la
  // pantalla de memoria y el log.
  uint32_t cycles() const { return cycles_; }
  unsigned long restedMs() const { return restedMs_; }
  // El INT del RTC quedó usable como fuente de despertar (ver probeRtcInt()).
  bool rtcIntUsable() const { return rtcIntUsable_; }

 private:
  bool armWakeSources(unsigned long budgetMs);
  bool motionMoved();
  void probeRtcInt();

  bool available_ = false;
  bool resting_ = false;
  bool rtcIntUsable_ = false;
  int8_t rtcIntPin_ = -1;
  uint64_t buttonMask_ = 0;  // pines que despiertan por nivel bajo
  unsigned long capMs_ = 0;
  uint32_t cycles_ = 0;
  unsigned long restedMs_ = 0;
  // Última muestra del acelerómetro, para ver si se movió entre ciclos.
  float lastX_ = 0, lastY_ = 0, lastN_ = 0;
  bool haveSample_ = false;
};

extern IdleSleep IDLE_SLEEP;
