// REV-054: las dos preguntas del reintento, que estaban contestadas con el
// mismo predicado.
//
//   1. ¿lo reintento AHORA, con el backoff de 500/1500 ms?   -> retryable()
//   2. ¿lo conservo para MÁS TARDE (cola offline)?           -> retryableLater()
//
// Para el 429 las respuestas son OPUESTAS, y por eso no puede ser una sola
// función. Con el 429 en la primera, un tope de uso de Groq costaba tres
// llamadas condenadas y tres mordiscos al cupo (visto en el aparato, 1.5.119).
// Sacándolo de las dos, la operación encolada se perdía — que es exactamente
// lo que arregló F02.
//
// Las dos son copia literal de lib/ServerClient/ServerClient.cpp. Se duplican
// acá a propósito: son dos líneas sin dependencias, y meter el .cpp entero
// arrastraría Arduino, WiFi y la SD.
#include <cstdio>
#include <initializer_list>

static bool retryable(int status) { return status < 0 || (status >= 500 && status <= 599); }
static bool retryableLater(int status) { return retryable(status) || status == 429; }

static int fallos = 0;

static void chk(const char* que, bool dio, bool esperado) {
  if (dio == esperado) return;
  printf("FALLO %s: dio %s, esperaba %s\n", que, dio ? "sí" : "no", esperado ? "sí" : "no");
  ++fallos;
}

int main() {
  // Un transporte caído: las dos que sí. Puede andar en el acto.
  chk("transporte, ahora", retryable(-1), true);
  chk("transporte, después", retryableLater(-1), true);

  // 5xx: el servidor puede estar saturado o reiniciando (un deploy de Railway).
  for (const int s : {500, 502, 503, 504, 599}) {
    chk("5xx, ahora", retryable(s), true);
    chk("5xx, después", retryableLater(s), true);
  }

  // EL 429, que es el caso que motiva todo esto.
  chk("429 NO se reintenta en el acto", retryable(429), false);
  chk("429 SÍ se conserva para más tarde", retryableLater(429), true);

  // El resto de los 4xx no se reintenta ni se conserva: es culpa del pedido y
  // va a fallar igual dentro de una hora.
  for (const int s : {400, 401, 403, 404, 409, 413, 422}) {
    chk("4xx, ahora", retryable(s), false);
    chk("4xx, después", retryableLater(s), false);
  }

  // Un 2xx/3xx no llega acá, pero por las dudas tampoco.
  for (const int s : {200, 201, 204, 301, 304}) {
    chk("2xx/3xx, ahora", retryable(s), false);
    chk("2xx/3xx, después", retryableLater(s), false);
  }

  // Y la relación entre las dos: lo que se reintenta en el acto SIEMPRE se
  // conserva. Al revés no.
  for (int s = -5; s <= 600; ++s) {
    if (retryable(s) && !retryableLater(s)) {
      printf("FALLO: %d se reintenta pero no se conserva\n", s);
      ++fallos;
    }
  }

  if (fallos) {
    printf("%d fallo(s)\n", fallos);
    return 1;
  }
  printf("retry_policy: todo bien\n");
  return 0;
}
