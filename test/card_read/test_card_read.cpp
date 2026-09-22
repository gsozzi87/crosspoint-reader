// REV-059: la decisión de leer o no leer un archivo de la tarjeta.
//
// Lo que se prueba acá es `judge()`, que es todo lo que hay que decidir antes
// de `resize()`. El I/O queda del otro lado (`readCapped()` en el .cpp) porque
// necesita la tarjeta; lo que aborta el firmware es el `resize`, y quien lo
// autoriza es esta función.
#include <cstdio>
#include <cstdlib>

#include "../../src/util/CardRead.h"

static int fallos = 0;

static const char* nombre(const cardread::Verdict v) {
  switch (v) {
    case cardread::Verdict::Empty:
      return "Empty";
    case cardread::Verdict::Ok:
      return "Ok";
    case cardread::Verdict::TooBig:
      return "TooBig";
  }
  return "?";
}

static void esperar(const char* caso, const uint64_t size, const size_t cap, const cardread::Verdict esperado) {
  const cardread::Verdict dio = cardread::judge(size, cap);
  if (dio != esperado) {
    std::printf("  FALLA %-44s size=%llu cap=%zu -> %s (se esperaba %s)\n", caso, static_cast<unsigned long long>(size),
                cap, nombre(dio), nombre(esperado));
    ++fallos;
  } else {
    std::printf("  ok    %-44s -> %s\n", caso, nombre(dio));
  }
}

int main() {
  using cardread::Verdict;
  const size_t CAP = cardread::CAP_JSON_CACHE;  // 256 KB

  std::printf("card_read: la decisión antes del resize\n");

  // Los cuatro que pidió la revisión.
  esperar("0 bytes: no es error, es que no hay nada", 0, CAP, Verdict::Empty);
  esperar("justo el tope: entra", CAP, CAP, Verdict::Ok);
  esperar("el tope + 1: NO entra", static_cast<uint64_t>(CAP) + 1, CAP, Verdict::TooBig);
  esperar("tamaño absurdo (4 GiB)", 4ULL * 1024 * 1024 * 1024, CAP, Verdict::TooBig);

  // El caso de verdad: una FAT dañada.
  esperar("64 bits enteros (FAT en la basura)", UINT64_MAX, CAP, Verdict::TooBig);

  // POR QUÉ el tamaño se pide en 64 bits. `HalFile::size()` devuelve size_t,
  // que en el S3 son 32 bits: 4 GiB + 100 se leería como 100 y pasaría
  // cualquier tope. Con `fileSize64()` no pasa.
  const uint64_t truncaA100 = 4ULL * 1024 * 1024 * 1024 + 100;
  esperar("4 GiB + 100 (size_t lo vería como 100)", truncaA100, CAP, Verdict::TooBig);
  if (static_cast<size_t>(truncaA100) == 100 && sizeof(size_t) == 4) {
    std::printf("  (acá size_t son 32 bits y de verdad truncaría a 100)\n");
  }

  // Lo normal, con números medidos de verdad.
  esperar("manifiesto de Noticias con 40 notas (8267 B)", 8267, CAP, Verdict::Ok);
  esperar("Salmos, el libro más grande (207 KB)", 207u * 1024, cardread::CAP_BOOK, Verdict::Ok);
  esperar("Salmos contra el tope de una nota: NO", 207u * 1024, cardread::CAP_ARTICLE, Verdict::TooBig);

  // Un tope de 0 no deja pasar nada: vale como "esto no se lee".
  esperar("tope 0 con un archivo de 1 byte", 1, 0, Verdict::TooBig);
  esperar("tope 0 con un archivo vacío", 0, 0, Verdict::Empty);

  // 1 byte siempre entra donde haya tope.
  esperar("1 byte contra 1 de tope", 1, 1, Verdict::Ok);
  esperar("2 bytes contra 1 de tope", 2, 1, Verdict::TooBig);

  // Y que los topes estén ordenados: una nota no puede ser más grande que un
  // libro, ni un libro más chico que una caché JSON.
  static_assert(cardread::CAP_ARTICLE < cardread::CAP_JSON_CACHE, "una nota es más chica que un manifiesto");
  static_assert(cardread::CAP_JSON_CACHE < cardread::CAP_BOOK, "un libro entero es el más grande");

  // `judge` es constexpr: esto se comprueba al compilar, no al correr.
  static_assert(cardread::judge(0, 10) == Verdict::Empty, "");
  static_assert(cardread::judge(10, 10) == Verdict::Ok, "");
  static_assert(cardread::judge(11, 10) == Verdict::TooBig, "");

  if (fallos) {
    std::printf("card_read: %d FALLOS\n", fallos);
    return 1;
  }
  std::printf("card_read: todo en orden\n");
  return 0;
}
