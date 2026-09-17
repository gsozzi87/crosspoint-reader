#pragma once

// "Terminé y no hay nadie mirando: duerman."
//
// Una pantalla que se abrió SOLA (el arranque por temporizador de un
// recordatorio) y que se resolvió SOLA (nadie tocó un botón) no tiene por qué
// devolver el aparato al hub: el hub levanta WiFi, sincroniza y deja el
// aparato despierto los diez minutos del auto-sleep. Con un recordatorio que
// vuelve cada diez minutos eso es el 100 % del tiempo encendido, que es
// exactamente cómo se vació la batería en una noche (1.5.91).
//
// El pedido lo levanta el loop de main.cpp, que es el único que puede llamar a
// enterDeepSleep() con el aparato en un estado consistente.
namespace sleepreq {
inline bool wanted = false;

inline void request() { wanted = true; }
inline bool take() {
  const bool w = wanted;
  wanted = false;
  return w;
}
}  // namespace sleepreq
