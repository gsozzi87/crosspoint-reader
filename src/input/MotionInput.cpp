#include "MotionInput.h"

#include <HalTiltSensor.h>
#include <Logging.h>

#include <cmath>

#include "HubStore.h"

MotionInput MOTION;

namespace {
constexpr const char* TAG = "MOTION";

// Umbrales, en g y deg/s. Salen del proyecto de referencia en Rust para esta
// misma placa (imu_events), que es el único número medido en este hardware que
// tenemos; el resto es geometría.
// Los seis que la pantalla de diagnóstico muestra viven en MotionInput.h (en
// mg / dps / ms): acá se convierten a g, que es la unidad de las cuentas.
constexpr float TILT_ON = MotionInput::TH_TILT_MG / 1000.0f;  // pasa de acá y es una inclinación
constexpr float TILT_OFF = 0.25f;   // vuelve de acá y se puede inclinar de nuevo
constexpr float SHAKE_DEV = MotionInput::TH_SHAKE_MG / 1000.0f;  // desvío de 1 g que cuenta como sacudón
constexpr float ROTATE_DPS = MotionInput::TH_ROTATE_DPS;
constexpr float LEVEL_FLAT = MotionInput::TH_LEVEL_MG / 1000.0f;  // x e y por debajo de esto = apoyado
constexpr float FACE_DOWN_N = -0.70f;
constexpr float FACE_UP_N = -0.10f;
constexpr float FACE_DOWN_SIDE = 0.30f;
constexpr float STILL_DELTA = MotionInput::TH_STILL_MG / 1000.0f;  // suma de cambios por muestra para "quieto"
constexpr unsigned long FACE_DOWN_STILL_MS = 500;
constexpr unsigned long DEBOUNCE_MS = MotionInput::TH_DEBOUNCE_MS;
constexpr unsigned long SHAKE_WINDOW_MS = 400;
constexpr uint8_t SHAKE_HITS = 2;

// Parámetros del motor de golpes del chip (hoja de datos 10.2/10.3), pensados
// para un golpe con la yema sobre la tapa a 250 Hz de muestreo.
constexpr uint8_t TAP_PRIORITY = 4;      // Z > X > Y: el golpe entra por la tapa
constexpr uint8_t TAP_PEAK_WINDOW = 10;  // muestras
constexpr uint16_t TAP_WINDOW = 25;
constexpr uint16_t TAP_DTAP_WINDOW = 125;  // medio segundo a 250 Hz
constexpr float TAP_ALPHA = 0.0625f;
constexpr float TAP_GAMMA = 0.25f;
constexpr float TAP_PEAK_MAG = 0.8f;  // g^2
constexpr float TAP_UDM = 0.4f;       // g

// CTRL2: ±8 g (2) a 250 Hz (5). El motor de golpes pide 200 Hz para arriba, y
// un golpe seco pasa holgado de 2 g, así que el fondo de escala chico saturaría
// justo donde hay que medir.
constexpr uint8_t ACCEL_FS_8G = 2;
constexpr uint8_t ACCEL_ODR_250HZ = 5;
}  // namespace

