#!/usr/bin/env bash
# REV-063 y REV-070: el ciclo de corriente se cuenta —lo use el panel o los
# rieles— y un fallo del PMIC no gasta el único intento.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra test/rescue_state/test_rescue_state.cpp -o "$OUT/t"
"$OUT/t"
