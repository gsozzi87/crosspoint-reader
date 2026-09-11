#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

// ws397: el diario de la batería.
//
// La idea original era leerle los miliamperios al PMIC y mostrarlos. NO SE
// PUEDE: el AXP2101 tiene ADC de VBAT, VBUS, VSYS, TS y temperatura de pastilla,
// pero **no tiene registro de corriente de batería** (el AXP192 lo tenía, este
// no), y el medidor del SDK sólo expone porcentaje, milivoltios y si carga. Un
// miliamperímetro instantáneo en esta placa no existe.
//
// Así que se mide como se mide de verdad la autonomía: **anotando el porcentaje
// contra el reloj y mirando la pendiente**. Cada MUESTRA_MIN minutos, y además
// justo antes de dormir (que es cuando empieza el tramo largo e interesante),
// se agrega una línea a `/.crosspoint/battery.csv`. Con eso, la pantalla de
// Memoria puede decir cuánto cae por hora de verdad y cuánto falta para que se
// apague, en vez de repetir una cuenta hecha a mano.
//
// La resolución del medidor es 1 %, o sea 15 mAh en una batería de 1500: para
// un rato corto no alcanza y por eso la ventana se toma lo más larga posible.
// Con el aparato reposando toda la noche, sobra.
namespace batterylog {

// Cada cuánto se anota estando despierto.
constexpr unsigned long SAMPLE_MIN = 10;
// Cuántas líneas se guardan antes de tirar la mitad más vieja. 300 líneas a
// 10 minutos son más de dos días, y el archivo no llega a 12 KB.
constexpr size_t MAX_ROWS = 300;

// Del loop de main.cpp: anota si ya pasó el rato y hay hora del RTC.
void tick();
// Antes de dormir, con la tarjeta todavía montada. Es la muestra que cierra el
// tramo despierto y abre el tramo dormido.
void sampleNow(const char* why);

// Una línea del diario.
struct Sample {
  time_t epoch = 0;
  int pct = 0;
  int mv = 0;
  bool charging = false;
};

// Lo que se pudo medir del archivo.
struct Drain {
  bool valid = false;
  float pctPerHour = 0;   // cuánto cae por hora, en puntos de porcentaje
  float hours = 0;        // qué tan larga es la ventana que se usó
  int fromPct = 0, toPct = 0;
  float hoursLeft = 0;    // cuánto falta hasta 0 % al ritmo actual
};

// Un tramo más corto que esto no dice nada: el medidor tiene 1 % de resolución
// (15 mAh en una batería de 1500) y en diez minutos eso es puro redondeo.
constexpr double MIN_WINDOW_S = 600;

// La cuenta, sin nada del aparato adentro: se prueba de escritorio en
// test/battery_drain/. Toma la ventana MÁS LARGA que termine en la muestra más
// nueva y que sea una descarga limpia — sin carga en el medio y sin que el
// porcentaje haya estado por debajo del actual (si hacia atrás baja, en algún
// momento subió, o sea que lo enchufaron y la pendiente dejó de querer decir
// algo).
inline Drain analyze(const std::vector<Sample>& rows) {
  Drain d;
  if (rows.size() < 2) return d;
  const Sample& last = rows.back();
  size_t first = rows.size() - 1;
  for (size_t i = rows.size(); i-- > 0;) {
    if (rows[i].charging) break;
    if (rows[i].pct < last.pct) break;
    first = i;
  }
  if (first >= rows.size() - 1) return d;
  const Sample& from = rows[first];
  const double seconds = static_cast<double>(last.epoch - from.epoch);
  if (seconds < MIN_WINDOW_S) return d;
  const int drop = from.pct - last.pct;
  if (drop <= 0) return d;
  d.valid = true;
  d.hours = static_cast<float>(seconds / 3600.0);
  d.pctPerHour = static_cast<float>(drop) / d.hours;
  d.fromPct = from.pct;
  d.toPct = last.pct;
  d.hoursLeft = d.pctPerHour > 0 ? static_cast<float>(last.pct) / d.pctPerHour : 0;
  return d;
}

// Lee el archivo y le pasa las líneas a `analyze`.
Drain measure();

// Cuántas muestras hay guardadas.
size_t rows();
// Borra el diario (Ajustes → Memoria, Atrás mantenido).
void reset();

}  // namespace batterylog
