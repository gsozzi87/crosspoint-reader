#!/usr/bin/env python3
"""Genera los bitmaps de los juegos: memoryIcons.h y cardIcons.h.

Son iconos de verdad, no dibujos a mano: el usuario probó los juegos en el
aparato y las figuras dibujadas con primitivas se veían pobres.

  - memoryIcons.h: las dieciséis figuras del juego de memoria, de los iconos
    Lucide (MIT, https://lucide.dev), a 48 y 64 px.
  - cardIcons.h: los cuatro palos del blackjack (SVG propios de relleno macizo:
    los de Lucide son de contorno y rellenos quedan como manchas) a 16 y 40 px,
    más los trece valores de la carta sacados de DejaVu Sans Bold. Los valores
    van como bitmap y no como texto porque hay que girarlos 180 grados en la
    esquina de abajo de la carta, y la fuente del aparato no se puede girar.

El formato es el mismo `freeink::Icon` que usa el resto del proyecto (ver
freeink-sdk/libs/assets/Icons/tools/gen_icons.py), así que se dibujan con el
mismo bucle de drawPixel y salen bien en cualquier orientación.

Uso (hace falta red la primera vez, para bajar los SVG de Lucide):

    pip install resvg-py pillow
    python3 src/activities/games/gen_game_icons.py

En la nube no hay rsvg-convert; por eso este script usa resvg_py en vez del
subprocess que usa el generador del SDK.
"""
import io
import os
import re
import sys
import urllib.request

import resvg_py
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(OUT, ".lucide-cache")
LUCIDE = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/{}.svg"
THRESHOLD = 128
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Dieciséis figuras bien distintas entre sí: hacen falta quince para el nivel
# más grande del juego de memoria y no se pueden confundir de un vistazo.
MEMORY = ["heart", "star", "sun", "moon", "cloud", "umbrella", "anchor", "key",
          "gift", "fish", "bird", "cat", "rocket", "ghost", "crown", "flame"]
MEMORY_SIZES = [48, 64]

SUIT_SIZES = [16, 40]
SUITS = {
    "spade": ('<path d="M50 4 C 80 28 97 49 97 66 C 97 80 86 90 73 90 C 63 90 55 85 50 77 '
              'C 45 85 37 90 27 90 C 14 90 3 80 3 66 C 3 49 20 28 50 4 Z"/>'
              '<path d="M50 62 C 53 80 59 92 68 97 L 32 97 C 41 92 47 80 50 62 Z"/>'),
    "heart": ('<path d="M50 93 C 20 69 3 51 3 32 C 3 15 15 4 29 4 C 39 4 46 10 50 19 '
              'C 54 10 61 4 71 4 C 85 4 97 15 97 32 C 97 51 80 69 50 93 Z"/>'),
    "diamond": ('<path d="M50 2 C 62 22 76 38 93 50 C 76 62 62 78 50 98 '
                'C 38 78 24 62 7 50 C 24 38 38 22 50 2 Z"/>'),
    "club": ('<circle cx="50" cy="26" r="20"/><circle cx="25" cy="60" r="20"/>'
             '<circle cx="75" cy="60" r="20"/>'
             '<path d="M43 60 C 45 80 42 92 32 98 L 68 98 C 58 92 55 80 57 60 Z"/>'),
}
RANKS = ["A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"]
RANK_PX = 26  # cuerpo de la fuente; el alto del bitmap sale de acá


def lucide(name):
    """Baja (y cachea) el SVG de Lucide."""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name + ".svg")
    if not os.path.exists(path):
        with urllib.request.urlopen(LUCIDE.format(name), timeout=30) as r:
            open(path, "wb").write(r.read())
    return open(path).read()


def render(svg, px):
    png = resvg_py.svg_to_bytes(svg_string=svg, width=px, height=px, background="#ffffff")
    img = Image.open(io.BytesIO(bytes(png))).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    return bg.convert("L")


