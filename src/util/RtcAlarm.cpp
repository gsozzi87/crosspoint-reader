#include "RtcAlarm.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>

RtcAlarm RTC_ALARM;

namespace {
constexpr const char* TAG = "RTCAL";

// Mapa de registros del PCF85063 (hoja de datos, tabla 3).
constexpr uint8_t REG_CONTROL_2 = 0x01;
constexpr uint8_t REG_SECOND_ALARM = 0x0B;  // 0x0B..0x0F: segundo, minuto, hora, día, día de semana

// Control_2: bit7 AIE (habilita la interrupción de alarma), bit6 AF (bandera).
// AF y TF se limpian escribiendo CERO y se conservan escribiendo UNO, así que
// para limpiar AF hay que dejar TF (bit3) en uno.
constexpr uint8_t C2_AIE = 0x80;
constexpr uint8_t C2_AF = 0x40;
constexpr uint8_t C2_TF = 0x08;

// bit7 de cada registro de alarma: 1 = ese campo NO se compara.
constexpr uint8_t ALARM_OFF = 0x80;

// Más de esto no entra: el chip compara día del mes, no fecha completa.
constexpr int32_t MAX_AHEAD_S = 27L * 86400L;

uint8_t toBcd(const uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
}  // namespace

void RtcAlarm::ensureWire() {
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.i2cSda < 0 || s.i2cScl < 0) return;
  // El bus ya lo levantó el RTC del SDK (o el SHTC3): begin() de nuevo sobre los
  // mismos pines es inocuo y cubre el orden de arranque.
  Wire.begin(s.i2cSda, s.i2cScl, s.i2cHz);
}

bool RtcAlarm::readReg(const uint8_t reg, uint8_t& out) const {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(addr_), 1) != 1) return false;
  out = static_cast<uint8_t>(Wire.read());
  return true;
}

bool RtcAlarm::writeReg(const uint8_t reg, const uint8_t val) const {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void RtcAlarm::begin() {
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.rtcAddr == 0 || s.rtcType != BoardConfig::RtcType::Pcf85063) return;
  addr_ = s.rtcAddr;
  ensureWire();
  uint8_t c2 = 0;
  if (!readReg(REG_CONTROL_2, c2)) {
    LOG_ERR(TAG, "el RTC no contesta en 0x%02X: sin alarma", addr_);
    return;
  }
  available_ = true;
  // Arrancar siempre en limpio: una alarma que quedó armada de la sesión
  // anterior dejaría el INT en bajo y el reposo no podría entrar nunca.
  disarm();
  LOG_INF(TAG, "alarma del RTC lista (Control_2 = 0x%02X)", c2);
}

// REV-077: apagar AIE es lo PRIMERO y lo que de verdad desarma. Los cinco
// registros de comparación pueden quedar como estén: con AIE en cero el chip no
// tira la línea, y además se los pone en ALARM_OFF igual. El orden importa
// porque `armAt()` reprograma campo por campo, y mientras eso pasa una alarma
// vieja con AIE puesto puede coincidir con una hora a medio escribir.
bool RtcAlarm::disableInterrupt() {
  uint8_t c2 = 0;
  if (!readReg(REG_CONTROL_2, c2)) return false;
  // AIE fuera y AF limpia; TF en uno para no pisarla.
  return writeReg(REG_CONTROL_2, static_cast<uint8_t>((c2 & ~(C2_AIE | C2_AF)) | C2_TF));
}

void RtcAlarm::disarm() {
  if (!available_) return;
  // REV-077: primero AIE, después los campos. Al revés, entre el primer campo y
  // el último hay una ventana con la alarma todavía habilitada.
  bool ok = disableInterrupt();
  for (uint8_t i = 0; i < 5; ++i) {
    if (!writeReg(static_cast<uint8_t>(REG_SECOND_ALARM + i), ALARM_OFF)) ok = false;
  }
  // REV-077: `armedAt_ = 0` se ponía pasara lo que pasara, así que el software
  // podía decir "no hay alarma" con el chip todavía armado — y de ahí un
  // despertar a una hora que no le corresponde a nada, o un INT abajo que hace
  // rechazar el reposo en bucle. Si no se pudo confirmar, el estado queda
  // DESCONOCIDO: `armedAt_` se deja en -1, que no coincide con ningún epoch
  // válido, así que el próximo `armAt()` no se saltea por el atajo de "ya está".
  if (ok) {
    armedAt_ = 0;
  } else {
    armedAt_ = -1;
    LOG_ERR(TAG, "no se pudo desarmar la alarma del RTC: el chip puede seguir armado");
  }
}

