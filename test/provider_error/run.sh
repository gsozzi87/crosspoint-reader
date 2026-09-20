#!/usr/bin/env bash
# REV-054: un tope de uso del proveedor se dice, no se vuelca crudo.
set -euo pipefail
cd "$(dirname "$0")/../../server"
bun test ../test/provider_error/provider_error.test.ts
