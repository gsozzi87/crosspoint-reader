#pragma once

#include <GfxRenderer.h>

// Dígitos de 7 segmentos, los del contador del temporizador y los del
// reproductor. Estaban duplicados en TimerActivity; viven acá porque en este
// panel un número grande de segmentos se lee de lejos y no depende de ninguna
// fuente cargada de la tarjeta.
namespace sevenseg {

// Segmentos a b c d e f g (arriba, arriba-derecha, abajo-derecha, abajo,
// abajo-izquierda, arriba-izquierda, medio).
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// `t` es el grosor del trazo.
inline void digit(const GfxRenderer& r, const int value, const int x, const int y, const int w, const int h,
                  const int t, const bool ink = true) {
  const uint8_t s = SEGMENTS[value % 10];
  const int half = h / 2;
  if (s & 0x01) r.fillRect(x + t, y, w - 2 * t, t, ink);                // a
  if (s & 0x02) r.fillRect(x + w - t, y + t, t, half - t, ink);         // b
  if (s & 0x04) r.fillRect(x + w - t, y + half, t, half - t, ink);      // c
  if (s & 0x08) r.fillRect(x + t, y + h - t, w - 2 * t, t, ink);        // d
  if (s & 0x10) r.fillRect(x, y + half, t, half - t, ink);              // e
  if (s & 0x20) r.fillRect(x, y + t, t, half - t, ink);                 // f
  if (s & 0x40) r.fillRect(x + t, y + half - t / 2, w - 2 * t, t, ink); // g
}

// Los dos puntos del reloj, centrados en una columna de ancho `w`.
inline void colon(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int t,
                  const bool ink = true) {
  r.fillRect(x + w / 2 - t / 2, y + h / 3 - t / 2, t, t, ink);
  r.fillRect(x + w / 2 - t / 2, y + 2 * h / 3 - t / 2, t, t, ink);
}

// "M:SS" con dígitos de segmentos. Devuelve el ancho que ocupó.
inline int clock(const GfxRenderer& r, const int seconds, const int x, const int y, const int dw, const int dh,
                 const int t, const int gap, const bool ink = true) {
  const int s = seconds < 0 ? 0 : seconds;
  const int m = s / 60;
  const int sec = s % 60;
  const int colonW = dw / 2;
  int cx = x;
  if (m >= 10) {
    digit(r, (m / 10) % 10, cx, y, dw, dh, t, ink);
    cx += dw + gap;
  }
  digit(r, m % 10, cx, y, dw, dh, t, ink);
  cx += dw + gap;
  colon(r, cx, y, colonW, dh, t, ink);
  cx += colonW + gap;
  digit(r, sec / 10, cx, y, dw, dh, t, ink);
  cx += dw + gap;
  digit(r, sec % 10, cx, y, dw, dh, t, ink);
  return cx + dw - x;
}

}  // namespace sevenseg
