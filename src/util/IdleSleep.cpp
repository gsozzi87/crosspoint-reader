#include "IdleSleep.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/gpio_struct.h>

#include "PowerKey.h"

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
  LOG_INF(TAG, "reposo %s: mascara 0x%08llx%s", available_ ? "listo" : "sin pines", (unsigned long long)buttonMask_,
          rtcIntUsable_ ? " + INT del RTC" : "");
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
  // GPIO38 tiene la ISR de flanco de PowerKey: se suelta ANTES de armarlo por
  // nivel, o la primera pulsación de PWR después del reposo es un bucle de
  // interrupción hasta el watchdog (ver disarmWakeSources()).
  POWER_KEY.pauseIrq();
  for (uint32_t pin = 0; pin < 64; ++pin) {
    if (!(buttonMask_ & (1ULL << pin))) continue;
    // Un pin con una interrupción habilitada NO se arma por nivel, sea de
    // quien sea: sería dejar armada la bomba de 1.5.93-1.5.98 para el próximo
    // que enganche una ISR en un botón. Se dice y no se reposa.
    if (GPIO.pin[pin].int_ena != 0) {
      LOG_ERR(TAG,
              "GPIO%u tiene una interrupción habilitada (tipo=%u): armarlo por nivel colgaría el aparato al "
              "apretarlo, no se reposa",
              (unsigned)pin, (unsigned)GPIO.pin[pin].int_type);
      return false;
    }
    if (gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL) != ESP_OK) {
      LOG_ERR(TAG, "GPIO%u no acepta despertar", (unsigned)pin);
      return false;
    }
  }
  if (rtcIntUsable_ && assigned(rtcIntPin_)) {
    // REV-078: el retorno NO se puede ignorar. Con el INT del RTC dado por
    // usable, `msUntilNextAlarm()` deja dormir SIN timer del ESP cuando la
    // alarma está a más de una hora: toda la noche colgada de que este pin
    // despierte. Si el armado falló y nadie lo miró, no hay ninguna fuente y la
    // alarma no suena — que es exactamente el modo de fallo de REV-060, una
    // capa más adentro.
    if (gpio_wakeup_enable(static_cast<gpio_num_t>(rtcIntPin_), GPIO_INTR_LOW_LEVEL) != ESP_OK) {
      // Se BAJA la bandera: si no, `msUntilNextAlarm()` seguiría devolviendo
      // "dormí sin timer" para siempre y cada intento fallaría igual. Bajándola,
      // la política cae sola al timer de una hora, que es la degradación
      // correcta: la alarma llega tarde en vez de no llegar.
      rtcIntUsable_ = false;
      LOG_ERR(TAG, "GPIO%d (INT del RTC) no acepta despertar: se deja de confiar en él y se vuelve al timer",
              (int)rtcIntPin_);
      return false;
    }
  }
  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) return false;
  if (budgetMs > 0) {
    // REV-074: idem. Este presupuesto no es decorativo — `main.cpp` lo recorta a
    // lo que falte para la próxima alarma Y para el deep sleep de los diez
    // minutos. Sin timer, el reposo no vuelve para ninguna de las dos cosas: el
    // aparato se queda en light sleep hasta que alguien apriete un botón, sin
    // bajar nunca al sueño profundo. Es el mismo defecto que ya costó una noche
    // de batería en 1.5.72, por el otro lado.
    if (esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(budgetMs) * 1000ULL) != ESP_OK) {
      LOG_ERR(TAG, "el timer del reposo no se pudo armar (%lu ms): no se reposa", budgetMs);
      return false;
    }
  }
  return true;
}

// DESARMAR ES OBLIGATORIO EN TODA SALIDA, y no saberlo costó un aparato trabado.
//
// `armWakeSources()` arma los botones con `GPIO_INTR_LOW_LEVEL`: una interrupción
// POR NIVEL, no por flanco. Mientras el pin siga en bajo esa interrupción se
// vuelve a disparar sola, una y otra vez. Normalmente no importa porque
// `esp_light_sleep_start()` la consume y al volver se desarma todo.
//
// Pero si se arma y NO se duerme, queda una interrupción por nivel sin nadie que
// la atienda: apretar un botón deja la CPU sin salir del vector de interrupción
// hasta que salta el WATCHDOG DE INTERRUPCIONES. Y como al reiniciar pasa lo
// mismo, el aparato entra en un bucle de reinicios del que sólo se sale
// sacándole la batería. Eso fue exactamente 1.5.97: la guardia del panel
// (`gfxPanelRefreshInFlight()`) volvía DESPUÉS de armar y desarmaba sólo el
// timer. Un `return` en el lugar equivocado.
//
// Y ESO ERA LA MITAD (1.5.99). `gpio_wakeup_enable(pin, LOW_LEVEL)` escribe el
// TIPO de interrupción del pin (bits 7-9 de GPIO_PINn_REG) y `gpio_wakeup_disable`
// sólo apaga el bit de despertar: el tipo queda en NIVEL para siempre (verificado
// desensamblando libesp_driver_gpio.a). A los cuatro botones no les importa —no
// tienen ISR—, pero GPIO38 tiene la ISR de flanco de PowerKey con la interrupción
// habilitada. O sea que desde el PRIMER reposo, toda pulsación de PWR (y el propio
// despertar por PWR) era una interrupción por nivel entrando sin parar hasta el
// watchdog, y como el PMIC mantiene la línea en bajo hasta que el loop la lea por
// I2C, no había salida. Desde 1.5.93 —cuando el reposo empezó a entrar de verdad—
// hasta 1.5.98. Por eso la ISR se suelta antes de armar y se vuelve a enganchar
// acá, que es lo que restaura el tipo a flanco.
void IdleSleep::disarmWakeSources() {
  for (uint32_t pin = 0; pin < 64; ++pin) {
    if (!(buttonMask_ & (1ULL << pin))) continue;
    gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
  }
  if (rtcIntUsable_ && assigned(rtcIntPin_)) gpio_wakeup_disable(static_cast<gpio_num_t>(rtcIntPin_));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  POWER_KEY.resumeIrq();
}

IdleSleep::Woke IdleSleep::tick(const unsigned long idleMs, const bool blocked) {
  // La guardia del panel va ACÁ ARRIBA, junto a las demás, y no pegada al sueño:
  // así no hay nada armado que desarmar. `main.cpp` ya la consultó, pero entre
  // aquella consulta y ésta pasan varios milisegundos —dos lecturas I2C en el
  // medio— y en ese hueco puede arrancar un refresco. Dormir con una onda en
  // curso se come el flanco de BUSY.
  if (!available_ || blocked || gfxPanelRefreshInFlight() || idleMs < REST_AFTER_MS) {
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
    // Puede haber alcanzado a armar algunos pines antes de fallar, y armado sin
    // dormir es la receta del watchdog de interrupciones: se desarma todo.
    disarmWakeSources();
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
  disarmWakeSources();
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
