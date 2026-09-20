#!/usr/bin/env bash
# REV-016: el POST que se reintenta no se aplica dos veces.
set -euo pipefail
cd "$(dirname "$0")/../../server"
bun test ../test/idempotency/idempotency.test.ts
bun test ../test/idempotency/metering.test.ts
bun test ../test/idempotency/journey.test.ts
