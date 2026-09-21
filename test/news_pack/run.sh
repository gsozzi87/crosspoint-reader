#!/usr/bin/env bash
# El reparto de titulares del paquete de noticias, sin red y sin aparato.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/news_pack/pack.test.ts
bun test ../test/news_pack/rebuild.test.ts

bun test ../test/news_pack/medical.test.ts
bun test ../test/news_pack/medical_pack.test.ts
bun test ../test/news_pack/titles.test.ts
bun test ../test/news_pack/ondemand.test.ts
