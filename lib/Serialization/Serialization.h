#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

// --- REV-095: leer una caché de la tarjeta SIN confiarle un tamaño ---
//
// Lo que había hacía `s.resize(len)` con `len` recién sacado del archivo, sin
// tope y sin mirar si la lectura siquiera funcionó. Tres cosas a la vez:
//
//  1. el firmware compila con `-fno-exceptions`, así que un `resize()` que no
//     se puede cumplir NO lanza: **aborta**;
//  2. `len` es un `uint32_t` de la tarjeta, o sea que una escritura cortada o
//     una FAT dañada pueden pedir 4 GB;
//  3. `readPod()` devuelve void: con el archivo truncado, `len` queda con
//     basura del stack y nadie se entera.
//
// Y estas cachés son REGENERABLES (`BookMetadataCache`, `Page`, `TextBlock`,
// `ImageBlock`, el mapa de anclas de `Section`): que una de ellas pueda tumbar
// el aparato al abrir un libro —y volver a tumbarlo en cada intento hasta que
// alguien borre el archivo a mano— es lo contrario de lo que una caché debe
// costar. Ante cualquier duda se devuelve false y el llamador la reconstruye.
//
// La decisión va aparte del I/O (`fits()`) para poder probarla de escritorio:
// `./test/serialization/run.sh`.

/// Tope semántico de una cadena de estas cachés: títulos, autores, rutas,
/// anclas y ruby. El más largo de verdad son unos cientos de bytes; 64 KB es
/// el límite entre "largo" y "esto no puede ser cierto".
inline constexpr uint32_t MAX_STRING = 64u * 1024u;

/// ¿Entra `len` en el tope Y en lo que queda del archivo? Lo segundo es lo que
/// ataja el archivo truncado: pedir más bytes de los que hay es corrupción, no
/// un archivo grande.
constexpr bool fits(const uint32_t len, const uint32_t cap, const uint64_t bytesLeft) {
  return len <= cap && static_cast<uint64_t>(len) <= bytesLeft;
}

/// `readPod` que comprueba que se leyeron TODOS los bytes.
template <typename T>
bool tryReadPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

/// Cadena con tope. Devuelve false —y deja `s` vacía— si el largo no es
/// creíble, si no entra en lo que queda del archivo o si la lectura se corta.
inline bool tryReadString(HalFile& file, std::string& s, const uint32_t cap = MAX_STRING) {
  s.clear();
  uint32_t len = 0;
  if (!tryReadPod(file, len)) return false;
  const uint64_t size = file.fileSize64();
  const uint64_t pos = file.position();
  if (!fits(len, cap, size > pos ? size - pos : 0)) return false;
  if (len == 0) return true;
  s.resize(len);
  return file.read(&s[0], len) == static_cast<int>(len);
}

/// Igual que `tryReadCount` pero el contador en el archivo es de 16 bits.
/// Existe porque el formato ya estaba escrito así y cambiarlo invalidaría
/// todas las cachés de todos los libros de todos los aparatos.
inline bool tryReadCount16(HalFile& file, uint32_t& count, const uint32_t cap, const uint32_t perItem = 1) {
  count = 0;
  uint16_t raw = 0;
  if (!tryReadPod(file, raw)) return false;
  if (raw > cap) return false;
  const uint64_t size = file.fileSize64();
  const uint64_t pos = file.position();
  const uint64_t left = size > pos ? size - pos : 0;
  if (static_cast<uint64_t>(raw) * perItem > left) return false;
  count = raw;
  return true;
}

/// Contador con tope, para todo `reserve()`/bucle que salga de la tarjeta.
/// `cap` es el máximo semántico; `perItem` los bytes MÍNIMOS que ocupa cada
/// elemento, para descartar un contador que no puede caber en lo que queda.
inline bool tryReadCount(HalFile& file, uint32_t& count, const uint32_t cap, const uint32_t perItem = 1) {
  count = 0;
  if (!tryReadPod(file, count)) return false;
  if (count > cap) {
    count = 0;
    return false;
  }
  const uint64_t size = file.fileSize64();
  const uint64_t pos = file.position();
  const uint64_t left = size > pos ? size - pos : 0;
  if (static_cast<uint64_t>(count) * perItem > left) {
    count = 0;
    return false;
  }
  return true;
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

inline void readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
