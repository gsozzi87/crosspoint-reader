"""REV-083: identificadores sin acentos, que es lo unico que cppcheck no traga.

Se quitan comentarios y literales de cadena/caracter y se mira si queda algun
byte fuera de ASCII. Lo que queda despues de eso es codigo: tipos, nombres de
variables, de funciones y de macros.
"""
import re
import subprocess
import sys

# Sólo lo nuestro: el SDK y las bibliotecas de terceros tienen sus propias reglas.
RAICES = ("src/", "lib/")
SALTAR = ("lib/Lua/", "lib/HelixMp3/", "lib/uzlib/", "lib/miniz/", "lib/EpdFont/builtinFonts/")
EXT = (".c", ".cpp", ".h", ".hpp")


def codigo_sin_texto(txt: str) -> str:
    txt = re.sub(r"//[^\n]*", "", txt)
    txt = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
    txt = re.sub(r'"(\\.|[^"\\\n])*"', '""', txt)
    txt = re.sub(r"'(\\.|[^'\\\n])*'", "''", txt)
    return txt


def main() -> int:
    archivos = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    malos = []
    mirados = 0
    for f in archivos:
        if not f.startswith(RAICES) or not f.endswith(EXT):
            continue
        if any(f.startswith(s) for s in SALTAR):
            continue
        mirados += 1
        try:
            txt = open(f, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        for n, linea in enumerate(codigo_sin_texto(txt).split("\n"), 1):
            fuera = [c for c in linea if ord(c) > 127]
            if fuera:
                malos.append(f"{f}:{n}: {''.join(sorted(set(fuera)))} en: {linea.strip()[:70]}")
    if malos:
        print("Identificadores con caracteres no ASCII (cppcheck aborta con esto):")
        for m in malos:
            print("  " + m)
        print(f"\n{len(malos)} linea(s). Los comentarios y los strings pueden llevar acentos; los NOMBRES no.")
        return 1
    print(f"ascii_identifiers: {mirados} archivos, todos los identificadores en ASCII")
    return 0


if __name__ == "__main__":
    sys.exit(main())