void MotionInput::begin() {
  available_ = false;
  tapTrusted_ = false;
  if (!halTiltSensor.isAvailable()) {
    LOG_INF(TAG, "sin IMU en esta placa: los gestos quedan apagados");
    return;
  }
  Imu& imu = halTiltSensor.imu();
  // halTiltSensor.begin() deja el chip apagado del todo (se enciende solo si el
  // usuario usa el giro para pasar página). Los gestos lo quieren siempre
  // muestreando, pero sin giróscopo.
  if (!imu.setAccelConfig(ACCEL_FS_8G, ACCEL_ODR_250HZ)) {
    LOG_ERR(TAG, "no se pudo configurar el acelerómetro");
    return;
  }
  if (!imu.setMode(Imu::Mode::AccelOnly)) {
    LOG_ERR(TAG, "no se pudo encender el acelerómetro");
    return;
  }
  gyroOn_ = false;
  available_ = true;

  // El doble golpe lo detecta el propio chip: a 80 ms de consulta no hay forma
  // de ver un golpe de 10 ms desde el loop.
  // Los tres pasos se loguean POR SEPARADO. Antes los tres colapsaban en un
  // "no contestó" y desde el aparato no había forma de saber si lo que falló
  // fue el autochequeo (chip mal identificado), la carga de parámetros (el
  // diálogo CTRL9, que es lo que más se cuelga) o el encendido del motor.
  if (!imu.selfCheckPassed()) {
    LOG_ERR(TAG, "golpes: el autochequeo del chip no pasó");
  } else if (!imu.configureTap(TAP_PRIORITY, TAP_PEAK_WINDOW, TAP_WINDOW, TAP_DTAP_WINDOW, TAP_ALPHA, TAP_GAMMA,
                               TAP_PEAK_MAG, TAP_UDM)) {
    LOG_ERR(TAG, "golpes: no se pudieron cargar los parámetros (diálogo CTRL9)");
  } else if (!imu.enableTap(true)) {
    LOG_ERR(TAG, "golpes: el motor no se pudo encender (CTRL8)");
  } else {
    tapTrusted_ = true;
    LOG_INF(TAG, "golpes: el motor del chip contestó");
  }
  const auto& map = HUB_STORE.imuMap;
  LOG_INF(TAG, "gestos %s, ejes n=%u(%d) x=%u(%d) y=%u(%d)%s", HUB_STORE.motionGestures ? "encendidos" : "apagados",
          map.normalAxis, map.normalSign, map.xAxis, map.xSign, map.yAxis, map.ySign,
          map.calibrated ? " (calibrados)" : " (de fábrica)");
  lastPollMs_ = millis();
}

void MotionInput::setGyro(const bool on) {
  if (!available_ || on == gyroOn_) return;
  if (!halTiltSensor.imu().setMode(on ? Imu::Mode::AccelGyro : Imu::Mode::AccelOnly)) return;
  gyroOn_ = on;
}

bool MotionInput::readRaw(RawSample& out) {
  Imu::Sample s;
  if (!halTiltSensor.imu().read(s)) return false;
  out.v[0] = s.ax;
  out.v[1] = s.ay;
  out.v[2] = s.az;
  out.g[0] = s.gx;
  out.g[1] = s.gy;
  out.g[2] = s.gz;
  return true;
}

void MotionInput::toScreen(const RawSample& raw, Reading& out) const {
  const auto& m = HUB_STORE.imuMap;
  out.x = raw.v[m.xAxis] * m.xSign;
  out.y = raw.v[m.yAxis] * m.ySign;
  out.n = raw.v[m.normalAxis] * m.normalSign;
  out.gx = raw.g[m.xAxis] * m.xSign;
  out.gy = raw.g[m.yAxis] * m.ySign;
  out.gn = raw.g[m.normalAxis] * m.normalSign;
  out.magnitude = sqrtf(raw.v[0] * raw.v[0] + raw.v[1] * raw.v[1] + raw.v[2] * raw.v[2]);
  out.valid = true;
}

void MotionInput::emit(const Event e) {
  // Un evento por vez: el que llega pisa al anterior sin consumir sólo si el
  // anterior ya se hizo viejo, así una pantalla que no mira los gestos no deja
  // uno colgado para siempre.
  const unsigned long now = millis();
  if (pending_ != Event::None && now - lastEventAt_ < 1500) return;
  pending_ = e;
  lastEvent_ = e;
  lastEventAt_ = now;
  LOG_DBG(TAG, "%s (x=%.2f y=%.2f n=%.2f)", name(e), last_.x, last_.y, last_.n);
}

