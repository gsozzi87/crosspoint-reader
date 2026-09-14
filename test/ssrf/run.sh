#!/usr/bin/env bash
# El filtro de URLs que salen a internet (SSRF), sin red.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/ssrf/ssrf.test.ts
