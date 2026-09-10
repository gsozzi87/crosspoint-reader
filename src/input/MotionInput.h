#pragma once

#include <Arduino.h>

#include <cstdint>

// Movimiento como entrada, no como adorno.
//
// El aparato tiene tres botones y una palanca, así que cualquier gesto que el
// IMU pueda leer bien es un atajo que el usuario no tiene que buscar en un
// menú: darlo vuelta para callar un recordatorio, sacudirlo para cancelar,
// golpearlo dos veces para hablar. Todo eso sale de acá.
//
// El QMI8658 comparte su INT1 con el enable del amplificador (GPIO39), así que
// NO se puede usar la interrupción: se consulta desde el loop cada POLL_MS. A
// esa cadencia el acelerómetro solo consume ~142 uA (el giróscopo, ~750), y por
// eso el modo normal es AccelOnly y el giro se enciende únicamente donde hace
// falta (rotación).
//
// Los ejes del chip no coinciden con los de la pantalla y la orientación del
// montaje no está documentada: `HubStore::imuMap` guarda qué eje es la normal a
// la pantalla y con qué signo, y se calibra en Ajustes → Movimiento. Todo lo de
// acá trabaja en ejes de PANTALLA (x a la derecha, y hacia el usuario, n hacia
// afuera), nunca en ejes crudos del chip.
class MotionInput {
 public:
  enum class Event : uint8_t {
    None,
    TiltLeft,
    TiltRight,
    TiltForward,  // el borde de arriba se aleja (como pasar de página)
    TiltBack,
    Shake,
    Rotate,     // giro rápido sobre la normal (necesita el giróscopo)
    Level,      // volvió a quedar horizontal y quieto, boca arriba
    FaceDown,   // boca abajo y quieto
    FaceUp,     // se levantó de boca abajo
    DoubleTap,  // dos golpecitos, del motor del propio chip
  };

  // Cadencia de consulta. 80 ms es lo más lento que todavía ve una sacudida
  // (que dura ~300 ms) sin gastar batería ni pelearse con el bus I2C.
  static constexpr unsigned long POLL_MS = 80;

  // Después de que halTiltSensor.begin() haya encontrado el chip: comparte esa
  // misma instancia, porque el integrado es uno solo y dos configuraciones a la
  // vez se pisan.
  void begin();
  // Del loop de main.cpp, después de mappedInputManager.update().
  void poll();

  bool available() const { return available_; }
  // El motor de golpes del chip contestó el diálogo de CTRL9 y la gravedad que
  // se leyó al arrancar era plausible.
  bool tapTrusted() const { return tapTrusted_; }

  // Consume el evento pendiente si es el que se pide.
  bool take(Event want);
  // Consume el evento pendiente, sea cual sea.
  Event takeAny();
  Event pending() const { return pending_; }
  static const char* name(Event e);

  // Enciende o apaga el giróscopo (Rotate). Cuesta cinco veces más batería que
  // el acelerómetro solo, así que se prende donde se usa y se apaga al salir.
  void setGyro(bool on);
  bool gyroOn() const { return gyroOn_; }

  // Lo último que se leyó, ya en ejes de pantalla, para la pantalla de ajuste.
  struct Reading {
    float x = 0, y = 0, n = 0;     // g
    float gx = 0, gy = 0, gn = 0;  // deg/s
    float magnitude = 0;           // g
    bool valid = false;
  };
  const Reading& reading() const { return last_; }
  unsigned long lastEventAt() const { return lastEventAt_; }
  Event lastEvent() const { return lastEvent_; }

  // Calibración guiada (Ajustes → Movimiento). Cada paso toma la lectura de
  // ahora y deduce un eje; se guardan en HubStore::imuMap.
  bool calibrateFlat();    // apoyado boca arriba -> cuál es la normal y su signo
  bool calibrateRight();   // inclinado a la derecha -> eje x y signo
  bool calibrateToward();  // inclinado hacia el usuario -> eje y y signo

 private:
  struct RawSample {
    float v[3] = {0, 0, 0};
    float g[3] = {0, 0, 0};
  };
  bool readRaw(RawSample& out);
  void toScreen(const RawSample& raw, Reading& out) const;
  void emit(Event e);

  bool available_ = false;
  bool tapTrusted_ = false;
  bool gyroOn_ = false;
  unsigned long lastPollMs_ = 0;
  unsigned long lastEventAt_ = 0;
  Event pending_ = Event::None;
  Event lastEvent_ = Event::None;
  Reading last_;

  // Inclinación: histéresis para que una posición sostenida no dispare cien
  // eventos por segundo.
  bool tilted_ = false;
  // Sacudida: muestras seguidas fuera de 1 g.
  uint8_t shakeHits_ = 0;
  unsigned long shakeFirstAt_ = 0;
  // Boca abajo: hay que quedarse quieto medio segundo para que cuente.
  bool faceDown_ = false;
  unsigned long faceDownSince_ = 0;
  float lastX_ = 0, lastY_ = 0, lastN_ = 0;
  float stillness_ = 0;
  // Horizontal: se avisa una sola vez por vez que vuelve.
  bool level_ = false;
};

extern MotionInput MOTION;
