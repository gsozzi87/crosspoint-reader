// REV-048: `servererr::tidy`, que es lo que el aparato muestra y loguea cuando
// el servidor contesta 502.
//
// Lo que se prueba es lo que puede salir mal de verdad: recortar en el medio de
// un caracter UTF-8 (los mensajes del proveedor vienen con acentos y comillas
// tipograficas) y los saltos de linea, que parten el renglon del log.
#include "ServerErrorText.h"

#include <cstdio>
#include <string>

static int fallos = 0;

static void chk(const char* que, const std::string& dio, const std::string& esperado) {
  if (dio == esperado) return;
  printf("FALLO %s\n  dio:      \"%s\"\n  esperado: \"%s\"\n", que, dio.c_str(), esperado.c_str());
  ++fallos;
}

// Todo byte de una cadena UTF-8 valida arranca un caracter (0xxxxxxx, 110xxxxx,
// 1110xxxx, 11110xxx) o lo continua (10xxxxxx), y una cadena bien formada nunca
// TERMINA en un byte de continuacion cuyo caracter quedo a medias.
static bool utf8Valido(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t largo = 0;
    if (c < 0x80) largo = 1;
    else if ((c & 0xE0) == 0xC0) largo = 2;
    else if ((c & 0xF0) == 0xE0) largo = 3;
    else if ((c & 0xF8) == 0xF0) largo = 4;
    else return false;  // un byte de continuacion suelto
    if (i + largo > s.size()) return false;
    for (size_t k = 1; k < largo; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += largo;
  }
  return true;
}

int main() {
  // Lo corto pasa tal cual.
  chk("corto", servererr::tidy("api.groq.com 400: model decommissioned", 120),
      "api.groq.com 400: model decommissioned");

  // Vacio.
  chk("vacio", servererr::tidy("", 120), "");

  // Justo en el limite no se recorta.
  chk("justo", servererr::tidy("abcde", 5), "abcde");
  chk("uno mas", servererr::tidy("abcdef", 5), "abcde...");

  // Saltos de linea y tabulaciones se vuelven espacios: un mensaje del
  // proveedor viene con el JSON de su error adentro y lo parte en dos.
  chk("saltos", servererr::tidy("linea uno\nlinea dos\r\n\tsangrada", 120),
      "linea uno linea dos   sangrada");

  // El caso que motiva el header: el corte cae EN EL MEDIO de una "é" (dos
  // bytes). Se retrocede al principio del caracter en vez de dejar medio.
  {
    const std::string in = "el modelo no está disponible en esta región";
    for (size_t n = 1; n <= in.size() + 2; ++n) {
      const std::string out = servererr::tidy(in, n);
      if (!utf8Valido(out)) {
        printf("FALLO utf8 con maxBytes=%zu: \"%s\"\n", n, out.c_str());
        ++fallos;
      }
      // Nunca crece mas alla del tope mas los puntos suspensivos.
      if (out.size() > n + 3) {
        printf("FALLO largo con maxBytes=%zu: %zu bytes\n", n, out.size());
        ++fallos;
      }
    }
  }

  // Un caracter de cuatro bytes (emoji) cortado por la mitad tampoco puede
  // dejar medio caracter: el retroceso es de hasta tres bytes.
  {
    const std::string in = std::string("ok ") + "\xF0\x9F\x92\xA1" + " idea";
    for (size_t n = 1; n <= in.size() + 2; ++n) {
      const std::string out = servererr::tidy(in, n);
      if (!utf8Valido(out)) {
        printf("FALLO utf8 (4 bytes) con maxBytes=%zu\n", n);
        ++fallos;
      }
    }
  }

  // Una cadena que es UN SOLO caracter multibyte y no entra: queda vacia con
  // los puntos, no con medio caracter.
  chk("no entra", servererr::tidy("\xC3\xA9", 1), "...");

  if (fallos) {
    printf("%d fallo(s)\n", fallos);
    return 1;
  }
  printf("server_error_text: todo bien\n");
  return 0;
}