bool RtcAlarm::armAt(const time_t epochUtc, const time_t nowUtc) {
  if (!available_ || epochUtc <= 0) return false;
  // Sin mes en los registros, el chip no distingue una fecha de más de un mes:
  // el 3 a las 9 se compara igual que el 3 del mes que viene a las 9. A más de
  // 27 días no se arma nada y manda el ciclo del reposo.
  if (nowUtc > 0 && epochUtc - nowUtc > MAX_AHEAD_S) {
    disarm();
    return false;
  }
  if (armedAt_ == epochUtc) return true;  // ya está: no se toca el bus

  struct tm t = {};
  gmtime_r(&epochUtc, &t);  // el RTC guarda UTC (ver HalClock::setFromEpochUtc)

  // REV-077: AIE FUERA ANTES DE TOCAR LOS CAMPOS. Los cinco se escriben en
  // cadena, así que entre el segundo nuevo y el día nuevo el chip tiene una
  // hora que no es ni la vieja ni la nueva — y con la alarma anterior todavía
  // habilitada, esa mezcla puede coincidir y disparar. Mientras dura la
  // reprogramación el estado es DESCONOCIDO y así queda anotado: si algo falla
  // a mitad, `armedAt_` no puede seguir diciendo el objetivo viejo.
  if (!disableInterrupt()) {
    LOG_ERR(TAG, "no se pudo apagar AIE antes de reprogramar: no se toca la alarma");
    return false;
  }
  armedAt_ = -1;

  // Segundo, minuto, hora y día se comparan; el día de la semana no (si no, la
  // alarma pediría que coincidan las dos cosas y no sonaría casi nunca).
  const bool ok = writeReg(REG_SECOND_ALARM, toBcd(static_cast<uint8_t>(t.tm_sec))) &&
                  writeReg(REG_SECOND_ALARM + 1, toBcd(static_cast<uint8_t>(t.tm_min))) &&
                  writeReg(REG_SECOND_ALARM + 2, toBcd(static_cast<uint8_t>(t.tm_hour))) &&
                  writeReg(REG_SECOND_ALARM + 3, toBcd(static_cast<uint8_t>(t.tm_mday))) &&
                  writeReg(REG_SECOND_ALARM + 4, ALARM_OFF);
  if (!ok) {
    // A medio escribir y con AIE ya fuera: el chip no puede disparar, pero los
    // campos son una mezcla. Se deja explícitamente desarmado.
    LOG_ERR(TAG, "no se pudo escribir la alarma: se desarma para no dejar una hora a medias");
    disarm();
    return false;
  }
  uint8_t c2 = 0;
  if (!readReg(REG_CONTROL_2, c2)) {
    disarm();
    return false;
  }
  // AIE adentro, AF limpia (arranca de cero), TF conservada.
  if (!writeReg(REG_CONTROL_2, static_cast<uint8_t>((c2 & ~C2_AF) | C2_AIE | C2_TF))) {
    disarm();
    return false;
  }
  armedAt_ = epochUtc;
  LOG_INF(TAG, "alarma armada para %04d-%02d-%02d %02d:%02d:%02d UTC", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
          t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

bool RtcAlarm::fired() {
  if (!available_) return false;
  uint8_t c2 = 0;
  if (!readReg(REG_CONTROL_2, c2)) return false;
  return (c2 & C2_AF) != 0;
}

void RtcAlarm::clearFlag() {
  if (!available_) return;
  uint8_t c2 = 0;
  if (!readReg(REG_CONTROL_2, c2)) return;
  writeReg(REG_CONTROL_2, static_cast<uint8_t>((c2 & ~C2_AF) | C2_TF));
}
