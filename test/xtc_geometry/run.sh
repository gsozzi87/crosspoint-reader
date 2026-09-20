#!/usr/bin/env bash
# REV-009: la geometría de las páginas XTH de 2 bits, sin placa.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -I lib/Xtc/Xtc test/xtc_geometry/test_geometry.cpp -o "$OUT/test_geometry"
"$OUT/test_geometry"
