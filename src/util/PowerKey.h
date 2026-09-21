#pragma once

#include <Arduino.h>

// ws397: the physical PWR key is wired to the AXP2101 PWRKEY input, not to an
// ESP GPIO. The PMIC latches press/release edges, a SHORT press (< IrqLevel)
// and a LONG press (>= IrqLevel, raised while still held) in INTSTS2 (0x49,
// write-1-to-clear) and pulls its open-drain IRQ line (GPIO38, `pmicIrq`) LOW
// until every enabled status bit has been cleared. This module owns that line:
// it configures the key timings and interrupt enables once at boot, then turns
// the latched bits into a press/release state machine from the main loop.
//
// It is deliberately NOT fed into the SDK's InputManager: its 5 ms debounce
// drops a status that is set in one read and gone in the next (a whole short
// tap can land in a single INTSTS2 read), and the hold-style input paths
// overwrite the power press timestamp. Consumers ask this object directly.
//
// GPIO38 is not an RTC GPIO, so the PWR key cannot wake the chip from deep
// sleep (OK on GPIO5 does, see BoardConfig::WS397.input.wakePin). A PWR press
// while asleep only latches status in the PMIC; begin() clears it on the next
// boot so it never replays as a phantom press.
class PowerKey {
 public:
  // A release before this many ms of hold is a "short press" (tookShortPress).
  // Matches the point where main.cpp's hold banner takes over the gesture.
  static constexpr unsigned long SHORT_PRESS_MAX_MS = 600;

  // setup(): pull-up on the IRQ line, PMIC identity check, key timings
  // (0x27), power-off policy (0x10/0x22), interrupt enables (0x40-0x42),
  // status flush (0x48-0x4A) and the FALLING edge timestamp ISR. Safe to call
  // on every board: it does nothing unless the profile assigns `pmicIrq`.
  void begin();
  // Emit the register snapshot begin() captured. begin() runs before the SD
  // log exists, so main.cpp calls this again once the log sink is attached.
  void logSnapshot() const;
  // loop(): IRQ level -> INTSTS2 -> state. Cheap when the line is idle.
  void pump();

  // True from the press edge to the release edge (once the edge polarity is
  // known; until then a press is confirmed by the PMIC's LONG status at 1 s).
  bool pressed() const { return pressed_ && confirmed_; }
  // millis() since the press edge while pressed(); the last hold length after.
  unsigned long heldMs() const;
  // Cuando empezo la pulsacion en curso (millis del flanco o del ancla del LONG).
  unsigned long pressStartMs() const { return pressStartMs_; }
  // True once per release that came before SHORT_PRESS_MAX_MS of hold and
  // whose hold was not already acted upon (consumeHold).
  bool tookShortPress();
  // The hold gesture already acted (the banner is up): the release must not
  // count as a short press.
  void consumeHold() { holdConsumed_ = true; }
  // True once per release, whatever the hold length. The owner's rule for
  // this key is "press and release = suspend", so main.cpp acts on the
  // release itself, not on its length. tookShortPress() clears this too: every
  // caller of that one is dropping the tap, and a dropped tap has no release.
  bool tookRelease();
  // La misma suelta pero SIN consumirla: es para el bombeo de la red
  // (include/NetPump.h), que necesita saber que el dueño apretó PWR para
  // cortar la petición, pero NO puede quedarse con el evento — el que
  // suspende es main.cpp, en un punto seguro y con la pantalla consistente.
  bool releasePending() const { return releasePending_; }

  // REPOSO: el light sleep arma GPIO38 como fuente de despertar POR NIVEL
  // (`gpio_wakeup_enable(LOW_LEVEL)`), y eso escribe el tipo de interrupción
  // del pin. Con la ISR de flanco de este módulo enganchada, ese pin en bajo
  // dispara la ISR sin parar hasta el watchdog de interrupciones — y el PMIC
  // mantiene la línea en bajo hasta que el loop la lea por I2C, que no llega
  // nunca. Así que ANTES de armar el pin se suelta la ISR (pauseIrq) y al
  // volver del reposo se vuelve a enganchar (resumeIrq), que además restaura
  // el tipo a flanco, porque `gpio_wakeup_disable` NO lo hace. Ver 1.5.99.
  void pauseIrq();
  void resumeIrq();
  // The PMIC answered in begin() and the key is being decoded.
  bool available() const { return available_; }
  // Si el escape físico de PWR 10 s quedó confirmado en este arranque (REV-066).
  bool hardOffArmed() const { return hardOffArmed_; }

  // RIELES EN EL SUEÑO PROFUNDO (1.5.107). Según el esquemático, ALDO1-3 del
  // AXP2101 alimentan EPD_VCC_AXP (el panel, vía el P-MOSFET Q2), Audio_VCC
  // (ES8311 + micrófono) y AudioCTR_VCC (NS4150B); ALDO4, BLDO y DLDO no se
  // usan, y DC1 es VCC3V3 (ESP, tarjeta, sensores), que NO se toca nunca.
  // Cortar los tres justo antes de dormir saca del todo lo que se comía la
  // batería de noche; begin() los vuelve a encender LO PRIMERO al arrancar
  // (corre antes que el panel en los dos caminos del setup), así que un
  // arranque por el motivo que sea los encuentra prendidos. Si esto fallara,
  // el aparato despierta con el panel a oscuras: PWR 10 s (corte duro del
  // PMIC) o sacar la batería lo devuelve a los valores de fábrica.
  bool railsOffForSleep() const;

