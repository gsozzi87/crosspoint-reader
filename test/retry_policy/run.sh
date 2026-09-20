#!/usr/bin/env bash
# REV-054: reintentar AHORA y conservar para MÁS TARDE son dos preguntas.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra test/retry_policy/test_retry.cpp -o "$OUT/t"
"$OUT/t"
