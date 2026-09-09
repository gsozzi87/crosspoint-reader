#!/usr/bin/env python3
"""Agrega (o cambia) claves de traducción en lib/I18n/translations/*.yaml.

Existe por dos motivos:

  1. una clave nueva hay que ponerla en los SIETE idiomas del producto y a mano
     es un despiste asegurado;
  2. cuando hay varias tareas tocando el repo a la vez, dos ediciones del mismo
     yaml se pisan. Acá el archivo se abre con flock, así el que llega segundo
     espera en vez de perder lo del primero.

Uso:
    python3 scripts/add_i18n.py claves.json

donde claves.json es:

    {
      "STR_LO_QUE_SEA": {
        "es": "Texto", "en": "Text", "fr": "...", "de": "...",
        "pt-BR": "...", "pt-PT": "...", "ru": "..."
      }
    }

Los idiomas que falten se completan con el inglés. Una clave que ya exista se
reemplaza. Las claves se agregan al final del archivo.
"""
import fcntl
import json
import os
import sys

FILES = {
    "es": "spanish.yaml",
    "en": "english.yaml",
    "fr": "french.yaml",
    "de": "german.yaml",
    "pt-BR": "portuguese-BR.yaml",
    "pt-PT": "portuguese-PT.yaml",
    "ru": "russian.yaml",
}
BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib", "I18n", "translations")


def quote(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as fh:
        keys = json.load(fh)

    for lang, filename in FILES.items():
        path = os.path.join(BASE, filename)
        with open(path, "r+", encoding="utf-8") as fh:
            fcntl.flock(fh, fcntl.LOCK_EX)
            lines = fh.read().split("\n")
            existing = {}
            for i, line in enumerate(lines):
                name = line.split(":")[0].strip()
                if name.startswith("STR_"):
                    existing[name] = i
            added = 0
            for key, values in keys.items():
                text = values.get(lang) or values.get("en") or ""
                entry = "%s: %s" % (key, quote(text))
                if key in existing:
                    lines[existing[key]] = entry
                else:
                    while lines and lines[-1].strip() == "":
                        lines.pop()
                    lines.append(entry)
                    added += 1
            fh.seek(0)
            fh.write("\n".join(lines) + "\n")
            fh.truncate()
        print("%s: %d nuevas, %d en total" % (filename, added, len(keys)))


if __name__ == "__main__":
    main()
