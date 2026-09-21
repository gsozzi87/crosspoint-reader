#!/usr/bin/env python3
"""EL PORTON DE RELEASE (REV-083), compartido por release.sh y release.ps1.

Hasta 1.5.120 esto no existia y se publico una OTA con la CI del mismo SHA en
ROJO — y peor, llevaba cuatro commits en rojo sin que nadie lo mirara. "El pio
run local salio limpio" NO es el gate: el build de una placa no corre cppcheck,
ni los tests de escritorio, ni las otras cinco placas.

La primera version del porton vivia SOLO en release.sh y tenia tres agujeros
que marco la segunda revision:

  1. `release.ps1` —el camino natural en Windows, y el unico que flashea por
     cable— no consultaba nada. Un porton que vive en uno de los dos caminos no
     es un porton, es una sugerencia.
  2. No exigia el arbol limpio. Con cambios sin commitear se podia aprobar la CI
     de un HEAD verde y despues compilar y subir codigo DISTINTO del que la CI
     probo. El binario no sale de lo que dice `git rev-parse HEAD`: sale de lo
     que hay en el disco.
  3. Tomaba `workflow_runs[0]` sin mirar de que workflow era. El dia que corra
     otro workflow sobre el mismo SHA, el porton pasa a contestar sobre algo que
     no es la CI.

Por eso esto es un archivo aparte y no dos copias: dos copias de una regla se
separan solas, como ya paso con las rutas protegidas en 1.5.91.

Salida: 0 si se puede publicar, 1 si no. Todo lo que explica va a stderr.
"""

import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

REPO = os.environ.get("WS397_REPO", "gsozzi87/crosspoint-reader")
WORKFLOW_PATH = ".github/workflows/ci.yml"


def fallar(*lineas: str) -> None:
    for linea in lineas:
        print(linea, file=sys.stderr)
    sys.exit(1)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout.strip()


def main() -> None:
    if os.environ.get("WS397_SKIP_CI_GATE") == "1":
        print("!!! OJO: el porton de release esta SALTEADO a mano (WS397_SKIP_CI_GATE=1).")
        print("!!! Lo que se publique NO tiene ninguna red de seguridad detras.")
        return

    # 1. El arbol tiene que estar limpio, o el binario no es el SHA que se mira.
    sucio = git("status", "--porcelain")
    if sucio:
        fallar(
            "ABORTADO: hay cambios sin commitear. El binario sale del disco, no del SHA,",
            "           asi que la CI de HEAD no dice nada sobre lo que se iba a subir.",
            "",
            sucio,
            "",
            "  - commitea (o guarda con `git stash`) y volve a correrlo;",
            "  - si venis de un release anterior, lo que falta commitear es",
            "    include/ws397_version.h y .ws397-build.",
        )

    sha = git("rev-parse", "HEAD")

    # 2. La CI de ESE SHA, y la CI de verdad: se filtra por el archivo del
    #    workflow, no por el primero que devuelva la API.
    url = f"https://api.github.com/repos/{REPO}/actions/runs?head_sha={sha}&per_page=20"
    print(f"Mirando la CI de {sha}...")
    try:
        with urllib.request.urlopen(url, timeout=30) as resp:
            runs = json.load(resp).get("workflow_runs", [])
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as err:
        fallar(
            f"ABORTADO: no se pudo consultar la CI ({type(err).__name__}: {err}).",
            "  - sin respuesta no se publica: el porton que falla abierto no es un porton;",
            "  - si la CI esta caida y sabes lo que haces: WS397_SKIP_CI_GATE=1.",
        )

    propias = [r for r in runs if r.get("path") == WORKFLOW_PATH]
    if not propias:
        otros = sorted({r.get("path", "?") for r in runs})
        fallar(
            f"ABORTADO: no hay ninguna corrida de {WORKFLOW_PATH} para {sha}.",
            f"           Lo que hay: {', '.join(otros) if otros else 'nada'}.",
            "  - si todavia no arranco, espera unos segundos;",
            "  - si el commit no esta empujado, empujalo: la CI corre en GitHub, no aca.",
        )

    # La mas nueva de las propias (un re-run deja varias).
    run = propias[0]
    estado = run.get("conclusion") or run.get("status")
    if estado != "success":
        fallar(
            f"ABORTADO: la CI de {sha} esta en '{estado}', no en 'success'.",
            f"           {run.get('html_url', '')}",
            "  - si todavia corre, espera a que termine;",
            "  - si fallo, arreglalo: un binario que compila no dice nada de cppcheck",
            "    ni de las pruebas de escritorio ni de las otras placas;",
            "  - si la CI esta caida y sabes lo que haces: WS397_SKIP_CI_GATE=1.",
        )

    print(f"CI de {sha}: success ({run.get('html_url', '')})")


if __name__ == "__main__":
    main()
