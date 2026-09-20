#!/usr/bin/env bash
# REV-047: los fallos del proveedor quedan anotados y a la vista.
set -euo pipefail
cd "$(dirname "$0")/../../server"
bun test ../test/provider_log/provider_log.test.ts
