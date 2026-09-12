"""Maquetas del hub ws397 a tamaño real (480x800), con los iconos y las fuentes
del propio repo. Todo sale en 1 bit, como el panel."""
import re
from PIL import Image, ImageDraw, ImageFont

REPO = "/home/user/crosspoint-reader"
W, H = 480, 800
BLACK, WHITE = 0, 255

# ── Fuentes reales del aparato ──────────────────────────────────────────────
# Los tamanos salen de la altura de mayuscula que declara DISENO.md: UI_14 = 20 px
# de mayuscula, UI_12 = 17, UI_10 = 15, SMALL = 12. Con eso las maquetas dicen la
# verdad sobre cuanto texto entra en cada renglon.
F = lambda p, s: ImageFont.truetype(f"{REPO}/lib/EpdFont/builtinFonts/source/{p}", s)
# fontconvert.py: ppem = size * 150 / 72. Ubuntu 14 -> 29,2 px; NotoSerif 18 ->
# 37,5 px, que es la cara MAS GRANDE que hay compilada. Nada de lo que se dibuje
# acá puede pasarse de ahí sin agregar flash.
SERIF = lambda s: F("NotoSerif/NotoSerif-Regular.ttf", s)
SERIFB = lambda s: F("NotoSerif/NotoSerif-Bold.ttf", s)
SERIFI = lambda s: F("NotoSerif/NotoSerif-Italic.ttf", s)
PPEM = lambda size: round(size * 150 / 72)
SERIF12, SERIF14, SERIF16, SERIF18 = (SERIF(PPEM(n)) for n in (12, 14, 16, 18))
SERIF12B, SERIF14B, SERIF16B, SERIF18B = (SERIFB(PPEM(n)) for n in (12, 14, 16, 18))
SERIF14I, SERIF16I = (SERIFI(PPEM(n)) for n in (14, 16))
NS = lambda s: F("NotoSans/NotoSans-Regular.ttf", s)
NSB = lambda s: F("NotoSans/NotoSans-Bold.ttf", s)
NSI = lambda s: F("NotoSans/NotoSans-Italic.ttf", s)
NS12, NS14, NS16, NS18 = (NS(PPEM(n)) for n in (12, 14, 16, 18))
NS12B, NS14B, NS16B, NS18B = (NSB(PPEM(n)) for n in (12, 14, 16, 18))
NS16I = NSI(PPEM(16))

UI14 = F("Ubuntu/Ubuntu-Bold.ttf", 29)
UI14R = F("Ubuntu/Ubuntu-Regular.ttf", 29)
UI12 = F("Ubuntu/Ubuntu-Regular.ttf", 25)
UI12B = F("Ubuntu/Ubuntu-Bold.ttf", 25)
UI10 = F("Ubuntu/Ubuntu-Regular.ttf", 21)
UI10B = F("Ubuntu/Ubuntu-Bold.ttf", 21)
SMALL = F("NotoSans/NotoSans-Regular.ttf", 17)
SMALLB = F("NotoSans/NotoSans-Bold.ttf", 17)
SERIF = lambda s: F("NotoSerif/NotoSerif-Regular.ttf", s)
SERIFB = lambda s: F("NotoSerif/NotoSerif-Bold.ttf", s)
SERIFI = lambda s: F("NotoSerif/NotoSerif-Italic.ttf", s)


# ── Iconos: los mismos bitmaps que dibuja el firmware ───────────────────────
def load_icons(header):
    """hubIcons.h -> {nombre: Image('1')}. Bit 1 = transparente, 0 = negro."""
    src = open(f"{REPO}/src/components/icons/{header}").read()
    out = {}
    for name, size, body in re.findall(
        r"icon_(\w+?)_(\d+)_bits\[\] = \{([^}]*)\}", src):
        by = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        s = int(size)
        stride = (s + 7) // 8
        img = Image.new("1", (s, s), 1)
        px = img.load()
        for y in range(s):
            for x in range(s):
                b = by[y * stride + (x >> 3)]
                px[x, y] = (b >> (7 - (x & 7))) & 1
        out[f"{name}_{s}"] = img
    return out


ICONS = {}
ICONS.update(load_icons("hubIcons.h"))
ICONS.update(load_icons("hubWidgetIcons.h"))


def icon(img, name, x, y, size=48):
    """Pega un icono. Lo negro se pinta, lo blanco no toca el fondo.

    Los 24 px solo existen para los ocho del sumario (hubWidgetIcons.h); el
    resto se reduce desde el de 48 para la maqueta. En el aparato habria que
    generarlos con gen_icons.py, que es gratis salvo unos KB de flash.
    """
    key = f"{name}_{size}"
    if key not in ICONS:
        base = ICONS.get(f"{name}_48") or ICONS[f"{name}_64"]
        chico = base.convert("L").resize((size, size), Image.LANCZOS)
        ICONS[key] = chico.point(lambda v: 255 if v > 205 else 0).convert("1")
    img.paste(0, (x, y), ImageOps_invert(ICONS[key]))


def ImageOps_invert(ic):
    """Mascara: 1 donde hay que pintar negro."""
    return ic.point(lambda v: 0 if v else 255).convert("1")


# ── Lienzo ──────────────────────────────────────────────────────────────────
def canvas():
    img = Image.new("L", (W, H), WHITE)
    return img, ImageDraw.Draw(img)


def finish(img):
    """A 1 bit, como el panel: sin grises intermedios."""
    return img.point(lambda v: 255 if v > 140 else 0).convert("1")


