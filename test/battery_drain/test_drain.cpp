// Prueba de escritorio de la cuenta del diario de batería.
//
// Un número de autonomía que miente es peor que no tener número: si la ventana
// se elige mal (por ejemplo tomando un tramo donde el aparato estuvo
// enchufado), la pantalla diría "quedan 40 días" con toda seriedad. Acá se
// prueba justamente eso, que es lo único de este subsistema que se puede
// probar sin placa.
//
// Correr: ./test/battery_drain/run.sh

#include <cmath>
#include <cstdio>
#include <vector>

#include "BatteryLog.h"

using batterylog::Drain;
using batterylog::Sample;

namespace {
int failures = 0;
constexpr time_t H = 3600;

void check(const bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FALLA", what);
  if (!ok) ++failures;
}

bool near(const float a, const float b, const float tol = 0.05f) { return std::fabs(a - b) <= tol; }

// `t` es un desplazamiento en segundos desde una fecha CREÍBLE, no un epoch
// absoluto: desde 1.5.93 `analyze()` descarta las muestras con fecha anterior a
// 2024 (ver credibleEpoch), así que escribir los casos sobre el epoch 0 medía el
// rechazo en vez de la cuenta.
constexpr time_t BASE = batterylog::EPOCH_2024 + 400L * 24 * 3600;  // principios de 2025

Sample s(const time_t t, const int pct, const bool charging = false) {
  Sample x;
  x.epoch = BASE + t;
  x.pct = pct;
  x.mv = 3700;
  x.charging = charging;
  return x;
}

// Una muestra con la fecha TAL CUAL, para los casos de reloj roto.
Sample raw(const time_t epoch, const int pct, const bool charging = false) {
  Sample x;
  x.epoch = epoch;
  x.pct = pct;
  x.mv = 3700;
  x.charging = charging;
  return x;
}
}  // namespace

