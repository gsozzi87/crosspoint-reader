#!/usr/bin/env bash
# REV-048: el mensaje de error del servidor, recortado sin romper UTF-8.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -I lib/ServerClient test/server_error_text/test_error_text.cpp -o "$OUT/test_error_text"
"$OUT/test_error_text"
