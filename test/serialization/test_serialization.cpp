// REV-095: la decisión que hay ANTES de reservar memoria con un número que
// salió de la tarjeta.
//
// El I/O queda del otro lado (necesita HalFile); lo que puede abortar el
// firmware es el `resize()`/`reserve()`, y quien lo autoriza es `fits()`.
#include <cstdint>
#include <cstdio>

// El header real arrastra HalStorage; acá se copia SOLO la función pura, y el
// `static_assert` de abajo la ata a la del header para que no se separen.
namespace serialization {
inline constexpr uint32_t MAX_STRING = 64u * 1024u;
constexpr bool fits(const uint32_t len, const uint32_t cap, const uint64_t bytesLeft) {
  return len <= cap && static_cast<uint64_t>(len) <= bytesLeft;
}
}  // namespace serialization

static int fallos = 0;

static void esperar(const char* caso, const uint32_t len, const uint32_t cap, const uint64_t left,
                    const bool esperado) {
  const bool dio = serialization::fits(len, cap, left);
  if (dio != esperado) {
    std::printf("  FALLA %-52s len=%u cap=%u quedan=%llu -> %s\n", caso, len, cap,
                static_cast<unsigned long long>(left), dio ? "entra" : "NO entra");
    ++fallos;
  } else {
    std::printf("  ok    %-52s -> %s\n", caso, dio ? "entra" : "NO entra");
  }
}

int main() {
  using serialization::MAX_STRING;
  std::printf("serialization: el tope antes del resize\n");

  // Los cuatro que pidió la revisión, sobre el largo de una cadena.
  esperar("largo 0: valido y no reserva nada", 0, MAX_STRING, 1000, true);
  esperar("justo el maximo", MAX_STRING, MAX_STRING, MAX_STRING, true);
  esperar("el maximo + 1", MAX_STRING + 1, MAX_STRING, 1u << 30, false);
  esperar("UINT32_MAX (FAT en la basura)", UINT32_MAX, MAX_STRING, 1ull << 40, false);

  // Archivo TRUNCADO: entra en el tope pero no en lo que queda del archivo.
  // Este es el caso que un tope a secas no ataja.
  esperar("cabe en el tope pero el archivo se acabo", 500, MAX_STRING, 499, false);
  esperar("cabe justo en lo que queda", 500, MAX_STRING, 500, true);
  esperar("truncado en medio del propio largo (quedan 0)", 8, MAX_STRING, 0, false);

  // Contadores: el tope semantico manda aunque el archivo sea enorme.
  esperar("512 elementos de pagina contra tope 512", 512, 512, 100000, true);
  esperar("513 elementos: caché invalida", 513, 512, 100000, false);
  esperar("65535 elementos (el u16 entero)", 65535, 512, 1ull << 30, false);
  esperar("4096 anclas contra su tope", 4096, 4096, 1ull << 20, true);

  // Un tope de 0 no deja pasar nada con contenido, pero 0 sigue siendo valido.
  esperar("tope 0 con 1 byte", 1, 0, 1000, false);
  esperar("tope 0 con 0 bytes", 0, 0, 1000, true);

  // constexpr: esto se comprueba al compilar, no al correr.
  static_assert(serialization::fits(0, 10, 10), "");
  static_assert(serialization::fits(10, 10, 10), "");
  static_assert(!serialization::fits(11, 10, 100), "");
  static_assert(!serialization::fits(10, 10, 9), "");

  if (fallos) {
    std::printf("serialization: %d FALLOS\n", fallos);
    return 1;
  }
  std::printf("serialization: todo en orden\n");
  return 0;
}
