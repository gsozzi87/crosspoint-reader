#!/usr/bin/env bash
# REV-059: leer un archivo de la tarjeta con tope. Un `resize()` imposible en un
# firmware sin excepciones no lanza: aborta, y el camino que lo recorre es el de
# suspender y apagar.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -Werror test/card_read/test_card_read.cpp -o "$OUT/t"
"$OUT/t"
