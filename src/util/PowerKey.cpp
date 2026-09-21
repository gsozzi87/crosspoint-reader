#include "PowerKey.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <soc/gpio_struct.h>

#include <algorithm>

PowerKey POWER_KEY;

namespace {
constexpr const char* TAG = "PWRKEY";

// AXP2101 registers (TG28 clone on the ws397, IC_TYPE 0x4A). Only the key,
// interrupt and power-off registers are touched: rails and charging stay as
// the PMIC configured them (CLAUDE.md rule).
constexpr uint8_t REG_IC_TYPE = 0x03;           // 0x4A
constexpr uint8_t REG_COMMON_CONFIG = 0x10;     // bit0 soft off, bit1 reset, bit2 PWRON shuts the PMIC
constexpr uint8_t REG_STATUS1 = 0x00;           // bit5 VBUS_GOOD: hay cable, cargue o no
constexpr uint8_t REG_PWRON_STATUS = 0x20;      // what powered the PMIC on (log only)
constexpr uint8_t REG_PWROFF_STATUS = 0x21;     // what powered it off last time (log only)
constexpr uint8_t REG_PWROFF_EN = 0x22;         // bit1 PWRON > OFFLEVEL powers off, bit0 1 = restart / 0 = off
constexpr uint8_t REG_IRQ_OFF_ON_LEVEL = 0x27;  // [1:0] PressOn [3:2] PressOff [5:4] IrqLevel
// Cargador de la PILA DE RESPALDO, la que sostiene al RTC cuando se corta el
// riel principal. ACÁ ESA PILA ES UNA CR2032, o sea PRIMARIA: no se carga.
// Meterle corriente la calienta, la hincha y la deja seca antes de tiempo — y
// una CR2032 seca es exactamente "el aparato vuelve del apagado sin hora".
constexpr uint8_t REG_MODULE_EN = 0x18;      // bit2 = cargador de la pila de respaldo
constexpr uint8_t MODULE_EN_BTN_BAT = 0x04;  // bit2
constexpr uint8_t REG_INTEN1 = 0x40;
constexpr uint8_t REG_INTEN2 = 0x41;
constexpr uint8_t REG_INTEN3 = 0x42;
constexpr uint8_t REG_INTSTS1 = 0x48;
constexpr uint8_t REG_INTSTS2 = 0x49;  // bit0 POSITIVE, bit1 NEGATIVE, bit2 LONG, bit3 SHORT (W1C)
constexpr uint8_t REG_INTSTS3 = 0x4A;
// Rieles: bit0-2 de 0x90 = ALDO1-3 (bit3 ALDO4, bit4-5 BLDO1-2, bit6 CPUSLDO,
// bit7 DLDO1); 0x92-0x94 = tensión de ALDO1-3, 0x1C = 3,3 V (0,5 V + n·0,1 V).
constexpr uint8_t REG_LDO_ONOFF0 = 0x90;
constexpr uint8_t ALDO123_MASK = 0x07;
constexpr uint8_t CHIP_ID = 0x4A;

// 0x27: IrqLevel 1 s (0 << 4): LONG latches one second into a hold, anchoring
// our hold timer even when the loop read the press edge late. PressOff 10 s
// (3 << 2): the PMIC's own hard cut sits far beyond the 3 s sleep hold, so it
// only ever acts as the emergency escape when the firmware is wedged (and it
// does cut the rail: alarms die, PWR 1 s brings it back). PressOn 1 s (2).
// Bits 7:6 are kept as found.
constexpr uint8_t IRQ_OFF_ON_LEVEL_VALUE = (0 << 4) | (3 << 2) | 2;  // 0x0E
constexpr uint8_t INTEN2_KEY_BITS = 0x0F;  // POSITIVE, NEGATIVE, LONG, SHORT; VBUS insert/remove left off

constexpr unsigned long POLL_FALLBACK_MS = 100;     // when the IRQ level cannot be trusted (rustmix cadence)
constexpr unsigned long I2C_RETRY_MS = 100;         // after a failed bus transaction
constexpr unsigned long IRQ_LEVEL_MS = 1000;        // the LONG threshold programmed above
constexpr unsigned long UNCONFIRMED_MAX_MS = 1500;  // a real press shows LONG by then
constexpr unsigned long STALE_PRESS_MS = 12000;     // the PMIC hard-cuts at 10 s: longer = missed release
constexpr unsigned long STALE_EDGE_MS = 5000;       // an edge older than this was latched while the loop was busy

// Learned edge polarity survives deep sleep (a chip reset with RTC RAM intact)
// so the first press after a wake is decoded at once. Lost with the rails,
// which is fine: it is re-learned from the first tap.
constexpr uint32_t EDGE_MEMORY_MAGIC = 0x50574B00;  // "PWK" + edge bit in the low byte
RTC_NOINIT_ATTR uint32_t s_edgeMemory;

// FALLING edge on the IRQ line = the PMIC just latched something. Only the
// time is recorded here (no I2C in an ISR); pump() picks it up as the true
// press/release instant when the loop is late (a refresh, a screenshot).
volatile unsigned long s_edgeAtMs = 0;
volatile int8_t s_irqPin = -1;
// Cuántas veces la ISR se encontró el pin configurado POR NIVEL. Tiene que ser
// cero: si no lo es, alguien armó el pin para despertar (LOW_LEVEL) sin soltar
// esta ISR primero, y sin esta guardia el aparato se colgaba en el watchdog de
// interrupciones (1.5.93 a 1.5.98). pump() lo dice en el log.
volatile uint32_t s_levelFixes = 0;

// Tipo de interrupción del pin, leído del registro (bits 7-9 de GPIO_PINn_REG).
inline bool IRAM_ATTR pinIsLevelTriggered(const int8_t pin) {
  const uint32_t t = GPIO.pin[pin].int_type;
  return t == GPIO_INTR_LOW_LEVEL || t == GPIO_INTR_HIGH_LEVEL;
}

void IRAM_ATTR onIrqFalling() {
  s_edgeAtMs = millis();
  // RED DE SEGURIDAD: una ISR de flanco sobre un pin por nivel es un bucle sin
  // salida (la línea sigue en bajo y la interrupción vuelve a entrar apenas
  // sale). No se puede levantar la línea desde acá —eso es I2C—, así que se
  // cambia el pin a flanco en el acto y se cuenta, para que el log lo diga.
  if (s_irqPin >= 0 && pinIsLevelTriggered(s_irqPin)) {
    GPIO.pin[s_irqPin].int_type = GPIO_INTR_NEGEDGE;
    ++s_levelFixes;
  }
}

const char* edgeName(uint8_t bit) {
  return bit == 0x01 ? "POSITIVE(bit0)" : bit == 0x02 ? "NEGATIVE(bit1)" : "unknown";
}
}  // namespace

