#!/usr/bin/env bash
# Compila y corre la prueba del cajón de arena de las apps en Lua.
#
# Los .c de Lua van con gcc (C) y lo nuestro con g++ (C++): pasarle los .c a g++
# los compila como C++ y los nombres no coinciden con el extern "C" de nuestros
# headers, que es exactamente el error que da si se hace todo de una.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

for f in lib/Lua/src/*.c; do
  gcc -std=c99 -O1 -w -I lib/Lua/src -c "$f" -o "$OUT/$(basename "$f" .c).o"
done
g++ -std=c++17 -O1 -w -I lib/Lua/src -I src/lua \
  test/lua_sandbox/test_sandbox.cpp src/lua/LuaSandbox.cpp "$OUT"/*.o -lm -o "$OUT/test_sandbox"
"$OUT/test_sandbox"
