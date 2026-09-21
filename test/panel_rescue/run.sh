#!/usr/bin/env bash
# REV-063: el ciclo de corriente al panel se cuenta, y un fallo del PMIC no
# gasta el único intento.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra test/panel_rescue/test_rescue.cpp -o "$OUT/t"
"$OUT/t"
