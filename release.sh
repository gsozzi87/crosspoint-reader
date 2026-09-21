#!/usr/bin/env bash
# Equivalente Linux de release.ps1: bump del build, compila y sube el .bin al servidor OTA.
# Requiere WS397_OTA_TOKEN. WS397_OTA_URL permite sobrescribir el servidor.
set -euo pipefail
cd "$(dirname "$0")"

: "${WS397_OTA_TOKEN:?Falta WS397_OTA_TOKEN}"
WS397_OTA_URL="${WS397_OTA_URL:-https://paper-esp32.up.railway.app/firmware/latest}"

# EL PORTON DE CI (REV-083). Hasta 1.5.120 esto no existia y se publico una OTA
# con la CI del mismo SHA en ROJO — y peor, llevaba cuatro commits en rojo sin
# que nadie lo mirara. "El pio run local salio limpio" NO es el gate: el build
# de una placa no corre cppcheck, ni los tests de escritorio, ni las otras cinco
# placas. Publicar con la red de seguridad caida es exactamente lo que no hay
# que hacer en una tanda que toca los caminos de energia.
#
# Se comprueba el SHA EXACTO que se va a servir. Si la CI de ese commit no esta
# en success (falla, o todavia corre, o no existe), no se sube nada.
sha=$(git rev-parse HEAD)
if [ "${WS397_SKIP_CI_GATE:-}" = "1" ]; then
  echo "!!! OJO: el porton de CI esta SALTEADO a mano (WS397_SKIP_CI_GATE=1) para $sha"
else
  echo "Mirando la CI de $sha..."
  ci=$(curl -fsS "https://api.github.com/repos/gsozzi87/crosspoint-reader/actions/runs?head_sha=$sha&per_page=5" 2>/dev/null        | python3 -c "
import sys, json
try:
    runs = json.load(sys.stdin).get('workflow_runs', [])
except Exception:
    print('sin-respuesta'); raise SystemExit
if not runs:
    print('sin-corrida'); raise SystemExit
r = runs[0]
print(r['conclusion'] or r['status'])
" 2>/dev/null || echo "sin-respuesta")
  if [ "$ci" != "success" ]; then
    echo "ABORTADO: la CI de $sha esta en '$ci', no en 'success'." >&2
    echo "  - si todavia corre, espera a que termine;" >&2
    echo "  - si fallo, arreglalo: un binario que compila no dice nada de cppcheck" >&2
    echo "    ni de las pruebas de escritorio ni de las otras placas;" >&2
    echo "  - si la CI esta caida y sabes lo que haces: WS397_SKIP_CI_GATE=1 ./release.sh" >&2
    exit 1
  fi
  echo "CI de $sha: success"
fi

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
