// REV-063: el rescate del panel gastaba su único intento aunque el corte de
// corriente NO se hubiera hecho.
//
// El caso que importa: el panel está de verdad trabado Y el I2C del PMIC falla
// un momento. `railsCycle()` devuelve false sin haber cortado nada, pero la
// marca de "ya se le dio un ciclo" ya estaba puesta, así que el arranque
// siguiente concluía "ya se intentó y sigue mudo" — con el único arreglo
// posible sin ejecutar y un aparato lentísimo para el usuario.
//
// Las dos direcciones del error cuestan cosas distintas y por eso hay tope:
// no reintentar deja el aparato roto; reintentar sin límite es un bucle de
// arranques, porque cada intento termina en ESP.restart().
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../../src/util/PanelRescue.h"

static int fallos = 0;

static void chk(const char* que, bool dio, bool esperado) {
  if (dio == esperado) return;
  printf("FALLO %s: dio %s, esperaba %s\n", que, dio ? "si" : "no", esperado ? "si" : "no");
  ++fallos;
}

static void chkNum(const char* que, uint32_t dio, uint32_t esperado) {
  if (dio == esperado) return;
  printf("FALLO %s: dio %lu, esperaba %lu\n", que, (unsigned long)dio, (unsigned long)esperado);
  ++fallos;
}

int main() {
  using namespace panelrescue;

  // Arranque en frío: RTC_NOINIT es basura. Nada tocado, se puede cortar.
  for (const uint32_t basura : {0u, 0xFFFFFFFFu, 0x12345678u, 0xDEADBEEFu, 0x50414E00u}) {
    chk("basura: no esta hecho", done(basura), false);
    chkNum("basura: cero intentos", failedTries(basura), 0);
    chk("basura: no se toco", attempted(basura), false);
    chk("basura: se puede cortar", mayCycle(basura), true);
  }

  // El camino bueno: se marca el intento, el PMIC confirma, se gasta el
  // one-shot y no se vuelve a intentar nunca en este encendido.
  {
    uint32_t w = 0xCAFEBABEu;
    w = markAttempt(w);
    chkNum("marcado: un intento", failedTries(w), 1);
    chk("marcado: cuenta como tocado", attempted(w), true);
    w = afterCycle(w, true);
    chk("confirmado: hecho", done(w), true);
    chk("confirmado: no se repite", mayCycle(w), false);
    chkNum("confirmado: ya no cuenta intentos", failedTries(w), 0);
    chk("confirmado: cuenta como tocado", attempted(w), true);
  }

  // EL CASO DE REV-063: el PMIC no dejó. NO se gasta el one-shot y el arranque
  // siguiente vuelve a probar.
  {
    uint32_t w = 0u;
    w = afterCycle(markAttempt(w), false);
    chk("fallado: NO queda como hecho", done(w), false);
    chk("fallado: se vuelve a probar", mayCycle(w), true);
    chkNum("fallado: un intento contado", failedTries(w), 1);

    w = afterCycle(markAttempt(w), false);
    chkNum("fallado dos veces", failedTries(w), 2);
    chk("fallado dos veces: todavia se prueba", mayCycle(w), true);

    w = afterCycle(markAttempt(w), false);
    chkNum("fallado tres veces", failedTries(w), MAX_TRIES);
    chk("al tope: se deja de insistir", mayCycle(w), false);
    chk("al tope: no miente diciendo que se hizo", done(w), false);
  }

  // Y si el PMIC anda recién al tercer intento, ahí sí se gasta el one-shot.
  {
    uint32_t w = afterCycle(markAttempt(0u), false);
    w = afterCycle(markAttempt(w), false);
    w = afterCycle(markAttempt(w), true);
    chk("tercero bueno: hecho", done(w), true);
    chk("tercero bueno: no se repite", mayCycle(w), false);
  }

  // Basura que cae justo en el rango del contador: se acota hacia "no insistas"
  // a propósito. Equivocarse para ese lado deja un aparato lento; para el otro
  // lado deja un bucle de reinicios, que es peor.
  {
    const uint32_t absurdo = TRY_BASE + 200u;
    chkNum("contador absurdo se acota", failedTries(absurdo), MAX_TRIES);
    chk("contador absurdo no cicla", mayCycle(absurdo), false);
  }

  // Los dos valores no se pueden confundir entre sí.
  chk("DONE no se lee como intento", failedTries(DONE) == 0, true);
  chk("un intento no se lee como DONE", done(markAttempt(0u)), false);

  if (fallos == 0) printf("panel_rescue: OK\n");
  return fallos == 0 ? 0 : 1;
}
