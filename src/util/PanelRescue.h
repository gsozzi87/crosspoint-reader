#pragma once

// EL RESCATE DEL PANEL, CONTADO (REV-063).
//
// Cuando el SSD1677 se traba, BUSY se queda en alto y cada pintada paga el tope
// entero: el aparato parece colgado. Lo único que lo destraba es un corte de
// corriente DE VERDAD sobre ALDO1-3 (el PMIC no se resetea con el ESP, así que
// sacarle la batería con el cable puesto tampoco alcanza). Eso se hace UNA vez
// por encendido y se reinicia; si al volver sigue mudo, se arranca igual.
//
// El defecto que arregla este archivo: la marca de "ya se le dio un ciclo de
// corriente" se ponía ANTES de llamar al PMIC y no se volvía a mirar. O sea que
// un fallo de I2C durante el rescate —que devuelve false sin haber cortado
// nada— gastaba el único intento igual, y el arranque siguiente concluía "ya se
// intentó y sigue mudo" cuando el corte NUNCA se hizo. Quedaba un aparato
// lentísimo con el único arreglo posible sin ejecutar.
//
// Ahora la palabra de RTC_NOINIT distingue las dos cosas:
//   - HECHO  (DONE): el corte se ejecutó y el PMIC lo confirmó. El one-shot se
//     gastó y no se repite: si el panel sigue mudo, es el panel.
//   - INTENTADO n veces (TRY_BASE + n): el PMIC no dejó. Se vuelve a probar en
//     el arranque siguiente, hasta MAX_TRIES — acotado a propósito, porque cada
//     intento termina en un reinicio y sin tope sería un bucle de arranques.
//
// Es un header puro y sin nada del aparato adentro justamente para poder
// probarlo sin placa: ./test/panel_rescue/run.sh

#include <cstdint>

namespace panelrescue {

// El corte se hizo y el PMIC lo confirmó ("PANL").
inline constexpr uint32_t DONE = 0x50414E4Cu;
// Se intentó y el PMIC no dejó: el byte bajo cuenta cuántas veces ("RSC" + n).
inline constexpr uint32_t TRY_BASE = 0x52534300u;
inline constexpr uint32_t MAX_TRIES = 3;

inline bool done(uint32_t word) { return word == DONE; }

// Cuántos intentos FALLIDOS lleva la palabra. RTC_NOINIT no se pone en cero en
// un arranque en frío, así que todo lo que no calce exacto vale 0, y un byte
// bajo absurdo se acota hacia arriba: equivocarse para el lado de "ya no
// intentes" no puede hacer un bucle de reinicios, y para el otro lado sí.
inline uint32_t failedTries(uint32_t word) {
  if ((word & 0xFFFFFF00u) != TRY_BASE) return 0;
  const uint32_t n = word & 0xFFu;
  return n > MAX_TRIES ? MAX_TRIES : n;
}

// ¿Se tocó el rescate alguna vez en este encendido? (Sirve para decir "el panel
// volvió a contestar después del ciclo" y limpiar la marca.)
inline bool attempted(uint32_t word) { return done(word) || failedTries(word) > 0; }

// ¿Se puede cortar la corriente ahora?
inline bool mayCycle(uint32_t word) { return !done(word) && failedTries(word) < MAX_TRIES; }

// Lo que se ESCRIBE antes de tocar el PMIC. Va antes a propósito: si el corte
// se llevara al ESP por delante, o el I2C colgara hasta el watchdog, el
// arranque siguiente tiene que encontrar el intento contado igual.
inline uint32_t markAttempt(uint32_t word) { return TRY_BASE + failedTries(word) + 1; }

// Lo que queda DESPUÉS del corte: confirmado gasta el one-shot; fallado
// conserva el conteo para volver a probar en el arranque siguiente.
inline uint32_t afterCycle(uint32_t markedWord, bool cycled) { return cycled ? DONE : markedWord; }

}  // namespace panelrescue