def pack(img):
    """A 1 bpp (MSB primero, 1 = transparente, 0 = negro) + el centro óptico."""
    w, h = img.size
    pix = img.load()
    data, sy, n = [], 0, 0
    for y in range(h):
        for xb in range(0, w, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                white = 1
                if x < w and pix[x, y] < THRESHOLD:
                    white, sy, n = 0, sy + y, n + 1
                byte |= white << (7 - b)
            data.append(byte)
    return data, (round(sy / n) if n else h // 2)


def emit(path, comment, entries):
    lines = ["#pragma once", "", '#include "Icon.h"', "", comment, ""]
    for alias, imgs in entries:
        for px, img in imgs:
            data, center = pack(img)
            arr = "icon_{}_{}_bits".format(re.sub(r"[^A-Za-z0-9]", "_", alias), px)
            lines.append("static const uint8_t {}[] = {{{}}};".format(
                arr, ", ".join("0x%02X" % b for b in data)))
            lines.append("static const freeink::Icon icon_{}_{} = {{{}, {}, {}, {}}};".format(
                re.sub(r"[^A-Za-z0-9]", "_", alias), px, img.size[0], img.size[1], center, arr))
        lines.append("")
    open(path, "w").write("\n".join(lines))
    print("escrito", path)


def main():
    # --- memoria ---
    # El trazo de Lucide viene en 2 px sobre una caja de 24: se engorda un poco
    # para que a 48 px se lea de lejos en tinta electrónica.
    css = "* { stroke-width: 2.4px }"
    mem = []
    for n in MEMORY:
        svg = lucide(n)
        imgs = []
        for px in MEMORY_SIZES:
            png = resvg_py.svg_to_bytes(svg_string=svg, width=px, height=px,
                                        background="#ffffff", style_sheet=css)
            img = Image.open(io.BytesIO(bytes(png))).convert("RGBA")
            bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
            bg.paste(img, mask=img.split()[3])
            imgs.append((px, bg.convert("L")))
        mem.append((n, imgs))
    emit(os.path.join(OUT, "memoryIcons.h"),
         "// Figuras del juego de memoria: iconos Lucide (MIT, https://lucide.dev) rasterizados a\n"
         "// 1 bpp con el mismo formato que src/components/icons/*.h (freeink::Icon).\n"
         "// Generado por src/activities/games/gen_game_icons.py; no editar a mano.\n"
         "// Iconos: " + ", ".join(MEMORY) + ".\n"
         "// Tamanos: " + ", ".join(str(s) for s in MEMORY_SIZES) + " px.",
         mem)

    # --- cartas: palos y valores ---
    entries = []
    for name, body in SUITS.items():
        imgs = []
        for px in SUIT_SIZES:
            svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="-6 -6 112 112" '
                   'width="100" height="100" fill="#000">' + body + "</svg>")
            imgs.append((px, render(svg, px)))
        entries.append(("suit_" + name, imgs))

    font = ImageFont.truetype(FONT, RANK_PX)
    glyphs, top, bot = [], 10 ** 9, 0
    for r in RANKS:
        img = Image.new("L", (60, 60), 255)
        ImageDraw.Draw(img).text((6, 6), r, font=font, fill=0)
        box = img.point(lambda v: 255 if v < THRESHOLD else 0).getbbox()
        glyphs.append((r, img, box))
        top, bot = min(top, box[1]), max(bot, box[3])
    # Todos los valores comparten el alto: así el de la esquina de abajo, girado
    # 180 grados, queda alineado con el de arriba.
    for r, img, box in glyphs:
        crop = img.crop((box[0], top, box[2], bot))
        entries.append(("rank_" + ("T" if r == "10" else r), [(crop.size[1], crop)]))

    emit(os.path.join(OUT, "cardIcons.h"),
         "// Palos y valores de las cartas del blackjack, a 1 bpp (freeink::Icon).\n"
         "// Los palos son SVG propios de relleno macizo (los de Lucide son de contorno y\n"
         "// rellenos quedan como manchas); los valores salen de DejaVu Sans Bold, para poder\n"
         "// girarlos 180 grados en la esquina de abajo (la fuente del aparato no se puede girar).\n"
         "// Generado por src/activities/games/gen_game_icons.py; no editar a mano.\n"
         "// Palos a " + ", ".join(str(s) for s in SUIT_SIZES) + " px; valores de alto uniforme.",
         entries)


if __name__ == "__main__":
    sys.exit(main())
