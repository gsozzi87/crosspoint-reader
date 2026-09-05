#!/usr/bin/env bash
# Equivalente Linux de release.ps1: bump del build, compila y sube el .bin al servidor OTA.
# Requiere WS397_OTA_URL y WS397_OTA_TOKEN en el entorno (en Claude Code web: variables del environment).
set -euo pipefail
cd "$(dirname "$0")"

: "${WS397_OTA_URL:?Falta WS397_OTA_URL}"
: "${WS397_OTA_TOKEN:?Falta WS397_OTA_TOKEN}"

build=$(( $(cat .ws397-build 2>/dev/null || echo 0) + 1 ))
version="1.5.$build"
sed -i -E "s/#define WS397_BUILD [0-9]+/#define WS397_BUILD $build/" include/ws397_version.h

echo "Compilando $version-ws397..."
python3 -m platformio run -e ws397
echo "$build" > .ws397-build

bin=.pio/build/ws397/firmware.bin
put_url="${WS397_OTA_URL%/latest}"
echo "Subiendo $bin a $put_url..."
curl -fsS -X PUT "$put_url" \
  -H "Authorization: Bearer $WS397_OTA_TOKEN" \
  -H "X-Version: $version" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@$bin"
echo
echo "Listo: en el aparato, Settings -> Check for updates instala $version"