void MotionInput::poll() {
  // La pantalla de diagnóstico necesita leer el sensor aunque los gestos estén
  // apagados: si no, muestra ceros congelados justo cuando hay que revisar si
  // el chip está vivo.
  if (!available_ || (!HUB_STORE.motionGestures && !diagnostics_)) return;
  const unsigned long now = millis();
  if (now - lastPollMs_ < POLL_MS) return;
  lastPollMs_ = now;

  // El chip pudo quedar apagado por el camino del giro para pasar página
  // (HalTiltSensor lo duerme cuando esa opción se apaga): se vuelve a encender.
  Imu& imu = halTiltSensor.imu();
  if (imu.mode() == Imu::Mode::Off) {
    imu.setMode(gyroOn_ ? Imu::Mode::AccelGyro : Imu::Mode::AccelOnly);
    return;  // una muestra de descarte mientras arranca
  }

  RawSample raw;
  if (!readRaw(raw)) return;
  Reading r;
  toScreen(raw, r);

  // Quietud: cuánto cambió la lectura respecto de la anterior. Sirve para el
  // boca abajo (que exige estar apoyado, no de paso) y para el horizontal.
  stillness_ = fabsf(r.x - lastX_) + fabsf(r.y - lastY_) + fabsf(r.n - lastN_);
  lastX_ = r.x;
  lastY_ = r.y;
  lastN_ = r.n;
  last_ = r;

  const bool debounced = now - lastEventAt_ < DEBOUNCE_MS;

  // --- 1. Sacudida: lo más urgente, porque es "pará todo" -------------------
  const float deviation = fabsf(r.magnitude - 1.0f);
  if (deviation > SHAKE_DEV) {
    if (!shakeHits_ || now - shakeFirstAt_ > SHAKE_WINDOW_MS) {
      shakeHits_ = 1;
      shakeFirstAt_ = now;
    } else if (++shakeHits_ >= SHAKE_HITS) {
      shakeHits_ = 0;
      tilted_ = true;  // el sacudón deja el aparato en cualquier posición
      if (!debounced) {
        emit(Event::Shake);
        return;
      }
    }
  } else if (shakeHits_ && now - shakeFirstAt_ > SHAKE_WINDOW_MS) {
    shakeHits_ = 0;
  }

  // --- 2. Doble golpe: lo dice el motor del chip ----------------------------
  if (tapTrusted_) {
    uint8_t tap = 0;
    if (imu.readTapStatus(tap) && (tap & 0x03) == 0x02) {
      if (!debounced) {
        emit(Event::DoubleTap);
        return;
      }
    }
  }

  // --- 3. Giro sobre la normal (sólo con el giróscopo encendido) ------------
  if (gyroOn_ && fabsf(r.gn) > ROTATE_DPS && !debounced) {
    emit(Event::Rotate);
    return;
  }

  // --- 4. Inclinación, con histéresis --------------------------------------
  if (tilted_) {
    if (fabsf(r.x) < TILT_OFF && fabsf(r.y) < TILT_OFF) tilted_ = false;
  } else if (!debounced) {
    if (r.x > TILT_ON) {
      tilted_ = true;
      emit(Event::TiltRight);
      return;
    }
    if (r.x < -TILT_ON) {
      tilted_ = true;
      emit(Event::TiltLeft);
      return;
    }
    if (r.y > TILT_ON) {
      tilted_ = true;
      emit(Event::TiltForward);
      return;
    }
    if (r.y < -TILT_ON) {
      tilted_ = true;
      emit(Event::TiltBack);
      return;
    }
  }

  // --- 5. Boca abajo: hay que apoyarlo y dejarlo quieto ---------------------
  const bool looksFaceDown = r.n <= FACE_DOWN_N && fabsf(r.x) < FACE_DOWN_SIDE && fabsf(r.y) < FACE_DOWN_SIDE;
  if (!faceDown_) {
    if (looksFaceDown && stillness_ <= STILL_DELTA) {
      if (!faceDownSince_) faceDownSince_ = now;
      if (now - faceDownSince_ >= FACE_DOWN_STILL_MS) {
        faceDown_ = true;
        faceDownSince_ = 0;
        emit(Event::FaceDown);
        return;
      }
    } else {
      faceDownSince_ = 0;
    }
  } else if (r.n >= FACE_UP_N) {
    faceDown_ = false;
    emit(Event::FaceUp);
    return;
  }

  // --- 6. Horizontal y quieto ----------------------------------------------
  const bool looksLevel = r.n > 0.7f && fabsf(r.x) < LEVEL_FLAT && fabsf(r.y) < LEVEL_FLAT;
  if (looksLevel && !level_ && stillness_ <= STILL_DELTA) {
    level_ = true;
    if (!debounced) emit(Event::Level);
  } else if (!looksLevel) {
    level_ = false;
  }
}

