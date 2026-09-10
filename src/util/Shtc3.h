#pragma once

// Sensor de temperatura y humedad SHTC3 (I²C 0x70) de la ws397: da la
// temperatura de adentro de casa para el widget del clima, que hasta ahora solo
// mostraba la de afuera. El SDK todavía no lo maneja (SensorsConfig mapea un
// SHT40), así que va acá con Wire directo sobre el bus de los sensores.
//
// La medición son dos tiempos separados por 12 ms de espera del sensor. Esos
// 12 ms NO pueden salir de un delay() adentro de un render: la tarea de dibujo
// tiene el panel tomado y el loop se queda esperando. Por eso `tick()` se llama
// desde el loop de main.cpp y las pantallas solo leen lo cacheado.
namespace shtc3 {
// Del loop: arranca una medición cuando el caché venció y la levanta 15 ms
// después. Nunca bloquea.
void tick();
// Última temperatura (°C) y humedad (%) leídas. NaN si todavía no hay ninguna.
float cachedCelsius();
float cachedHumidity();
// Cuándo se leyó (millis), 0 si nunca.
unsigned long readAt();
}  // namespace shtc3