int main() {
  printf("-- lo que no alcanza para medir --\n");
  check(!batterylog::analyze({}).valid, "sin muestras no hay medición");
  check(!batterylog::analyze({s(0, 100)}).valid, "con una sola muestra tampoco");
  check(!batterylog::analyze({s(0, 100), s(300, 99)}).valid, "cinco minutos es muy poco (resolución de 1 %)");
  check(!batterylog::analyze({s(0, 80), s(10 * H, 80)}).valid, "diez horas sin caer un punto: no hay pendiente");

  printf("\n-- la cuenta --\n");
  {
    // 100 % -> 90 % en 10 horas = 1 %/h, y quedan 90 horas.
    const Drain d = batterylog::analyze({s(0, 100), s(5 * H, 95), s(10 * H, 90)});
    check(d.valid, "diez horas y diez puntos sí miden");
    check(near(d.pctPerHour, 1.0f), "1,0 %/h");
    check(near(d.hours, 10.0f), "la ventana son las diez horas enteras");
    check(near(d.hoursLeft, 90.0f, 0.5f), "quedan 90 horas");
    check(d.fromPct == 100 && d.toPct == 90, "y dice de dónde a dónde");
  }

  printf("\n-- lo que invalida el tramo --\n");
  {
    // Lo enchufaron en el medio: la ventana tiene que cortar DESPUÉS de eso y
    // medir sólo la descarga limpia del final, no los dos tramos juntos.
    const Drain d =
        batterylog::analyze({s(0, 90), s(1 * H, 60), s(2 * H, 100, /*charging=*/true), s(3 * H, 100), s(13 * H, 90)});
    check(d.valid, "después de una carga se sigue midiendo");
    check(near(d.hours, 10.0f), "pero sólo el tramo de después (10 h, no 13)");
    check(near(d.pctPerHour, 1.0f), "y da 1,0 %/h, no la mezcla de los dos");
  }
  {
    // Sin bandera de carga pero el porcentaje subió: también corta, porque
    // hacia atrás aparece un valor por debajo del actual.
    const Drain d = batterylog::analyze({s(0, 40), s(1 * H, 100), s(11 * H, 90)});
    check(near(d.hours, 10.0f), "un salto hacia arriba corta la ventana aunque no diga 'cargando'");
  }
  {
    // La muestra más nueva está cargando: no se mide nada.
    const Drain d = batterylog::analyze({s(0, 100), s(10 * H, 90, /*charging=*/true)});
    check(!d.valid, "si la última muestra está cargando, no se mide");
  }

  printf("\n-- un caso realista --\n");
  {
    // Una noche entera reposando: 8 horas, 3 puntos. ~0,4 %/h -> ~10 días.
    const Drain d = batterylog::analyze({s(0, 78), s(4 * H, 77), s(8 * H, 75)});
    check(d.valid, "una noche de reposo alcanza para medir");
    check(near(d.pctPerHour, 0.375f, 0.01f), "0,375 %/h");
    check(d.hoursLeft > 190 && d.hoursLeft < 210, "y proyecta unos ocho días de autonomía");
  }

  printf("-- fechas que no son fechas (lo que se veía como \"desde el origen de los tiempos\") --\n");
  {
    // El caso real: el aparato anotó una muestra con el RTC sin poner en hora
    // (1970) y después, ya en hora, siguió anotando. La ventana salía de
    // cincuenta y seis años, la pendiente daba cero y la autonomía, siglos.
    const Drain d = batterylog::analyze({raw(0, 100), s(0, 90), s(10 * H, 80)});
    check(d.valid, "con una muestra de 1970 en el medio igual se mide");
    check(near(d.hours, 10.0f), "...y la ventana son las diez horas buenas, no cincuenta y seis años");
    check(near(d.pctPerHour, 1.0f), "...con la pendiente de verdad");
  }
  {
    // El PCF85063 sin pila arranca en 2000-01-01, que pasa cualquier prueba de
    // "> 0" y es justo la que había.
    const time_t Y2K = 946684800;
    const Drain d = batterylog::analyze({raw(Y2K, 100), s(0, 90), s(10 * H, 80)});
    check(near(d.hours, 10.0f), "y lo mismo con el 2000-01-01 del RTC sin pila");
  }
  check(!batterylog::analyze({raw(0, 100), raw(10 * H, 90)}).valid, "si TODAS las fechas son de 1970 no se mide nada");
  {
    // Alguien puso el reloj en hora en el medio: mirando hacia atrás la fecha
    // sube en vez de bajar, y de ahí para atrás ya no se puede comparar.
    const Drain d = batterylog::analyze({s(50 * H, 100), s(0, 95), s(10 * H, 85)});
    check(d.valid, "con el reloj corregido en el medio igual se mide");
    check(near(d.hours, 10.0f), "...sobre el tramo posterior a la corrección");
  }
  check(!batterylog::analyze({s(0, 100), s(30L * 24 * H, 60)}).valid,
        "un mes de ventana no es una descarga: es el aparato apagado");

  // --- REV-002: el parseo de una línea del archivo -------------------------
  //
  // Hasta este arreglo la línea se parseaba con `sscanf("%ld,%d,%d,%d", ...)`
  // casteando `&Sample::epoch` (time_t, 8 bytes en este target) a `long*`
  // (4 bytes) y `&Sample::charging` (bool) a `int*`. Lo del bool no es
  // teórico: un valor que no sea 0 ni 1 deja el bool en un estado inválido, y
  // un múltiplo de 256 se leía como `false`, o sea una muestra CARGANDO
  // colándose adentro de la ventana de descarga.
  {
    Sample x;
    check(batterylog::parseLine("1735689600,80,4000,1", x), "se parsea una línea normal");
    check(x.epoch == 1735689600 && x.pct == 80 && x.mv == 4000 && x.charging, "...con sus cuatro campos");
    check(batterylog::parseLine("1735689600,80,4000,0", x) && !x.charging, "charging=0 es false");
    // Sin el cuarto campo (formato viejo) no se inventa carga.
    check(batterylog::parseLine("1735689600,80,4000", x) && !x.charging, "sin el cuarto campo, no está cargando");
  }
  {
    // El byte bajo de 256 es 0: con el cast a int* esto se leía como "no está
    // cargando" y la muestra entraba en la ventana.
    Sample x;
    check(batterylog::parseLine("1735689600,80,4000,256", x) && x.charging, "charging=256 sigue siendo cargando");
    check(batterylog::parseLine("1735689600,80,4000,2", x) && x.charging, "charging=2 sigue siendo cargando");
    check(batterylog::parseLine("1735689600,80,4000,-1", x) && x.charging, "charging=-1 sigue siendo cargando");
  }
  {
    // Post-2038: no entra en un `long` de 32 bits, que es el que había.
    Sample x;
    check(batterylog::parseLine("4102444800,80,4000,0", x), "se parsea una fecha de 2100");
    check(x.epoch == static_cast<time_t>(4102444800LL), "...sin truncarse a 32 bits");
  }
  {
    Sample x;
    check(!batterylog::parseLine("", x), "una línea vacía no es una muestra");
    check(!batterylog::parseLine("basura", x), "una línea de basura tampoco");
    check(!batterylog::parseLine("1735689600,80", x), "ni una línea a medias");
    check(!batterylog::parseLine("0,80,4000,0", x), "ni epoch 0");
    check(!batterylog::parseLine("-5,80,4000,0", x), "ni epoch negativo");
    check(!batterylog::parseLine(nullptr, x), "ni un puntero nulo");
  }
  {
    // Y una muestra CARGANDO parseada de verdad corta la ventana, que es para
    // lo que sirve todo esto.
    Sample a, b, c;
    batterylog::parseLine("1735689600,100,4200,256", a);  // cargando, con el valor "raro"
    batterylog::parseLine("1735725600,95,4100,0", b);     // +10 h
    batterylog::parseLine("1735761600,85,4000,0", c);     // +20 h
    const Drain d = batterylog::analyze({a, b, c});
    check(d.valid, "con la carga bien parseada igual se mide");
    check(near(d.hours, 10.0f), "...pero la ventana arranca DESPUÉS de la carga");
  }

  printf("\n%s (%d fallas)\n", failures == 0 ? "TODO BIEN" : "HAY FALLAS", failures);
  return failures == 0 ? 0 : 1;
}
