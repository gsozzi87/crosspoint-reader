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

  // Umbrales de los gestos, en mg / dps / ms. Públicos porque la pantalla de
  // diagnóstico (Ajustes → Movimiento) los muestra como referencia de lectura
  // de los medidores: si se afinan acá y allá quedan literales viejos, la
  // pantalla miente.
  static constexpr int TH_TILT_MG = 550;
  static constexpr int TH_SHAKE_MG = 420;
  static constexpr int TH_ROTATE_DPS = 120;
  static constexpr int TH_LEVEL_MG = 150;
  static constexpr int TH_STILL_MG = 20;
  static constexpr int TH_DEBOUNCE_MS = 350;

  // La enciende la pantalla de diagnóstico mientras está abierta: con los
  // gestos apagados el chip no se lee, y ahí los medidores quedan en cero justo
  // cuando hay que revisar si el sensor está vivo.
  void setDiagnostics(const bool on) { diagnostics_ = on; }

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
  // Por qué NO contestó, para mostrarlo en la pantalla de diagnóstico. Depender
  // del log para esto no sirve: el log viaja al servidor y el servidor puede
  // estar caído, sin vincular o sin red, que es justo cuando uno necesita
  // diagnosticar. nullptr si contestó bien.
  const char* tapFailure() const { return tapFailure_; }

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
  bool diagnostics_ = false;
  bool tapTrusted_ = false;
  // EL REGISTRO DE GOLPES DEL QMI8658 QUEDA LATCHEADO: guarda el último evento
  // y NO se limpia al leerlo. Preguntarle "¿hubo doble golpe?" cada 80 ms
  // devuelve que sí para siempre desde el primero, así que con la lista blanca
  // fuera (1.5.55) el aparato se metía solo en Hablar doce veces por segundo.
  // Se emite sólo cuando el byte CAMBIA (evento nuevo) y con un tiempo muerto,
  // y además hay que haber visto un sacudón de verdad en el acelerómetro: un
  // golpe mueve la lectura, un registro viejo no.
  uint8_t lastTapStatus_ = 0;
  bool tapStatusSeen_ = false;
  unsigned long lastTapEmitMs_ = 0;
  unsigned long lastBigMoveMs_ = 0;
  const char* tapFailure_ = nullptr;
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
