#pragma once

// EL LOOP QUE NO VUELVE (REV-065).
//
// El detector de pasadas largas de 1.5.117 mide DESPUÉS de `activityManager.loop()`:
// sirve para una operación que tarda un montón y termina, y no puede servir
// para una que no termina nunca. Un mutex tomado dos veces, una espera sin
// plazo, un driver que no contesta: el loop no vuelve, el código que mide no
// corre, y el aparato queda con la UI, los botones y PWR congelados. La única
// salida que le queda al usuario es el corte duro del PMIC (PWR 10 s) — y eso,
// sólo si quedó armado (REV-066).
//
// El watchdog de tarea del framework no cubre esto: Arduino-ESP32 3.3.7 arranca
// con `loopTaskWDTEnabled = false` y en este árbol nadie lo suscribe, así que
// `esp_task_wdt_reset()` es un no-op — incluido el del bombeo de red.
//
// Acá va un supervisor propio, y no el de la IDF, por tres motivos concretos:
//   1. corre en el OTRO núcleo, así que un busy-loop de la UI no lo mata de
//      hambre, y una tarea bloqueada en un mutex lo deja correr igual;
//   2. deja el motivo en RAM del RTC (qué pantalla, qué operación de red,
//      cuánto tardó) y el arranque siguiente lo cuenta en el log — que es la
//      regla de 1.5.96: lo detecta el aparato, no el dueño;
//   3. el reposo (light sleep) congela todo el chip mientras `millis()` sigue
//      corriendo, así que hace falta poder decirle "esto es a propósito".
//
// NO escribe en la tarjeta desde el supervisor a propósito: si el loop se colgó
// TENIENDO el mutex del almacenamiento, escribir ahí colgaría también al
// supervisor y no quedaría nadie para reiniciar.

#include <cstdint>

namespace loopwdt {

// Cuánto puede estar el loop sin dar señales de vida antes de darlo por
// colgado. Es deliberadamente holgado: lo más largo que este firmware puede
// tardar SIN un solo latido es un `connect()` de TCP más el handshake de TLS,
// que no tienen bombeo (está anotado en 1.5.117) y entre los dos pueden irse a
// más de un minuto con un router que no contesta. Todo lo demás —descargas,
// OTA, el POST de Hablar— late cada 25 ms por el bombeo de red. Dos minutos de
// aparato congelado ya son inaceptables; el punto no es cortar temprano sino
// que exista un final.
constexpr uint32_t BUDGET_MS = 120000;

// Arranca el supervisor. Va al final del setup(): el arranque tiene sus propias
// esperas largas y no hay nadie a quien responderle todavía.
void begin();

// "Sigo vivo". Del final de cada pasada del loop y del bombeo de red, que es lo
// que mantiene vivo el latido durante una descarga de noventa segundos.
void beat();

// El loop se va a detener A PROPÓSITO (reposo, sueño profundo, apagado). Sin
// esto, una noche entera de light sleep volvería con el latido viejo por horas
// y el supervisor reiniciaría el aparato en cuanto el chip despertara.
void pause(const char* why);
void resume();

// ¿Al arranque anterior lo mató el supervisor? Lo consulta el log para saber si
// tiene que conservar las últimas líneas de la RAM del RTC (REV-081): un
// `esp_restart()` es un reinicio "normal" para `esp_reset_reason()`, así que
// sin esto la evidencia del cuelgue se tiraba justo cuando más servía.
bool trippedLastBoot();

// Deja la línea en el log con lo que había quedado anotado. Se llama una vez,
// con el log ya abierto.
void reportBoot();

}  // namespace loopwdt
