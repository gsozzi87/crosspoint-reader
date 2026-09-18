#!/usr/bin/env bash
# Lo que dice el bot de libros, leído de escritorio (server/src/librosParse.ts):
# la lista `Título /comando` y la ficha `Título - Autor` con sus botones, con
# los textos exactos de las dos capturas del dueño. Sin Telegram y sin red.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/libros/parse.test.ts
