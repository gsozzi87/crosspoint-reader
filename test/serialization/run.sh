#!/usr/bin/env bash
# REV-095: una caché de la tarjeta no puede tumbar el aparato. El `resize()` de
# un firmware sin excepciones no falla: aborta, así que el número que sale del
# archivo se comprueba ANTES.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -Werror test/serialization/test_serialization.cpp -o "$OUT/t"
"$OUT/t"
# La copia de `fits()` de la prueba tiene que ser la MISMA que la del header: si
# alguien cambia una y no la otra, esto lo dice en vez de que la prueba mienta.
grep -q 'return len <= cap && static_cast<uint64_t>(len) <= bytesLeft;' lib/Serialization/Serialization.h \
  || { echo "ERROR: fits() del header ya no coincide con la de la prueba" >&2; exit 1; }
echo "serialization: fits() del header coincide con la de la prueba"
