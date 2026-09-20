// REV-017: quién puede salir de la cola offline.
//
// El daño que esto evita no es un duplicado: la cola guarda POSTs con ids del
// STORE y el servidor numera desde 1 en CADA cuenta, así que una entrada de la
// cuenta A aplicada sobre la B le tilda, le corre la fecha o le BORRA a B el
// objeto que casualmente tenga ese número.
#include <cstdio>
#include <string>

#include "QueueAccount.h"

static int fallos = 0;

static void chk(const char* que, bool dio, bool esperado) {
  if (dio == esperado) return;
  printf("FALLO %s: dio %s, esperaba %s\n", que, dio ? "sale" : "no sale", esperado ? "sale" : "no sale");
  ++fallos;
}

static queueacct::Entry sellada(const char* cuenta) { return {true, cuenta}; }
static queueacct::Entry vieja() { return {false, ""}; }

int main() {
  const queueacct::Session tranquila{"ana@x", false};
  const queueacct::Session recienCambiada{"beto@x", true};

  // Lo normal: sellada con la cuenta de ahora.
  chk("sellada con la cuenta de ahora", queueacct::allowed(sellada("ana@x"), tranquila), true);

  // Sellada con otra: nunca. Es la red por si `clearQueue()` no pudo escribir.
  chk("sellada con otra cuenta", queueacct::allowed(sellada("beto@x"), tranquila), false);
  chk("sellada con la cuenta VIEJA tras el cambio", queueacct::allowed(sellada("ana@x"), recienCambiada), false);

  // EL CASO DEL REVISOR: entrada de un firmware anterior (sin sello) y el
  // aparato acaba de cambiar de cuenta. Antes salía UNA vez contra la nueva.
  chk("vieja + cambio de cuenta recién", queueacct::allowed(vieja(), recienCambiada), false);

  // Y la vieja sí sale mientras no haya habido cambio: si no, un aparato que
  // actualiza por OTA con cosas encoladas las perdería sin motivo.
  chk("vieja sin cambio de cuenta", queueacct::allowed(vieja(), tranquila), true);

  // Servidor de una sola cuenta: la cuenta es la cadena vacía de los dos lados
  // y todo tiene que seguir saliendo igual.
  const queueacct::Session soloUna{"", false};
  chk("un solo usuario, sellada", queueacct::allowed(sellada(""), soloUna), true);
  chk("un solo usuario, vieja", queueacct::allowed(vieja(), soloUna), true);
  // Y una sellada con una cuenta real no sale contra el servidor de una sola:
  // es el aparato que se mudó de un servidor con cuentas a uno sin.
  chk("un solo usuario, sellada con cuenta ajena", queueacct::allowed(sellada("ana@x"), soloUna), false);

  // El motivo que va al log existe justo cuando no sale, y no cuando sí.
  if (queueacct::refusal(sellada("ana@x"), tranquila) != nullptr) {
    printf("FALLO: motivo con una que sale\n");
    ++fallos;
  }
  {
    const char* a = queueacct::refusal(sellada("beto@x"), tranquila);
    const char* b = queueacct::refusal(vieja(), recienCambiada);
    if (!a || !b) {
      printf("FALLO: falta el motivo\n");
      ++fallos;
    }
    // Y son motivos DISTINTOS: mandan a mirar cosas distintas.
    else if (std::string(a) == std::string(b)) {
      printf("FALLO: los dos motivos son el mismo\n");
      ++fallos;
    }
  }

  // ── REV-055: el sello VACÍO es un sello ──────────────────────────────────
  //
  // `""` es la cuenta del servidor de un solo usuario: una identidad conocida,
  // no una ausente. `enqueue()` escribe el campo SIEMPRE, así que estas tres
  // tienen que distinguirse — y antes las dos primeras eran la MISMA cosa
  // adentro del archivo.
  {
    const queueacct::Session unoSoloTrasCambio{"", true};

    // 1. Entrada NUEVA del modo de un solo usuario (sellada con ""), en una
    //    sesión donde ADEMÁS se detectó un cambio de cuenta. Tiene que salir:
    //    es del dueño de ahora y la acaba de crear. Ésta es la que se perdía.
    chk("nueva con sello vacío, aunque hubo cambio", queueacct::allowed(sellada(""), unoSoloTrasCambio), true);

    // 2. Entrada de verdad vieja (sin campo) con un cambio reciente: sigue
    //    bloqueada. Si esto se aflojara, volveríamos a REV-017.
    chk("vieja sin sello, con cambio", queueacct::allowed(vieja(), unoSoloTrasCambio), false);

    // 3. Sellada con otra cuenta: bloqueada, como siempre.
    chk("sellada con otra, en un solo usuario", queueacct::allowed(sellada("ana@x"), unoSoloTrasCambio), false);

    // Y el motivo de las dos que no salen sigue siendo distinto.
    const char* a = queueacct::refusal(vieja(), unoSoloTrasCambio);
    const char* b = queueacct::refusal(sellada("ana@x"), unoSoloTrasCambio);
    if (!a || !b || std::string(a) == std::string(b)) {
      printf("FALLO: los motivos de REV-055 no distinguen\n");
      ++fallos;
    }
  }

  if (fallos) {
    printf("%d fallo(s)\n", fallos);
    return 1;
  }
  printf("queue_account: todo bien\n");
  return 0;
}
