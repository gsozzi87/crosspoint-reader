#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

// Partir un texto largo en trozos para leerlos en voz alta.
//
// El servidor sintetiza como mucho 4000 caracteres por pedido (server/src/tts.ts),
// y un clip de ese tamaño son minutos de audio y megas de PSRAM, así que un
// artículo se lee de a pedazos: uno suena mientras se pide el siguiente.
//
// Los cortes van donde termina una frase (punto, signo de cierre, fin de
// párrafo), nunca a la mitad de una palabra, y jamás dentro de un carácter
// UTF-8: un byte suelto de una "ñ" en el medio del texto rompe la
// transcripción y la URL del pedido.
namespace textchunks {

struct Span {
  size_t start = 0;
  size_t len = 0;
};

inline bool isSpaceByte(const char c) { return static_cast<unsigned char>(c) <= ' '; }

// `target` no se usa como largo exacto: el corte es el último final de frase
// que entra antes de `maxLen`, y `minLen` evita trozos ridículamente cortos
// (un título con punto cortaría en la primera línea).
inline std::vector<Span> split(const std::string& text, const size_t maxLen = 900, const size_t minLen = 300) {
  std::vector<Span> out;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    while (i < n && isSpaceByte(text[i])) ++i;  // el trozo nunca arranca con espacios
    if (i >= n) break;
    size_t cut = 0;
    if (n - i <= maxLen) {
      cut = n;
    } else {
      const size_t limit = i + maxLen;
      const size_t from = i + minLen < limit ? i + minLen : i + 1;
      // 1) el ÚLTIMO final de frase que entra: se recorre hacia adelante y se
      //    va quedando el más lejano.
      for (size_t j = from; j < limit; ++j) {
        const char c = text[j];
        if (c == '\n') {
          cut = j + 1;
        } else if ((c == '.' || c == '!' || c == '?' || c == ';') && (j + 1 >= n || isSpaceByte(text[j + 1]))) {
          cut = j + 1;
        }
      }
      // 2) sin final de frase, el último espacio.
      if (cut == 0) {
        for (size_t j = limit; j > from; --j) {
          if (isSpaceByte(text[j])) {
            cut = j;
            break;
          }
        }
      }
      // 3) y si tampoco (una palabra kilométrica), corte duro sin partir un
      //    carácter UTF-8 por la mitad.
      if (cut == 0) {
        cut = limit;
        while (cut > i && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        if (cut <= i) cut = limit;
      }
    }
    size_t len = cut - i;
    while (len > 0 && isSpaceByte(text[i + len - 1])) --len;  // sin la cola de espacios
    if (len > 0) out.push_back({i, len});
    i = cut;
  }
  return out;
}

}  // namespace textchunks
