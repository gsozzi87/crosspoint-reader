#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
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
// Al despertar de un sueño profundo: compara la última línea del diario (la de
// "antes de dormir") con la lectura de ahora y lo dice en el log, con %/h. Es
// la medida que el dueño tenía que sacar a mano de dos arranques ("anoche se
// tragó el 9 %"): ahora la dice el aparato, y avisa cuando es demasiado.
void reportAfterSleep();

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
  float pctPerHour = 0;  // cuánto cae por hora, en puntos de porcentaje
  float hours = 0;       // qué tan larga es la ventana que se usó
  int fromPct = 0, toPct = 0;
  float hoursLeft = 0;  // cuánto falta hasta 0 % al ritmo actual
};

// Un tramo más corto que esto no dice nada: el medidor tiene 1 % de resolución
// (15 mAh en una batería de 1500) y en diez minutos eso es puro redondeo.
constexpr double MIN_WINDOW_S = 600;

// Desde cuándo una fecha es creíble. El RTC devuelve cosas con `> 0` que no son
// una fecha: sin pila, el PCF85063 arranca en 2000-01-01, y el reloj del sistema
// en 1970. Una sola muestra con una de esas fechas mezclada con las buenas hace
// una ventana de veinte o cincuenta años — que es lo que se veía en
// Ajustes → Memoria como una medición "desde el origen de los tiempos", con la
// pendiente en cero y una autonomía de siglos. Es el mismo umbral que usa
// `ServerClient::ensureClockForTls()` para decidir si le cree al reloj.
constexpr time_t EPOCH_2024 = 1704067200;  // 2024-01-01T00:00:00Z
inline bool credibleEpoch(const time_t t) { return t >= EPOCH_2024; }

// Un tramo más largo que esto no es una descarga: es el aparato apagado (o la
// fecha rota). Nadie mide la autonomía sobre dos semanas, y si aparece una
// ventana así es que algo anda mal con las fechas, no que la batería dure eso.
constexpr double MAX_WINDOW_S = 14 * 24 * 3600;

// Una línea del diario -> Sample, sin nada del aparato adentro (se prueba de
// escritorio en test/battery_drain/). Formato: `epoch,pct,mv,charging`.
//
// Los temporarios tienen el tipo que `sscanf` espera y recién después se copian
// al Sample. Escribir `%d` a través de un `int*` que apunta a un `bool`, o `%ld`
// a través de un `long*` que apunta a un `time_t`, es violar el aliasing y en
// este target además no coinciden los tamaños: `long` son 4 bytes y `time_t`
// son 8, así que el `%ld` llenaba MEDIO campo (andaba de casualidad, porque
// `epoch` arranca en 0 y el procesador es little-endian). Y el `%d` sobre el
// bool dejaba adentro un byte que no es 0 ni 1: un `charging` que vale 2 es un
// bool inválido, y uno que viene 256 se lee como `false` — o sea una muestra
// CARGANDO que se cuela adentro de la ventana de descarga, que es justo lo
// único que `analyze()` no puede permitir.
inline bool parseLine(const char* line, Sample& out) {
  if (line == nullptr) return false;
  long long epoch = 0;
  int pct = 0, mv = 0, charging = 0;
  const int got = sscanf(line, "%lld,%d,%d,%d", &epoch, &pct, &mv, &charging);
  if (got < 3) return false;
  if (epoch <= 0) return false;
  out.epoch = static_cast<time_t>(epoch);
  out.pct = pct;
  out.mv = mv;
  // Cualquier cosa distinta de 0 es "cargando": así un 2 o un 256 de un archivo
  // dañado no se convierten en un `false` silencioso.
  out.charging = (got >= 4) && (charging != 0);
  return true;
}

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
  // La muestra más nueva tiene que tener fecha creíble: sin eso no hay nada que
  // medir, y medir contra una fecha rota es peor que no medir.
  if (!credibleEpoch(last.epoch)) return d;
  size_t first = rows.size() - 1;
  for (size_t i = rows.size(); i-- > 0;) {
    if (rows[i].charging) break;
    if (rows[i].pct < last.pct) break;
    // La fecha tiene que ser creíble Y tiene que ir hacia atrás. Una muestra
    // anotada cuando el RTC no estaba en hora (1970, o el 2000-01-01 del
    // PCF85063 sin pila) estira la ventana décadas hacia atrás, y con ella la
    // pendiente se va a cero y la autonomía a siglos. Y una fecha que va
    // hacia ADELANTE mirando hacia atrás quiere decir que en el medio pusieron
    // el reloj en hora: de ahí para atrás ya no se puede comparar.
    if (!credibleEpoch(rows[i].epoch)) break;
    if (rows[i].epoch > last.epoch) break;
    first = i;
  }
  if (first >= rows.size() - 1) return d;
  const Sample& from = rows[first];
  const double seconds = static_cast<double>(last.epoch - from.epoch);
  if (seconds < MIN_WINDOW_S || seconds > MAX_WINDOW_S) return d;
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
