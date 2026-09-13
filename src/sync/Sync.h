#pragma once

// Sincronización oportunista: si la red ya está arriba por CUALQUIER motivo,
// se aprovecha para bajar lo que cambió.
//
// Hasta 1.5.75 la única sincronización de verdad era `HubSyncActivity`, y sólo
// se abría desde el hub (caché vencida al entrar, o Atrás mantenido). Trece o
// catorce Activities levantan WiFi —Hablar, Noticias, la Biblia, el Clima, los
// Viajes, Mi día, el Traductor, Preguntarle al libro, Vincular…— y ninguna
// bajaba nada de paso. La subida sí estaba resuelta: `ServerClient::request()`
// vacía la cola en la primera petición de cada sesión de red.
//
// Cuándo corre: desde el loop, con la red conectada, la pantalla de turno
// desocupada y no más de una vez cada `MIN_GAP_MIN` minutos. "Desocupada"
// importa: casi toda Activity de red pide `preventAutoSleep()` MIENTRAS
// trabaja, así que esta guardia espera sola a que termine y usa la ventana en
// la que la red sigue arriba pero ya no hay nada en curso.
namespace devicesync {

// Cada cuánto, como mucho. La sincronización del hub tiene su propio ciclo de
// 6 h; esto es para que abrir Hablar a media mañana ya traiga lo del día.
constexpr int MIN_GAP_MIN = 30;
// Y nunca antes de que la pantalla lleve esto quieta: la sincronización es
// SÍNCRONA y bloquea el loop unos segundos, así que hacerla encima de una
// pulsación se siente como que el aparato se colgó.
constexpr unsigned long QUIET_MS = 3000;

// Del loop de main.cpp. No hace nada si no corresponde. Devuelve true si
// sincronizó (para poder decirlo en el log una sola vez).
bool ifDue(unsigned long idleMs);

// Que la próxima pasada NO sincronice (la acaba de hacer otro, como
// `HubSyncActivity`): evita bajar dos veces lo mismo.
void markFresh();

}  // namespace devicesync
