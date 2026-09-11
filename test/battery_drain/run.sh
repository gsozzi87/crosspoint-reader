#!/usr/bin/env bash
# Compila y corre la prueba de la cuenta del diario de batería.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -I src/util test/battery_drain/test_drain.cpp -o "$OUT/test_drain"
"$OUT/test_drain"
