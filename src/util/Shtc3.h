#pragma once

// Sensor de temperatura y humedad SHTC3 (I²C 0x70) de la ws397: da la
// temperatura de adentro de casa para el widget del clima, que hasta ahora solo
// mostraba la de afuera. El SDK todavía no lo maneja (SensorsConfig mapea un
// SHT40), así que va acá con Wire directo sobre el bus de los sensores.
namespace shtc3 {
// Lee temperatura (°C) y humedad (%). Devuelve false si el sensor no contesta.
// Tarda ~15 ms: no llamarla en cada pasada del loop.
bool read(float& celsius, float& humidity);
// Temperatura cacheada, refrescada como mucho una vez por minuto. NaN si no hay.
float cachedCelsius();
}  // namespace shtc3