bool MotionInput::take(const Event want) {
  if (pending_ != want) return false;
  pending_ = Event::None;
  return true;
}

MotionInput::Event MotionInput::takeAny() {
  const Event e = pending_;
  pending_ = Event::None;
  return e;
}

const char* MotionInput::name(const Event e) {
  switch (e) {
    case Event::TiltLeft: return "inclinar izquierda";
    case Event::TiltRight: return "inclinar derecha";
    case Event::TiltForward: return "inclinar adelante";
    case Event::TiltBack: return "inclinar atras";
    case Event::Shake: return "sacudir";
    case Event::Rotate: return "girar";
    case Event::Level: return "horizontal";
    case Event::FaceDown: return "boca abajo";
    case Event::FaceUp: return "boca arriba";
    case Event::DoubleTap: return "doble golpe";
    case Event::None: break;
  }
  return "-";
}

// --- Calibración -------------------------------------------------------------
// Cada paso mira qué eje del chip tiene casi toda la gravedad y con qué signo.
// No se le pide precisión al usuario: con que el eje dominante sea el correcto
// alcanza, y eso se cumple con cualquier inclinación de más de 45 grados.
namespace {
// Devuelve el eje con mayor módulo y su signo, o -1 si ninguno domina.
int dominantAxis(const float v[3], int& sign) {
  int best = 0;
  for (int i = 1; i < 3; ++i) {
    if (fabsf(v[i]) > fabsf(v[best])) best = i;
  }
  if (fabsf(v[best]) < 0.5f) return -1;  // ni apoyado ni inclinado de verdad
  sign = v[best] >= 0 ? 1 : -1;
  return best;
}
}  // namespace

bool MotionInput::calibrateFlat() {
  if (!available_) return false;
  RawSample raw;
  if (!readRaw(raw)) return false;
  int sign = 1;
  const int axis = dominantAxis(raw.v, sign);
  if (axis < 0) return false;
  HUB_STORE.imuMap.normalAxis = static_cast<uint8_t>(axis);
  HUB_STORE.imuMap.normalSign = static_cast<int8_t>(sign);
  return true;
}

bool MotionInput::calibrateRight() {
  if (!available_) return false;
  RawSample raw;
  if (!readRaw(raw)) return false;
  int sign = 1;
  const int axis = dominantAxis(raw.v, sign);
  if (axis < 0 || axis == HUB_STORE.imuMap.normalAxis) return false;
  // Inclinado hacia la DERECHA, la gravedad tira hacia la izquierda de la
  // pantalla: el eje x de pantalla es el opuesto al que domina.
  HUB_STORE.imuMap.xAxis = static_cast<uint8_t>(axis);
  HUB_STORE.imuMap.xSign = static_cast<int8_t>(-sign);
  return true;
}

bool MotionInput::calibrateToward() {
  if (!available_) return false;
  RawSample raw;
  if (!readRaw(raw)) return false;
  int sign = 1;
  const int axis = dominantAxis(raw.v, sign);
  if (axis < 0 || axis == HUB_STORE.imuMap.normalAxis || axis == HUB_STORE.imuMap.xAxis) return false;
  HUB_STORE.imuMap.yAxis = static_cast<uint8_t>(axis);
  HUB_STORE.imuMap.ySign = static_cast<int8_t>(-sign);
  HUB_STORE.imuMap.calibrated = true;
  return true;
}
