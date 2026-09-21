#!/usr/bin/env bash
# Equivalente Linux de release.ps1: bump del build, compila y sube el .bin al servidor OTA.
# Requiere WS397_OTA_TOKEN. WS397_OTA_URL permite sobrescribir el servidor.
set -euo pipefail
cd "$(dirname "$0")"

: "${WS397_OTA_TOKEN:?Falta WS397_OTA_TOKEN}"
WS397_OTA_URL="${WS397_OTA_URL:-https://paper-esp32.up.railway.app/firmware/latest}"

# EL PORTON DE RELEASE (REV-083). Arbol limpio + la CI de ESTE SHA en verde,
# antes del bump. La regla vive en tools/release_gate.py y no acá, porque
# release.ps1 tiene que aplicar exactamente la misma: un porton que vive en uno
# de los dos caminos no es un porton, es una sugerencia.
python3 tools/release_gate.py

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

# El PUT contestó 200, pero eso no dice qué quedó servido. Sin esta comprobación
# el repositorio podía quedar diciendo N mientras /firmware/latest seguía
# entregando N-1, y el único síntoma era que el aparato no veía la
# actualización (la comparación es major.minor.patch estricta).
echo "Verificando lo que quedó servido en $WS397_OTA_URL..."
servido=$(curl -fsS -m 30 "$WS397_OTA_URL" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
if [ "$servido" != "$version" ]; then
  echo "ERROR: se subió $version pero /firmware/latest entrega '${servido:-nada}'." >&2
  echo "       El release NO está publicado. No anunciarlo hasta que coincidan." >&2
  exit 1
fi
echo "OK: /firmware/latest entrega $servido"

echo "Listo: en el aparato, Settings -> Check for updates instala $version"
