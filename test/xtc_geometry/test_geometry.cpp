// REV-009: la geometría de una página XTH de 2 bits, sin placa.
//
// El cuerpo de una página de 2 bits son dos planos de ((w*h+7)/8) bytes — eso
// es lo que el parser lee del archivo y lo que el lector reserva. Pero el
// render recorre los planos por COLUMNAS con un paso alineado a byte,
// ((h+7)/8), así que el byte más alto que toca es (w-1)*paso + (h-1)/8. Con una
// altura que no es múltiplo de 8 eso cae FUERA del buffer.
//
// 800x480 —el tamaño del panel— lo tapa, porque 480 % 8 == 0. Por eso hay que
// probarlo acá y no esperar a verlo en el vidrio.
//
// Correr: ./test/xtc_geometry/run.sh

#include <cstdint>
#include <cstdio>

#include "XtcTypes.h"

namespace {
int failures = 0;

void check(const bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FALLA", what);
  if (!ok) ++failures;
}

// El byte más alto del malloc que tocaría el render si se lo dejara entrar.
size_t highestByteTouched(const uint16_t w, const uint16_t h) {
  const size_t colBytes = (static_cast<size_t>(h) + 7) / 8;
  return xtc::planeBytes(w, h) + (static_cast<size_t>(w) - 1) * colBytes + (static_cast<size_t>(h) - 1) / 8;
}

// Lo que se reserva de verdad en XtcReaderActivity::renderPage().
size_t allocated(const uint16_t w, const uint16_t h) { return xtc::planeBytes(w, h) * 2; }
}  // namespace

int main() {
  printf("-- lo que el panel usa de verdad --\n");
  check(xtc::canRender2Bit(800, 480), "800x480 se puede dibujar");
  check(highestByteTouched(800, 480) < allocated(800, 480), "...y no se sale del buffer");

  printf("\n-- alturas que no son multiplo de 8: antes se leia fuera del buffer --\n");
  struct Caso {
    uint16_t w, h;
    size_t over;  // cuántos bytes se leían de más
  };
  // Los números salen de la aritmética, no de una corrida: 800x477 son 300.
  const Caso malos[] = {{800, 479, 100}, {800, 477, 300}, {600, 300, 300}, {100, 4, 50}, {3, 1, 2}};
  for (const Caso& c : malos) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%ux%u se rechaza", c.w, c.h);
    check(!xtc::canRender2Bit(c.w, c.h), msg);
    const size_t over = highestByteTouched(c.w, c.h) - (allocated(c.w, c.h) - 1);
    snprintf(msg, sizeof(msg), "...y habria leido %u bytes de mas", (unsigned)c.over);
    check(over == c.over, msg);
  }

  printf("\n-- alturas multiplo de 8: siguen andando --\n");
  const uint16_t buenas[] = {8, 16, 24, 64, 240, 480, 600, 1024};
  for (const uint16_t h : buenas) {
    char msg[96];
    snprintf(msg, sizeof(msg), "600x%u se puede dibujar", h);
    check(xtc::canRender2Bit(600, h), msg);
    snprintf(msg, sizeof(msg), "...y no se sale del buffer (600x%u)", h);
    check(highestByteTouched(600, h) < allocated(600, h), msg);
  }

  printf("\n-- dimensiones imposibles --\n");
  check(!xtc::canRender2Bit(0, 480), "ancho 0 se rechaza");
  check(!xtc::canRender2Bit(800, 0), "alto 0 se rechaza");

  // El contrato de verdad, barrido exhaustivo: lo que la guardia ACEPTA no se
  // sale del buffer, y lo que RECHAZA se habría salido. No es una tautología —
  // `highestByteTouched` reproduce el indexado del render, que es otra cuenta.
  //
  // (Ojo: la regla NO es exactamente "alto múltiplo de 8". Con anchos de 1 o 2
  // píxeles el redondeo de los dos lados coincide y la página es segura igual.
  // Por eso la guardia compara los dos tamaños en vez de mirar el resto.)
  bool invariante = true;
  for (uint32_t w = 1; w <= 96 && invariante; w++) {
    for (uint32_t h = 1; h <= 520; h++) {
      const uint16_t W = static_cast<uint16_t>(w), H = static_cast<uint16_t>(h);
      const bool acepta = xtc::canRender2Bit(W, H);
      const bool cabe = highestByteTouched(W, H) < allocated(W, H);
      if (acepta != cabe) {
        printf("      desacuerdo en %ux%u: acepta=%d cabe=%d\n", w, h, acepta ? 1 : 0, cabe ? 1 : 0);
        invariante = false;
        break;
      }
    }
  }
  check(invariante, "la guardia acepta exactamente lo que no se sale del buffer");

  printf("\n%s (%d fallas)\n", failures == 0 ? "TODO BIEN" : "HAY FALLAS", failures);
  return failures == 0 ? 0 : 1;
}
