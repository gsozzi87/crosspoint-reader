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

Sample s(const time_t t, const int pct, const bool charging = false) {
  Sample x;
  x.epoch = t;
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
    const Drain d = batterylog::analyze({s(0, 90), s(1 * H, 60), s(2 * H, 100, /*charging=*/true),
                                         s(3 * H, 100), s(13 * H, 90)});
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

  printf("\n%s (%d fallas)\n", failures == 0 ? "TODO BIEN" : "HAY FALLAS", failures);
  return failures == 0 ? 0 : 1;
}
