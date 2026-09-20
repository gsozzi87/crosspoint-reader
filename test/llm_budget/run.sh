#!/usr/bin/env bash
# REV-049: el presupuesto de salida y el 200 con el texto vacío.
set -euo pipefail
cd "$(dirname "$0")/../../server"
bun test ../test/llm_budget/budget.test.ts
bun test ../test/llm_budget/empty_reply.test.ts