# ── Primitivas ──────────────────────────────────────────────────────────────
def txt(d, x, y, s, font, anchor="la", fill=BLACK):
    d.text((x, y), s, font=font, fill=fill, anchor=anchor)


def tw(d, s, font):
    return d.textlength(s, font=font)


def rule(d, x, y, w, weight=1, fill=BLACK):
    d.rectangle([x, y, x + w - 1, y + weight - 1], fill=fill)


def spaced(d, x, y, s, font, gap=3, fill=BLACK, anchor_right=None):
    """Versalita espaciada: el recurso del tema Diario para los antetitulos."""
    s = s.upper()
    if anchor_right is not None:
        total = sum(d.textlength(c, font=font) + gap for c in s) - gap
        x = anchor_right - total
    for c in s:
        d.text((x, y), c, font=font, fill=fill)
        x += d.textlength(c, font=font) + gap
    return x


def dither(img, box, density=2):
    """Trama regular. density: 2 = 50 %, 3 = 33 %, 4 = 25 %."""
    x0, y0, x1, y1 = box
    px = img.load()
    for y in range(max(0, y0), min(H, y1)):
        for x in range(max(0, x0), min(W, x1)):
            if (x + y) % density == 0 if density == 2 else (x % density == 0 and y % density == 0):
                px[x, y] = BLACK


def frame(d, box, r=0, weight=1):
    if r:
        d.rounded_rectangle(box, radius=r, outline=BLACK, width=weight)
    else:
        d.rectangle(box, outline=BLACK, width=weight)


def leaders(d, x0, x1, y, step=6):
    """Puntos guia del tablero de estacion."""
    x = x0
    while x < x1:
        d.rectangle([x, y, x + 1, y + 1], fill=BLACK)
        x += step


# ── Siete segmentos (el mismo recurso que ya usa el aparato) ────────────────
SEG = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
}


def seven_width(s, h=70, t=None, gap=None):
    """Ancho exacto que va a ocupar seven(), para poder centrarlo."""
    t = t or max(4, h // 9)
    w = int(h * 0.56)
    gap = gap if gap is not None else t + 4
    total = 0
    for ch in s:
        total += (2 * (t // 2 + 1) if ch == ":" else w) + gap
    return total - gap


def seven(d, x, y, s, h=70, t=None, gap=None):
    """Digitos de 7 segmentos. Devuelve el ancho usado."""
    t = t or max(4, h // 9)
    w = int(h * 0.56)
    gap = gap if gap is not None else t + 4
    x0 = x
    for ch in s:
        if ch == ":":
            r = t // 2 + 1
            cy1, cy2 = y + h // 3, y + 2 * h // 3
            d.ellipse([x, cy1 - r, x + 2 * r, cy1 + r], fill=BLACK)
            d.ellipse([x, cy2 - r, x + 2 * r, cy2 + r], fill=BLACK)
            x += 2 * r + gap
            continue
        segs = SEG.get(ch, "")
        mid = y + h // 2
        if "a" in segs: d.rectangle([x + t, y, x + w - t, y + t], fill=BLACK)
        if "b" in segs: d.rectangle([x + w - t, y + t, x + w, mid - t // 2], fill=BLACK)
        if "c" in segs: d.rectangle([x + w - t, mid + t // 2, x + w, y + h - t], fill=BLACK)
        if "d" in segs: d.rectangle([x + t, y + h - t, x + w - t, y + h], fill=BLACK)
        if "e" in segs: d.rectangle([x, mid + t // 2, x + t, y + h - t], fill=BLACK)
        if "f" in segs: d.rectangle([x, y + t, x + t, mid - t // 2], fill=BLACK)
        if "g" in segs: d.rectangle([x + t, mid - t // 2, x + w - t, mid + t // 2], fill=BLACK)
        x += w + gap
    return x - x0


# ── Barra de botones, igual en los tres temas ──────────────────────────────
def buttons(img, d, labels=("Subir", "Bajar", "Selecc.", "Sinc.")):
    y = H - 52
    rule(d, 0, y - 10, W)
    cw = W // 4
    for i, lab in enumerate(labels):
        x = i * cw
        frame(d, [x + 6, y, x + cw - 8, y + 40], r=8)
        txt(d, x + cw // 2 - 1, y + 20, lab, SMALL, anchor="mm")


# ── Estado de la barra superior ────────────────────────────────────────────
def battery(d, x, y, pct=73, w=26, h=13):
    frame(d, [x, y, x + w, y + h])
    d.rectangle([x + w + 1, y + 4, x + w + 3, y + h - 4], fill=BLACK)
    d.rectangle([x + 2, y + 2, x + 2 + int((w - 4) * pct / 100), y + h - 2], fill=BLACK)


def fit(d, s, font, maxw, ell="…"):
    """Corta con puntos suspensivos, como truncatedText() del aparato."""
    if d.textlength(s, font=font) <= maxw:
        return s
    while s and d.textlength(s + ell, font=font) > maxw:
        s = s[:-1]
    return s.rstrip() + ell


def wrap(d, text, font, maxw):
    """Corta en palabras contra el ancho real, como el paginador del aparato."""
    out, linea = [], ""
    for palabra in text.split():
        probe = (linea + " " + palabra).strip()
        if d.textlength(probe, font=font) <= maxw:
            linea = probe
        else:
            if linea:
                out.append(linea)
            linea = palabra
    if linea:
        out.append(linea)
    return out
