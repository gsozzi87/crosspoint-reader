#include "IdleSleep.h"

#include <BoardConfig.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_sleep.h>


IdleSleep IDLE_SLEEP;

namespace {
constexpr const char* TAG = "REST";

// El INT del PCF85063 está en GPIO45 según la hoja de la placa. No está en el
// perfil (el SDK no tiene alarma de RTC), así que vive acá hasta que haga falta
// en otro lado. OJO: GPIO45 también es strap de VDD_SPI, así que se lee como
// entrada sin pull y nunca se maneja: tocarle el pull podría dejar el flash a
// 1,8 V en el próximo arranque.
constexpr int8_t RTC_INT_PIN = 45;

bool assigned(const int8_t pin) { return pin >= 0; }

void addPin(uint64_t& mask, const int8_t pin) {
  if (assigned(pin)) mask |= 1ULL << static_cast<uint32_t>(pin);
}
}  // namespace

void IdleSleep::begin() {
  if (!BoardConfig::isWS397()) return;
  const auto& in = BoardConfig::ACTIVE.input;
  // Los cuatro botones son activos en bajo y ya vienen con pull-up del
  // InputManager: acá sólo se marca cuáles despiertan.
  addPin(buttonMask_, in.back);
  addPin(buttonMask_, in.confirm);
  addPin(buttonMask_, in.up);
  addPin(buttonMask_, in.down);
  // La IRQ del PMIC es open-drain activa en bajo: por ahí entra el botón PWR,
  // que no es un GPIO. Sin esto, PWR no despertaría del reposo y el aparato
  // parecería colgado justo con el botón que uno aprieta cuando parece colgado.
  addPin(buttonMask_, BoardConfig::ACTIVE.pmicIrq);
  probeRtcInt();
  available_ = buttonMask_ != 0;
  LOG_INF(TAG, "reposo %s: mascara 0x%08llx%s", available_ ? "listo" : "sin pines",
          (unsigned long long)buttonMask_, rtcIntUsable_ ? " + INT del RTC" : "");
}

// El INT del RTC sólo sirve si en reposo está en alto: si flota bajo, cada
// ciclo de light sleep terminaría al instante y el aparato giraría en falso
// gastando más que despierto. Se mira una vez, al arrancar, y se loguea.
void IdleSleep::probeRtcInt() {
  pinMode(RTC_INT_PIN, INPUT);  // sin pull: es strap de VDD_SPI
  delayMicroseconds(50);
  const int level = digitalRead(RTC_INT_PIN);
  rtcIntUsable_ = level == HIGH;
  rtcIntPin_ = RTC_INT_PIN;
  if (!rtcIntUsable_) {
    LOG_ERR(TAG, "INT del RTC (GPIO%d) en bajo al arrancar: no se usa para despertar", (int)RTC_INT_PIN);
  }
}

bool IdleSleep::armWakeSources(const unsigned long budgetMs) {
  for (uint32_t pin = 0; pin < 64; ++pin) {
    if (!(buttonMask_ & (1ULL << pin))) continue;
    if (gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL) != ESP_OK) {
      LOG_ERR(TAG, "GPIO%u no acepta despertar", (unsigned)pin);
      return false;
    }
  }
  if (rtcIntUsable_ && assigned(rtcIntPin_)) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(rtcIntPin_), GPIO_INTR_LOW_LEVEL);
  }
  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) return false;
  if (budgetMs > 0) esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(budgetMs) * 1000ULL);
  return true;
}

IdleSleep::Woke IdleSleep::tick(const unsigned long idleMs, const bool blocked) {
  if (!available_ || blocked || idleMs < REST_AFTER_MS) {
    if (resting_) {
      LOG_DBG(TAG, "fin del reposo tras %lu ms en %u ciclos", restedMs_, (unsigned)cycles_);
      resting_ = false;
    }
    return Woke::NotSlept;
  }

  // Cuánto puede durar este ciclo: SOLO lo que falte para la próxima alarma
  // (capNextRest lo pone desde el loop). Sin nada armado no hay timer y el
  // reposo dura hasta que alguien apriete un botón, que es exactamente lo que
  // se quiere y es el sueño más profundo que se puede tener sin perder estado.
  const unsigned long budget = capMs_;
  // Un tope diminuto significa que hay algo a punto de vencer (o ya vencido):
  // no se reposa, se deja que el loop siga y lo atienda.
  if (budget > 0 && budget < MIN_REST_MS) {
    if (resting_) resting_ = false;
    capMs_ = 0;
    return Woke::NotSlept;
  }

  if (!resting_) {
    resting_ = true;
    LOG_INF(TAG, "a reposar tras %lu ms quieto (%s)", idleMs,
            budget > 0 ? "hasta la próxima alarma" : "hasta que toquen un botón");
  }

  if (!armWakeSources(budget)) {
    available_ = false;
    resting_ = false;
    LOG_ERR(TAG, "no se pudo armar el despertador: reposo apagado");
    return Woke::NotSlept;
  }

  // El acelerómetro no se lee mientras se reposa (desde 1.5.72 el movimiento no
  // despierta), así que dejarlo muestreando a 250 Hz toda la noche es gastar
  // por nada: es el consumidor más grande de la lista. MotionInput::poll() lo
  // vuelve a encender solo en cuanto el loop corra de nuevo.
  halTiltSensor.deepSleep();

  const unsigned long before = millis();
  const esp_err_t err = esp_light_sleep_start();
  const unsigned long slept = millis() - before;
  // El timer se desarma siempre: si quedara puesto, el deep sleep que venga
  // después heredaría estos 2 s y el aparato arrancaría en bucle.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  capMs_ = 0;

  if (err != ESP_OK) {
    // Rechazado: algún pin ya estaba en el nivel de despertar (un botón
    // apretado, la IRQ del PMIC trabada en bajo, el INT del RTC con la bandera
    // AF puesta). NO es actividad: si se devolviera Button, el llamador
    // reiniciaría el contador de ocio en cada pasada y con un pin trabado el
    // aparato se quedaría despierto a 40 mA para siempre, sin reposar y sin
    // llegar nunca al deep sleep. Se avisa una vez cada tanto y se sigue.
    resting_ = false;
    if (++rejects_ == 1 || rejects_ % 64 == 0) {
      LOG_ERR(TAG, "el kernel rechazó el reposo (%d seguidos): algún pin ya está en bajo", (int)rejects_);
    }
    return Woke::Rejected;
  }
  rejects_ = 0;

  ++cycles_;
  restedMs_ += slept;
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    resting_ = false;
    LOG_INF(TAG, "despertó por pin tras %lu ms", slept);
    return Woke::Button;
  }
  return Woke::Timer;
}