  // CICLO DE CORRIENTE AL PANEL (1.5.108). Un SSD1677 que se quedó con BUSY
  // en alto no sale de ahí ni con RST ni con un reinicio del ESP: el PMIC no
  // se resetea con el ESP y el riel sigue puesto, así que "sacar la batería"
  // con el USB enchufado tampoco lo apaga. Lo único que lo vuelve es cortarle
  // la alimentación de verdad: ALDO1-3 abajo, `offMs` de espera para que se
  // descarguen los condensadores detrás de Q2, y arriba otra vez. Se lleva
  // también el códec y el amplificador, que se reinician con el aparato.
  bool railsCycle(uint16_t offMs) const;

  // APAGAR DE VERDAD, no dormir: el AXP2101 corta sus rieles (bit0 de 0x10,
  // soft off) y el aparato queda consumiendo lo que consume el PMIC y nada
  // más. No hay alarmas, no hay reloj en pantalla, no hay wake por botón: se
  // vuelve con PWR mantenido 1 s, que es lo que la hoja de datos llama
  // PressOn. Devuelve false si el PMIC no contesta (ahí el llamador se
  // conforma con dormir, que es lo que hacía antes).
  bool powerOff() const;

  // ¿Hay cable? VBUS_GOOD del AXP2101 (bit5 de 0x00), que es una cosa distinta
  // de "está cargando": con la batería llena el PMIC deja de cargar y el cable
  // sigue puesto. `HalGPIO::isUsbConnected()` en esta placa pregunta si carga,
  // así que con la batería llena decía que no había cable, el aparato entraba
  // en reposo con el USB enchufado y el CDC se caía — del lado de la compu eso
  // se ve como que el aparato se desconecta y se reconecta cada tanto.
  // Devuelve false si el PMIC no contesta.
  bool vbusPresent() const;

 private:
  static constexpr uint8_t BIT_POSITIVE = 0x01;  // INTSTS2 bit0: PWRKEY positive edge
  static constexpr uint8_t BIT_NEGATIVE = 0x02;  // INTSTS2 bit1: PWRKEY negative edge
  static constexpr uint8_t BIT_LONG = 0x04;      // INTSTS2 bit2: held >= IrqLevel (1 s)
  static constexpr uint8_t BIT_SHORT = 0x08;     // INTSTS2 bit3: released before IrqLevel
  static constexpr uint8_t KEY_BITS = 0x0F;

  bool readReg(uint8_t reg, uint8_t& out) const;
  bool writeReg(uint8_t reg, uint8_t val) const;
  // Escribe y RELEE para confirmar los bits de `mask`, con tres intentos: el
  // bus I2C es compartido y un NACK suelto no puede pasar por configuración
  // aplicada (REV-066 / REV-070).
  bool writeVerified(uint8_t reg, uint8_t val, uint8_t mask, const char* what) const;
  void decode(uint8_t sts2, unsigned long now, unsigned long edgeAt);
  void learnPressEdge(uint8_t edgeBit, const char* how);
  void flushAllStatus(const char* why);

  bool available_ = false;
  int8_t irqPin_ = -1;
  uint8_t addr_ = 0;
  // Edge polarity: which of POSITIVE/NEGATIVE the PMIC raises on press. Not
  // documented consistently (folloup labels POSITIVE as press, the electrical
  // reading of an active-low PWRON says NEGATIVE), so it is learned at runtime:
  // SHORT is latched together with the release edge, and LONG can only follow
  // a press. Persisted in RTC memory across deep sleep.
  uint8_t pressEdge_ = 0;        // 0 = unknown, else BIT_POSITIVE or BIT_NEGATIVE
  bool edgeLearned_ = false;     // pressEdge_ is authoritative (not a first guess)
  bool pinLatchTrusted_ = true;  // IRQ line stays LOW while status is pending
  unsigned long lastPollMs_ = 0;
  unsigned long lastFailMs_ = 0;

  bool pressed_ = false;
  bool confirmed_ = false;  // polarity known, or LONG seen for this press
  bool longSeen_ = false;   // LONG latched during the current press
  bool holdConsumed_ = false;
  bool shortPress_ = false;
  bool releasePending_ = false;
  bool irqPaused_ = false;
  unsigned long pressStartMs_ = 0;
  unsigned long heldMs_ = 0;

  uint8_t snapshot_[17] = {};
  uint8_t railsRestored_ = 0;  // qué ALDO de 1-3 tuvo que volver a encender begin()
  // REV-066: si la configuración del corte duro (PWR mantenido 10 s) quedó
  // CONFIRMADA por relectura. Es la última salida cuando todo lo demás falló,
  // así que no alcanza con haber escrito los registros: hay que comprobarlo.
  bool hardOffArmed_ = false;
  bool snapshotValid_ = false;
};

extern PowerKey POWER_KEY;
