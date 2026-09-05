#!/usr/bin/env bash
# Setup script del environment de Claude Code web (se pega en la configuración del entorno).
# Además de esto, el environment necesita las variables WS397_OTA_URL y WS397_OTA_TOKEN:
# sin ellas release.sh no sube, y el .bin queda con CROSSPOINT_OTA_RELEASE_URL="" (el
# aparato no encontraría updates).
set -e
git submodule update --init --recursive
pip install -q platformio scons==4.8.1

# --- Workarounds para la política de egreso del entorno (verificados 2026-09-05) ---
# Bloqueado: api.registry.platformio.org / dl.registry.platformio.org (registro de PlatformIO),
# github.com/*/archive/* y codeload.github.com. Permitido: PyPI y github.com/*/releases/download/*.
PIO=${HOME}/.platformio
CA=/root/.ccr/agent-proxy-ca.crt

# 1) tool-scons viene del registro (bloqueado): paquete local sobre el scons de PyPI.
mkdir -p "$PIO/packages/tool-scons"
cat > "$PIO/packages/tool-scons/package.json" <<'JSON'
{"name": "tool-scons", "version": "4.40801.0", "description": "SCons 4.8.1 from PyPI (registry unreachable)", "system": ["*"]}
JSON
cat > "$PIO/packages/tool-scons/.piopm" <<'JSON'
{"type": "tool", "name": "tool-scons", "version": "4.40801.0", "spec": {"owner": "platformio", "id": null, "name": "tool-scons", "requirements": "~4.40801.0", "uri": null}}
JSON
cat > "$PIO/packages/tool-scons/scons.py" <<'PY'
import sys
from SCons.Script import main
if __name__ == "__main__":
    sys.exit(main())
PY

# 2) Las librerías del registro, clonadas de GitHub en el tag exacto (WebSockets 2.7.3 no
#    tiene tag: es master).
libdeps=.pio/libdeps/ws397
mkdir -p "$libdeps"
lib() { # name repo ref owner version requirements
  [ -d "$libdeps/$1" ] && return 0
  git clone -q --depth 1 ${3:+--branch "$3"} "https://github.com/$2" "$libdeps/$1"
  rm -rf "$libdeps/$1/.git"
  printf '{"type": "library", "name": "%s", "version": "%s", "spec": {"owner": "%s", "id": null, "name": "%s", "requirements": "%s", "uri": null}}' \
    "$1" "$5" "$4" "$1" "$6" > "$libdeps/$1/.piopm"
}
lib SdFat greiman/SdFat 2.3.1 greiman 2.3.1 '^2.3.1'
lib ArduinoJson bblanchon/ArduinoJson v7.4.2 bblanchon 7.4.2 7.4.2
lib QRCode ricmoo/QRCode v0.0.1 ricmoo 0.0.1 0.0.1
lib PNGdec bitbank2/PNGdec 1.1.6 bitbank2 1.1.6 1.1.6
lib Arduino-wolfSSL wolfSSL/Arduino-wolfSSL 5.7.2 wolfssl 5.7.2 5.7.2
lib WebSockets Links2004/arduinoWebSockets "" links2004 2.7.3 2.7.3

python3 -m platformio settings set enable_telemetry No >/dev/null 2>&1 || true

# 3) El venv "penv" de pioarduino recién existe después del primer `run` (que falla
#    instalando su platformio-core desde github.com/.../archive, 403). Crearlo y arreglarlo:
python3 -m platformio run -e ws397 -t nobuild >/dev/null 2>&1 || true
if [ -x "$PIO/penv/bin/uv" ]; then
  # misma versión de platformio-core desde PyPI: el instalador lo da por cumplido
  UV_CACHE_DIR=$PIO/.cache/uv "$PIO/penv/bin/uv" pip install --python="$PIO/penv/bin/python" -q platformio==6.1.19 scons==4.8.1
  # penv_setup.py pisa SSL_CERT_FILE con el certifi de su venv: agregarle la CA del proxy
  cert=$("$PIO/penv/bin/python" -c 'import certifi;print(certifi.where())')
  grep -q -- "$(sed -n 2p "$CA")" "$cert" 2>/dev/null || cat "$CA" >> "$cert"
fi
# Primer build para cachear la toolchain (opcional, tarda varios minutos la primera vez)
# python3 -m platformio run -e ws397 || true
