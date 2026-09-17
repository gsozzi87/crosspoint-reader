#!/usr/bin/env bash
# La salida a internet del servidor: el contrato del lookup fijado y el filtro
# de direcciones. Sin red: el servidor de prueba es local.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/net_lookup/lookup.test.ts