bool PowerKey::readReg(uint8_t reg, uint8_t& out) const {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr_, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = static_cast<uint8_t>(Wire.read());
  return true;
}

bool PowerKey::writeReg(uint8_t reg, uint8_t val) const {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

// REV-066 / REV-070: escribir un registro del PMIC y COMPROBAR que quedó.
//
// El bus I2C es compartido (PMIC, RTC, códec, IMU, sensores) y un NACK suelto
// es una cosa real acá — el arranque ya reintenta tres veces la lectura del
// reloj por el mismo motivo. Hasta ahora estos `writeReg()` se llamaban sin
// mirar el retorno y sin releer, así que un fallo transitorio dejaba al
// firmware CREYENDO que había configurado el PMIC.
//
// `mask` son los bits que nos importan: en 0x10 los bits 0/1 son de disparo
// (apagado por software, reinicio) y no se pueden comparar en una relectura,
// y en 0x27 los bits 7:6 son de otra cosa y se conservan.
bool PowerKey::writeVerified(uint8_t reg, uint8_t val, uint8_t mask, const char* what) const {
  for (int intento = 1; intento <= 3; ++intento) {
    if (writeReg(reg, val)) {
      uint8_t leido = 0;
      if (readReg(reg, leido) && ((leido ^ val) & mask) == 0) return true;
    }
    delay(2);
  }
  LOG_ERR(TAG, "el PMIC no aceptó %s (reg 0x%02X = %02X): tres intentos y la relectura no coincide", what, reg, val);
  return false;
}

void PowerKey::begin() {
  available_ = false;
  const auto& board = BoardConfig::ACTIVE;
  if (board.board != BoardConfig::Board::WS397 || board.pmicIrq < 0 || board.batteryGauge.gaugeAddr == 0) return;
  irqPin_ = board.pmicIrq;
  addr_ = board.batteryGauge.gaugeAddr;

  // Open-drain IRQ, no external pull-up seen on the schematic: pull it up here.
  // deepSleep() isolates the pad (esp_sleep_config_gpio_isolate), so this has
  // to be repeated on every boot — which it is, begin() runs from setup().
  pinMode(irqPin_, INPUT_PULLUP);

  const auto& g = board.batteryGauge;
  Wire.begin(g.i2cSda, g.i2cScl, g.i2cHz);  // idempotent on an already-started bus

  // REV-070: se reintenta. Un solo NACK acá no es "no hay PMIC": es el bus
  // compartido. Y rendirse a la primera cuesta caro en esta placa, porque abajo
  // se vuelven a encender los rieles que `sleepNow()` cortó para dormir — sin
  // eso el panel y el audio se quedan sin alimentación y el aparato parece
  // muerto hasta un corte de corriente de verdad.
  uint8_t id = 0;
  bool contesta = false;
  for (int intento = 1; intento <= 3 && !contesta; ++intento) {
    contesta = readReg(REG_IC_TYPE, id) && id == CHIP_ID;
    if (!contesta) delay(2);
  }
  if (!contesta) {
    LOG_ERR(TAG,
            "!!! OJO: el PMIC de 0x%02X no contesta (id=%02X) tras 3 intentos: el boton PWR queda "
            "deshabilitado Y los rieles del panel y el audio NO se pueden volver a encender",
            addr_, id);
    return;
  }

  // Raw snapshot before touching anything: what the vendor/OTP/last firmware
  // left behind (PressOff, power-off enables, interrupt enables, pending status).
  // Los seis últimos son los RIELES (sólo lectura): 0x80 DCDC on/off, 0x90 LDO
  // on/off (bit0-3 ALDO1-4, bit4-5 BLDO1-2, bit6 CPUSLDO, bit7 DLDO1) y la
  // tensión de ALDO1-4 (0x92-0x95: 0x1C = 3,3 V). Según el esquemático, DC1 es
  // VCC3V3 (ESP, tarjeta, sensores) y ALDO1-3 alimentan EPD_VCC_AXP, Audio_VCC
  // y AudioCTR_VCC: lo que sigue prendido toda la noche en el sueño profundo.
  static const uint8_t SNAP_REGS[17] = {REG_COMMON_CONFIG,
                                        REG_PWRON_STATUS,
                                        REG_PWROFF_STATUS,
                                        REG_PWROFF_EN,
                                        REG_IRQ_OFF_ON_LEVEL,
                                        REG_INTEN1,
                                        REG_INTEN2,
                                        REG_INTEN3,
                                        REG_INTSTS1,
                                        REG_INTSTS2,
                                        REG_INTSTS3,
                                        0x80,
                                        0x90,
                                        0x92,
                                        0x93,
                                        0x94,
                                        0x95};
  for (size_t i = 0; i < sizeof(SNAP_REGS); ++i) {
    if (!readReg(SNAP_REGS[i], snapshot_[i])) snapshot_[i] = 0xEE;
  }
  // Los rieles que sleepNow() cortó para dormir vuelven ACÁ, antes de que
  // nadie toque el panel o el audio. Si ya están prendidos (primer arranque
  // después de la OTA, o un reinicio sin sueño) no se escribe nada.
  railsRestored_ = 0;
  uint8_t ldo = 0;
  if (readReg(REG_LDO_ONOFF0, ldo)) {
    const uint8_t missing = static_cast<uint8_t>(~ldo & ALDO123_MASK);
    if (missing != 0) {
      // SOLO el bit de encendido: la tensión de cada ALDO queda como la dejó la
      // fábrica. El volcado de 1.5.108 mostró ALDO1-4 = 1C 1C 19 0D, o sea que
      // ALDO3 va a 3,0 V y no a 3,3: escribirle 0x1C, como hacía 1.5.107,
      // habría subido 0,3 V a un chip que no sabemos cuál es. El PMIC no se
      // resetea con el ESP, así que esos registros no se pierden entre sueños.
      // REV-070: verificado. Si esta escritura falla y nadie la mira, el panel y
      // el audio se quedan sin alimentación y el aparato parece muerto — y la
      // única salida es el corte duro del PMIC, que es justo lo que REV-066
      // dice que puede no estar armado. Los dos defectos juntos son "no hay
      // forma de volver".
      if (writeVerified(REG_LDO_ONOFF0, static_cast<uint8_t>(ldo | ALDO123_MASK), ALDO123_MASK,
                        "el reencendido de los rieles ALDO1-3")) {
        railsRestored_ = missing;
      } else {
        LOG_ERR(TAG,
                "!!! OJO: los rieles del panel y el audio quedaron APAGADOS (faltan %02X): la pantalla "
                "y el sonido no van a funcionar en este arranque",
                missing);
      }
      delay(20);  // que los LDO suban y el panel salga de su reset antes de display.begin()
    }
  }
  snapshotValid_ = true;
  logSnapshot();

  // Key timings (read-modify-write keeps bits 7:6).
  // REV-066: las tres escrituras del ESCAPE FÍSICO se verifican. Mantener PWR
  // 10 s es la última salida cuando todo lo demás falló —el loop trabado, los
  // rieles sin volver (REV-070)—, así que el firmware no puede darla por
  // armada sin comprobarlo. Si alguna no queda, se dice y `hardOffArmed_`
  // queda en false.
  hardOffArmed_ = true;
  uint8_t v = 0;
  if (readReg(REG_IRQ_OFF_ON_LEVEL, v)) {
    if (!writeVerified(REG_IRQ_OFF_ON_LEVEL, static_cast<uint8_t>((v & 0xC0) | IRQ_OFF_ON_LEVEL_VALUE), 0x3F,
                       "el tiempo de PressOff (PWR 10 s)")) {
      hardOffArmed_ = false;
    }
  } else {
    hardOffArmed_ = false;
  }
  // Hard power-off by the key: both enables the driver knows about, and "off"
  // rather than "restart" (0x22 bit0). Which one the silicon honours is a
  // hardware check; setting both makes the 10 s escape real either way.
  // Bits 0/1 of 0x10 are write-to-trigger (soft power-off, reset) and must
  // never be echoed back from a read.
  // `mask` 0x04 y no el byte entero: los bits 0/1 de 0x10 son de disparo y no
  // se pueden comparar en una relectura.
  if (readReg(REG_COMMON_CONFIG, v)) {
    if (!writeVerified(REG_COMMON_CONFIG, static_cast<uint8_t>((v & static_cast<uint8_t>(~0x03)) | 0x04), 0x04,
                       "que PWRON pueda apagar el PMIC")) {
      hardOffArmed_ = false;
    }
  } else {
    hardOffArmed_ = false;
  }
  if (readReg(REG_PWROFF_EN, v)) {
    if (!writeVerified(REG_PWROFF_EN, static_cast<uint8_t>((v | 0x02) & static_cast<uint8_t>(~0x01)), 0x03,
                       "el apagado por PWRON (y no reinicio)")) {
      hardOffArmed_ = false;
    }
  } else {
    hardOffArmed_ = false;
  }
  if (!hardOffArmed_) {
    LOG_ERR(TAG,
            "!!! OJO: el escape fisico (PWR mantenido 10 s corta la corriente) NO quedo confirmado. "
            "Si el aparato se traba, puede no haber forma de apagarlo sin sacarle la bateria");
  }
  // NUNCA cargar la pila de respaldo: es una CR2032, primaria. El bit viene
  // apagado de fábrica, pero "de fábrica" acá es el OTP del clon del PMIC y un
  // gestor de arranque del vendor, y ninguno de los dos lo decidimos nosotros.
  // Si alguno lo dejó encendido, el aparato le estuvo metiendo corriente a una
  // celda que no la acepta desde el día uno — y una CR2032 castigada así se
  // seca en semanas, que es justo el síntoma de volver del apagado sin hora.
  // Se comprueba y se apaga en cada arranque, y se dice en el log si estaba
  // encendido: ese renglón es la diferencia entre "cambiá la pila" y "cambiá
  // la pila Y ya sabemos quién te la secó".
  if (readReg(REG_MODULE_EN, v) && (v & MODULE_EN_BTN_BAT) != 0) {
    writeReg(REG_MODULE_EN, static_cast<uint8_t>(v & static_cast<uint8_t>(~MODULE_EN_BTN_BAT)));
    LOG_ERR(TAG, "el PMIC estaba CARGANDO la pila de respaldo (0x18=%02X): es una CR2032, se apaga", v);
  }

  // Only the key may pull the IRQ line: anything else left enabled by OTP
  // (battery, charger, VBUS) would hold it LOW forever and turn the cheap
  // level check into an I2C read on every loop.
  writeReg(REG_INTEN1, 0x00);
  writeReg(REG_INTEN3, 0x00);
  writeReg(REG_INTEN2, INTEN2_KEY_BITS);

  // The PMIC does not reset with the ESP: whatever the key did while we were
  // asleep or booting (a press held through power-on, a press while asleep)
  // is still latched. Flush it before the state machine sees anything.
  flushAllStatus(nullptr);
  if (digitalRead(irqPin_) == LOW) {
    delay(2);
    flushAllStatus(nullptr);
  }
  pinLatchTrusted_ = digitalRead(irqPin_) == HIGH;
  if (!pinLatchTrusted_) {
    LOG_ERR(TAG, "IRQ line still LOW after clearing status: polling INTSTS2 every %lu ms instead", POLL_FALLBACK_MS);
  }

  uint8_t r27 = 0, r10 = 0, r22 = 0, r41 = 0;
  readReg(REG_IRQ_OFF_ON_LEVEL, r27);
  readReg(REG_COMMON_CONFIG, r10);
  readReg(REG_PWROFF_EN, r22);
  readReg(REG_INTEN2, r41);
  LOG_INF(TAG, "init: 27=%02X 10=%02X 22=%02X 41=%02X irq=GPIO%d level=%d latch=%s", r27, r10, r22, r41, irqPin_,
          digitalRead(irqPin_), pinLatchTrusted_ ? "pin" : "poll");

  if ((s_edgeMemory & 0xFFFFFF00u) == EDGE_MEMORY_MAGIC) {
    const uint8_t edge = static_cast<uint8_t>(s_edgeMemory & 0xFF);
    if (edge == BIT_POSITIVE || edge == BIT_NEGATIVE) {
      pressEdge_ = edge;
      edgeLearned_ = true;
      LOG_INF(TAG, "press edge = %s (from RTC memory)", edgeName(edge));
    }
  }

  s_edgeAtMs = 0;
  s_irqPin = irqPin_;
  attachInterrupt(digitalPinToInterrupt(irqPin_), onIrqFalling, FALLING);
  irqPaused_ = false;

  pressed_ = false;
  confirmed_ = false;
  longSeen_ = false;
  holdConsumed_ = false;
  shortPress_ = false;
  releasePending_ = false;
  pressStartMs_ = 0;
  heldMs_ = 0;
  lastPollMs_ = millis();
  available_ = true;
}

bool PowerKey::vbusPresent() const {
  if (!available_) return false;
  uint8_t st = 0;
  if (!readReg(REG_STATUS1, st)) return false;
  return (st & 0x20) != 0;
}

bool PowerKey::powerOff() const {
  if (!available_) return false;
  uint8_t cfg = 0;
  if (!readReg(REG_COMMON_CONFIG, cfg)) return false;
  // bit0 = soft off. Se deja el resto del registro como está: los rieles y la
  // carga los configuró el PMIC y no son nuestros (regla de CLAUDE.md).
  return writeReg(REG_COMMON_CONFIG, static_cast<uint8_t>(cfg | 0x01));
}

bool PowerKey::railsOffForSleep() const {
  if (!available_) return false;
  uint8_t ldo = 0;
  if (!readReg(REG_LDO_ONOFF0, ldo)) return false;
  if ((ldo & ALDO123_MASK) == 0) return true;
  const uint8_t after = static_cast<uint8_t>(ldo & ~ALDO123_MASK);
  if (!writeReg(REG_LDO_ONOFF0, after)) return false;
  uint8_t check = 0xEE;
  readReg(REG_LDO_ONOFF0, check);
  LOG_INF(TAG, "rieles ALDO1-3 cortados para dormir (90: %02X -> %02X)", ldo, check);
  return check == after;
}

bool PowerKey::railsCycle(uint16_t offMs) const {
  if (!available_) return false;
  uint8_t ldo = 0;
  if (!readReg(REG_LDO_ONOFF0, ldo)) return false;
  if (!writeReg(REG_LDO_ONOFF0, static_cast<uint8_t>(ldo & ~ALDO123_MASK))) return false;
  delay(offMs);
  // Sólo se vuelven a encender: las tensiones siguen siendo las de fábrica.
  const bool ok = writeReg(REG_LDO_ONOFF0, static_cast<uint8_t>(ldo | ALDO123_MASK));
  delay(50);
  uint8_t check = 0xEE;
  readReg(REG_LDO_ONOFF0, check);
  LOG_INF(TAG, "ciclo de corriente a ALDO1-3 (panel, códec, amplificador): %u ms apagados, 90: %02X -> %02X", offMs,
          ldo, check);
  return ok && (check & ALDO123_MASK) == ALDO123_MASK;
}

void PowerKey::logSnapshot() const {
  if (!snapshotValid_) return;
  LOG_INF(
      TAG,
      "AXP2101 regs at boot: 10=%02X 20=%02X 21=%02X 22=%02X 27=%02X 40=%02X 41=%02X 42=%02X 48=%02X 49=%02X 4A=%02X",
      snapshot_[0], snapshot_[1], snapshot_[2], snapshot_[3], snapshot_[4], snapshot_[5], snapshot_[6], snapshot_[7],
      snapshot_[8], snapshot_[9], snapshot_[10]);
  LOG_INF(TAG, "AXP2101 rieles: DCDC(80)=%02X LDO(90)=%02X ALDO1-4=%02X %02X %02X %02X (1C = 3,3 V)", snapshot_[11],
          snapshot_[12], snapshot_[13], snapshot_[14], snapshot_[15], snapshot_[16]);
  if (railsRestored_ != 0) {
    LOG_INF(TAG, "rieles ALDO1-3 re-encendidos al arrancar (estaban apagados: %02X): panel, códec y amplificador",
            railsRestored_);
  }
}

void PowerKey::flushAllStatus(const char* why) {
  uint8_t s1 = 0, s2 = 0, s3 = 0;
  readReg(REG_INTSTS1, s1);
  readReg(REG_INTSTS2, s2);
  readReg(REG_INTSTS3, s3);
  writeReg(REG_INTSTS1, 0xFF);
  writeReg(REG_INTSTS2, 0xFF);
  writeReg(REG_INTSTS3, 0xFF);
  if (why) LOG_INF(TAG, "%s: flushed sts1=%02X sts2=%02X sts3=%02X", why, s1, s2, s3);
}

void PowerKey::learnPressEdge(uint8_t edgeBit, const char* how) {
  if (edgeLearned_ && pressEdge_ == edgeBit) return;
  if (edgeLearned_) LOG_ERR(TAG, "press edge changes from %s to %s (%s)", edgeName(pressEdge_), edgeName(edgeBit), how);
  pressEdge_ = edgeBit;
  edgeLearned_ = true;
  s_edgeMemory = EDGE_MEMORY_MAGIC | edgeBit;
  LOG_INF(TAG, "press edge = %s (learned: %s)", edgeName(edgeBit), how);
}

void PowerKey::pauseIrq() {
  if (!available_ || irqPaused_) return;
  // detachInterrupt deja el pin sin ISR, sin interrupción habilitada y con tipo
  // DISABLE: recién ahí es seguro que el reposo lo arme por nivel.
  detachInterrupt(digitalPinToInterrupt(irqPin_));
  irqPaused_ = true;
}

void PowerKey::resumeIrq() {
  if (!available_ || !irqPaused_) return;
  // Si la línea ya está en bajo, el despertar fue por PWR y el flanco pasó
  // mientras la ISR no estaba: se anota ahora, que es lo más cerca que hay.
  if (digitalRead(irqPin_) == LOW) s_edgeAtMs = millis();
  // attachInterrupt escribe el tipo (flanco de bajada) antes de habilitar la
  // interrupción, así que nunca hay un instante con ISR y nivel a la vez.
  attachInterrupt(digitalPinToInterrupt(irqPin_), onIrqFalling, FALLING);
  irqPaused_ = false;
  if (pinIsLevelTriggered(irqPin_) || GPIO.pin[irqPin_].int_ena == 0) {
    LOG_ERR(TAG, "GPIO%d quedó mal tras el reposo (tipo=%u int_ena=%u): la ISR no protege", irqPin_,
            (unsigned)GPIO.pin[irqPin_].int_type, (unsigned)GPIO.pin[irqPin_].int_ena);
  }
}

void PowerKey::pump() {
  if (!available_) return;
  const unsigned long now = millis();

  // Lo que la ISR tuvo que arreglar sola se dice acá, que es donde hay log.
  static uint32_t levelFixesSaid = 0;
  if (s_levelFixes != levelFixesSaid) {
    levelFixesSaid = s_levelFixes;
    LOG_ERR(TAG,
            "GPIO%d estaba armado POR NIVEL con la ISR de PWR enganchada (van %lu): la ISR lo pasó a flanco. "
            "Sin esto, apretar PWR colgaba el aparato en el watchdog de interrupciones",
            irqPin_, (unsigned long)levelFixesSaid);
  }
  // Y si nadie apretó todavía, la misma comprobación desde el loop: un pin por
  // nivel con la ISR puesta es una bomba armada, no un estado que esperar.
  if (!irqPaused_ && pinIsLevelTriggered(irqPin_) && GPIO.pin[irqPin_].int_ena != 0) {
    gpio_set_intr_type(static_cast<gpio_num_t>(irqPin_), GPIO_INTR_NEGEDGE);
    LOG_ERR(TAG,
            "GPIO%d quedó armado POR NIVEL con la ISR de PWR enganchada: se restaura a flanco. "
            "Alguien armó el pin para despertar sin pauseIrq()",
            irqPin_);
  }

  // A confirmed press that outlives the PMIC's own 10 s hard cut cannot be a
  // real hold: a release edge went missing. Drop it rather than sleep on it.
  if (pressed_ && now - pressStartMs_ > STALE_PRESS_MS) {
    LOG_ERR(TAG, "press older than %lu ms without a release: dropped", STALE_PRESS_MS);
    pressed_ = false;
    confirmed_ = false;
    longSeen_ = false;
    holdConsumed_ = false;
  }

  // A lone edge with unknown polarity was taken as a press. A real press held
  // this long would have latched LONG (IrqLevel 1 s) and a tap would have
  // brought SHORT with its other edge: neither came, so it was a release edge.
  if (pressed_ && !confirmed_ && now - pressStartMs_ > UNCONFIRMED_MAX_MS) {
    learnPressEdge(pressEdge_ ^ (BIT_POSITIVE | BIT_NEGATIVE), "lone edge with no LONG or SHORT within 1.5 s");
    pressed_ = false;
    longSeen_ = false;
    holdConsumed_ = false;
  }

  // Edge-driven: the line is LOW only while the PMIC holds a status for us.
  // Polling: the level is not trusted (stuck LOW), read INTSTS2 on a timer.
  if (pinLatchTrusted_) {
    if (digitalRead(irqPin_) == HIGH) return;
  } else if (now - lastPollMs_ < POLL_FALLBACK_MS) {
    return;
  }
  if (lastFailMs_ != 0 && now - lastFailMs_ < I2C_RETRY_MS) return;
  lastPollMs_ = now;

  const unsigned long edgeAt = s_edgeAtMs;
  s_edgeAtMs = 0;

  // Up to three rounds while the line stays LOW: a new edge that latches
  // between the read and the write-1-to-clear is only visible on the next read
  // (we clear just the bits we saw, so it is never lost, only delayed).
  for (int round = 0; round < 3; ++round) {
    uint8_t s2 = 0;
    if (!readReg(REG_INTSTS2, s2)) {
      lastFailMs_ = now;
      return;
    }
    lastFailMs_ = 0;
    const uint8_t key = s2 & KEY_BITS;
    if (key) {
      writeReg(REG_INTSTS2, key);
      decode(key, now, round == 0 ? edgeAt : 0);
    }
    if (!pinLatchTrusted_) break;  // polling mode: one read per poll
    if (digitalRead(irqPin_) == HIGH) break;
    if (!key) {
      // Line LOW with nothing of ours pending: some other source is latched
      // (INTEN1/3 were zeroed, so this is unexpected). Flush everything once;
      // if it still will not lift, stop trusting the level and poll instead.
      flushAllStatus("IRQ stuck LOW");
      if (digitalRead(irqPin_) == LOW) {
        pinLatchTrusted_ = false;
        LOG_ERR(TAG, "IRQ line stuck LOW: polling INTSTS2 every %lu ms from now on", POLL_FALLBACK_MS);
      }
      break;
    }
  }
}

void PowerKey::decode(const uint8_t s2, const unsigned long now, const unsigned long edgeAt) {
  const uint8_t edges = s2 & (BIT_POSITIVE | BIT_NEGATIVE);
  const bool bothEdges = edges == (BIT_POSITIVE | BIT_NEGATIVE);
  const bool singleEdge = edges == BIT_POSITIVE || edges == BIT_NEGATIVE;
  const bool lng = s2 & BIT_LONG;
  // SHORT is latched with the release of a press shorter than IrqLevel. Once
  // LONG has been seen for this press the SHORT bit means nothing to us
  // (folloup: a hold can latch both), so only the edges end a long press.
  const bool sht = (s2 & BIT_SHORT) && !lng && !longSeen_;
  // The edge instant from the ISR, when it is recent enough to be this event.
  const unsigned long at = (edgeAt != 0 && now - edgeAt < STALE_EDGE_MS) ? edgeAt : now;
  // UNA PULSACIÓN VIEJA NO ES UNA ORDEN. Si el flanco es de hace más de 5 s, el
  // loop estuvo ocupado (un POST de 90 s, un render largo) y el PMIC guardó
  // todo —press, release, LONG, SHORT, típico `sts2=0F`— hasta que alguien lo
  // leyó. El dueño apretó PWR para "destrabar" el aparato colgado en Hablar, y
  // 90 s después, cuando por fin llegó la respuesta, el aparato se SUSPENDIÓ con
  // esa pulsación guardada: "cuando sale de eso, se traba". Se decodifica igual
  // (el estado tiene que quedar consistente) pero no se emite ni la suelta ni
  // el toque, y se dice.
  const bool stale = edgeAt != 0 && now - edgeAt >= STALE_EDGE_MS;

  // Learn the polarity where the read is unambiguous: SHORT rides with the
  // release edge, so the other edge is the press.
  if (sht && singleEdge) learnPressEdge(edges ^ (BIT_POSITIVE | BIT_NEGATIVE), "SHORT arrived with the release edge");

  bool press = false;
  bool release = false;
  if (bothEdges) {
    press = true;  // a whole tap inside one read
    release = true;
  } else if (singleEdge) {
    if (edgeLearned_) {
      if (edges == pressEdge_)
        press = true;
      else
        release = true;
    } else if (!pressed_) {
      // First edge ever: take it as a press; pump() undoes it if nothing
      // confirms it (see UNCONFIRMED_MAX_MS) and learns the opposite.
      pressEdge_ = edges;
      press = true;
    } else {
      release = true;
    }
  }
  if (lng && !pressed_) press = true;
  if (sht) {
    if (!pressed_) press = true;
    release = true;
  }

  if (press && pressed_ && !release) {
    // Two presses without a release in between: the release went missing.
    // Start over from this press.
    pressed_ = false;
  }
  if (press && !pressed_) {
    pressed_ = true;
    longSeen_ = false;
    holdConsumed_ = false;
    pressStartMs_ = at;
    confirmed_ = edgeLearned_;
  }
  if (lng && pressed_) {
    // LONG is raised exactly IrqLevel after the press: if the loop only got
    // to read the press edge late, this pulls the start back where it was.
    const unsigned long anchor = now - IRQ_LEVEL_MS;
    if (static_cast<long>(pressStartMs_ - anchor) > 0) pressStartMs_ = anchor;
    longSeen_ = true;
    if (!confirmed_ && !edgeLearned_ && singleEdge == false) {
      // A LONG after a lone tentative edge proves that edge was the press.
      learnPressEdge(pressEdge_, "LONG followed the first edge");
    }
    confirmed_ = true;
  }
  if (release && pressed_) {
    pressed_ = false;
    heldMs_ = at - pressStartMs_;  // modular: the anchor may sit before millis() wrapped
    if (stale) {
      LOG_INF(TAG, "sts2=%02X release held=%lu ms: la pulsación es de hace %lu ms, con el loop ocupado; no cuenta", s2,
              heldMs_, now - edgeAt);
    } else {
      if (!holdConsumed_ && heldMs_ < SHORT_PRESS_MAX_MS) shortPress_ = true;
      releasePending_ = true;
      LOG_INF(TAG, "sts2=%02X release held=%lu ms%s%s", s2, heldMs_, shortPress_ ? " short" : "",
              longSeen_ ? " long" : "");
    }
    holdConsumed_ = false;
    longSeen_ = false;
    confirmed_ = false;
  } else {
    LOG_INF(TAG, "sts2=%02X pressed=%d confirmed=%d held=%lu ms", s2, pressed_ ? 1 : 0, confirmed_ ? 1 : 0,
            pressed_ ? now - pressStartMs_ : 0UL);
  }
}

unsigned long PowerKey::heldMs() const {
  if (pressed_) return millis() - pressStartMs_;
  return heldMs_;
}

bool PowerKey::tookShortPress() {
  const bool took = shortPress_;
  shortPress_ = false;
  releasePending_ = false;  // a dropped tap has no release either
  return took;
}

bool PowerKey::tookRelease() {
  const bool took = releasePending_;
  releasePending_ = false;
  return took;
}
