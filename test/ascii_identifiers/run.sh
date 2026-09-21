#!/usr/bin/env bash
# REV-083: ningun IDENTIFICADOR del firmware puede llevar caracteres no ASCII.
#
# cppcheck no los soporta y aborta el analisis entero con
# "unhandled character(s) (character code=195)". Eso dejo la CI en rojo cuatro
# commits seguidos por un parametro llamado `qué`, y encima se publico una OTA
# encima de esa CI roja.
#
# Los COMENTARIOS y los STRINGS en castellano se quedan como estan: es medio
# repo y cppcheck no tiene problema con ellos. Lo unico que rompe es un
# identificador.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 test/ascii_identifiers/check.py
