#pragma once

// Leer un archivo de la tarjeta ENTERO a memoria, con tope.
//
// REV-059. El patrón estaba copiado ocho veces, idéntico y sin tope:
//
//     std::string raw;
//     raw.resize(f.size());          // <- lo que diga la FAT, y punto
//     const int got = f.read(&raw[0], raw.size());
//
// El firmware compila con `-fno-exceptions`, así que un `resize()` que no se
// puede cumplir no tira `bad_alloc`: **aborta**. Y el tamaño sale de la tabla
// de archivos, o sea de un dato en la tarjeta que puede venir corrupto — no de
// algo que nosotros hayamos escrito. El caso que importa no es "el archivo
// creció": es una FAT dañada devolviendo un número absurdo.
//
// Dónde duele de verdad: `paintWallpaperForSleep()` lee los titulares de
// Noticias en el camino de SUSPENDER y de APAGAR. Un cuelgue ahí se ve como
// "PWR no apaga" o como un reinicio en la transición al sueño — y el dato que
// lo causa es REGENERABLE (son noticias). Una caché nunca puede tener permiso
// para impedir que el aparato duerma.
//
// La decisión (`judge`) va aparte del I/O a propósito, para poder probarla de
// escritorio: `./test/card_read/run.sh`.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cardread {

enum class Verdict : uint8_t {
  Empty,   ///< 0 bytes: no hay nada que leer, y no es un error
  Ok,      ///< entra en el tope
  TooBig,  ///< se trata como caché inválida: no se reserva NADA
};

/// Qué hacer con un archivo de `size` bytes contra un tope de `cap`.
/// `size` va en 64 bits A PROPÓSITO: `HalFile::size()` devuelve `size_t`, que
/// en el S3 son 32 bits, así que un archivo de 4 GiB + 100 se leería como 100
/// y pasaría cualquier tope. El tamaño hay que pedirlo con `fileSize64()`.
constexpr Verdict judge(const uint64_t size, const size_t cap) {
  if (size == 0) return Verdict::Empty;
  if (cap == 0) return Verdict::TooBig;
  if (size > static_cast<uint64_t>(cap)) return Verdict::TooBig;
  return Verdict::Ok;
}

// Los topes, nombrados por lo que guardan y no por un número suelto. Todos
// están MUY por encima de lo que el archivo mide de verdad: no son una cuota,
// son el límite entre "grande" y "esto no puede ser cierto".
//
// - manifiesto de Noticias medido con 40 notas: 8267 bytes;
// - el libro más grande de la Biblia (Salmos): 207 KB.
inline constexpr size_t CAP_JSON_CACHE = 256 * 1024;  ///< manifiestos y cachés JSON
inline constexpr size_t CAP_ARTICLE = 128 * 1024;     ///< una nota, un capítulo suelto
inline constexpr size_t CAP_BOOK = 1024 * 1024;       ///< un libro entero de la Biblia

/// Lee `path` entero. Devuelve vacío si no está, si mide 0, si pasa el tope o
/// si la lectura falla — o sea que el llamador trata todos los fallos igual, que
/// es lo correcto para una caché: si no se puede leer, no hay caché.
/// Pasarse del tope deja una línea de [ERR] en el log, porque es lo único que
/// distingue "no había nada" de "había algo imposible".
std::string readCapped(const char* tag, const std::string& path, size_t cap);

}  // namespace cardread
