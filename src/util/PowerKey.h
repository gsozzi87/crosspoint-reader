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
  // True once per release that came before SHORT_PRESS_MAX_MS of hold and
  // whose hold was not already acted upon (consumeHold).
  bool tookShortPress();
  // The hold gesture already acted (the banner is up): the release must not
  // count as a short press.
  void consumeHold() { holdConsumed_ = true; }
  // The PMIC answered in begin() and the key is being decoded.
  bool available() const { return available_; }

  // APAGAR DE VERDAD, no dormir: el AXP2101 corta sus rieles (bit0 de 0x10,
  // soft off) y el aparato queda consumiendo lo que consume el PMIC y nada
  // más. No hay alarmas, no hay reloj en pantalla, no hay wake por botón: se
  // vuelve con PWR mantenido 1 s, que es lo que la hoja de datos llama
  // PressOn. Devuelve false si el PMIC no contesta (ahí el llamador se
  // conforma con dormir, que es lo que hacía antes).
  bool powerOff() const;

 private:
  static constexpr uint8_t BIT_POSITIVE = 0x01;  // INTSTS2 bit0: PWRKEY positive edge
  static constexpr uint8_t BIT_NEGATIVE = 0x02;  // INTSTS2 bit1: PWRKEY negative edge
  static constexpr uint8_t BIT_LONG = 0x04;      // INTSTS2 bit2: held >= IrqLevel (1 s)
  static constexpr uint8_t BIT_SHORT = 0x08;     // INTSTS2 bit3: released before IrqLevel
  static constexpr uint8_t KEY_BITS = 0x0F;

  bool readReg(uint8_t reg, uint8_t& out) const;
  bool writeReg(uint8_t reg, uint8_t val) const;
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
  uint8_t pressEdge_ = 0;    // 0 = unknown, else BIT_POSITIVE or BIT_NEGATIVE
  bool edgeLearned_ = false;  // pressEdge_ is authoritative (not a first guess)
  bool pinLatchTrusted_ = true;  // IRQ line stays LOW while status is pending
  unsigned long lastPollMs_ = 0;
  unsigned long lastFailMs_ = 0;

  bool pressed_ = false;
  bool confirmed_ = false;   // polarity known, or LONG seen for this press
  bool longSeen_ = false;    // LONG latched during the current press
  bool holdConsumed_ = false;
  bool shortPress_ = false;
  unsigned long pressStartMs_ = 0;
  unsigned long heldMs_ = 0;

  uint8_t snapshot_[11] = {};
  bool snapshotValid_ = false;
};

extern PowerKey POWER_KEY;
