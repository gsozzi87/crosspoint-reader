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
//   - el timer, y SOLO para no pasarse del próximo recordatorio o del fin del
//     temporizador. Si no hay nada armado no hay timer y el reposo dura lo que
//     dure: es el sueño más profundo que se puede tener sin perder el estado.
//
// Lo que NO despierta: el movimiento. Hasta 1.5.71 el ciclo duraba 2 s y cada
// vez miraba el acelerómetro para encenderse solo al levantarlo; con el umbral
// que fuera, el aparato entraba y salía del reposo cada dos segundos para
// siempre (el log estaba lleno de "despertó por movimiento tras 2000 ms") y el
// reposo no existía en la práctica. Se sacó por decisión del usuario: "no
// pretendo despertarlo con el IMU, SIEMPRE APRETARE UN BOTON".
//
// A diferencia del deep sleep, acá NO hace falta que el pin sea RTC GPIO: el
// light sleep del ESP32-S3 despierta con cualquier GPIO. Eso es justamente lo
// que destraba la alarma del RTC (GPIO45, que no es RTC GPIO y por eso no
// servía para el deep sleep) y el botón PWR por la IRQ del PMIC (GPIO38).
class IdleSleep {
 public:
  // Cuánto se queda quieto antes de reposar. No es un ajuste del usuario a
  // propósito: es corto porque volver no cuesta nada, y si se nota es un error.
  // Bajó de 45 a 30 s en 1.5.71: sin el sondeo del acelerómetro, entrar al
  // reposo dejó de costar nada, así que conviene entrar antes.
  static constexpr unsigned long REST_AFTER_MS = 30000;
  // Por debajo de esto no vale la pena reposar: entrar y salir cuesta más que
  // lo que se ahorra, y con una alarma vencida que la pantalla de turno no
  // atiende (el lector, a propósito) el tope sale 1 ms y el aparato giraba
  // entrando y saliendo del light sleep sin parar.
  static constexpr unsigned long MIN_REST_MS = 500;

  enum class Woke : uint8_t {
    NotSlept,  // no se durmió (bloqueado, apagado, o todavía no toca)
    Button,    // un botón, el PWR o el RTC: hay que atender
    Rejected,  // el kernel no dejó dormir (un pin ya estaba en el nivel de
               // despertar). NO es actividad de nadie: si el llamador lo trata
               // como tal, un pin trabado en bajo reinicia el contador de ocio
               // en cada pasada y el deep sleep no llega nunca.
    Timer,     // venció el tope del ciclo: se sigue reposando
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
  void probeRtcInt();

  bool available_ = false;
  bool resting_ = false;
  bool rtcIntUsable_ = false;
  int8_t rtcIntPin_ = -1;
  uint64_t buttonMask_ = 0;  // pines que despiertan por nivel bajo
  unsigned long capMs_ = 0;
  uint32_t cycles_ = 0;
  unsigned long restedMs_ = 0;
  uint16_t rejects_ = 0;  // rechazos seguidos de esp_light_sleep_start()
};

extern IdleSleep IDLE_SLEEP;
