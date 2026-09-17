#!/usr/bin/env bash
# La poda del log guardado a 24 h, sin red y sin aparato.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/device_log/prune.test.ts
