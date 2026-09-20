#!/usr/bin/env bash
# REV-017: una entrada de la cola de la cuenta A no se aplica sobre la cuenta B.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -I lib/ServerClient test/queue_account/test_queue_account.cpp -o "$OUT/t"
"$OUT/t"
