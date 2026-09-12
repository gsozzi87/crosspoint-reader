"""Tres temas visuales completos para el hub de la ws397."""
import sys
sys.path.insert(0, "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock")
from base import *

APPS = [
    ("Leer", "read"), ("Hablar", "ask"), ("Traductor", "translator"),
    ("Recordatorios", "reminders"), ("Tiempo", "timer"), ("Notas", "notes"),
    ("Biblia", "bible"), ("Música", "music"), ("Noticias", "news"),
    ("Fotos", "photos"), ("Apps", "games"), ("Clima", "weather"),
    ("Ajustes", "settings"),
]

REMS = [
    ("Dentista", "jue 10 sep 10:30", "Una sola vez", True),
    ("Sacar la basura", "lun 14 sep 21:00", "Los lunes y jueves", False),
    ("Mandar una factura", "mañana 06:00", "Una sola vez", False),
    ("Comprar regalo", "sin fecha", "", False),
]


# ═══════════════════════════════════════════════════════════════════════════
# TEMA 1 · DIARIO
#   Sin iconos y sin cuadrícula: el aparato como una hoja impresa. La jerarquía
#   la hace una serif con cuerpos muy distintos y reglas finas. Las aplicaciones
#   son un índice, no un mosaico.
# ═══════════════════════════════════════════════════════════════════════════
def diario_hub():
    img, d = canvas()
    M = 24
    CW = W - 2 * M

    # Cabecera: la fecha ES el título
    txt(d, M, 20, "viernes 11", SERIFB(46))
    spaced(d, M, 76, "de septiembre de 2026", SMALL, gap=2)
    txt(d, W - M, 26, "21:53", SERIFB(30), anchor="ra")
    battery(d, W - M - 62, 74, 73)
    txt(d, W - M - 68, 74, "73 %", SMALL, anchor="ra")
    rule(d, M, 104, CW, 3)
    rule(d, M, 110, CW, 1)

    # El clima como entradilla
    txt(d, M, 122, "17°", SERIFB(50))
    txt(d, M + 94, 136, "Nublado", SERIF(28))
    txt(d, W - M, 128, "Interior 29°", SMALLB, anchor="ra")
    txt(d, W - M, 150, "Humedad 50 %", SMALL, anchor="ra")
    txt(d, M, 182, "Iztacalco · máxima 24° · mínima 15° · humedad 85 %", SMALL)
    rule(d, M, 208, CW)

    # Lo que viene
    spaced(d, M, 220, "Lo que viene", SMALLB, gap=3)
    for i, (hora, titulo, pie) in enumerate([
        ("06:00", "Mandar una factura", "recordatorio · mañana"),
        ("09:30", "Escuela de traducción", "Francisca Serrano Ruiz"),
    ]):
        y = 244 + i * 58
        txt(d, M, y + 4, hora, SERIFB(24))
        txt(d, M + 84, y, titulo, SERIF(27))
        txt(d, M + 84, y + 32, pie, SMALL)
    rule(d, M, 362, CW)

    # Versículo
    spaced(d, M, 374, "Versículo del día", SMALLB, gap=3)
    txt(d, M, 396, "«Porque de tal manera amó Dios", SERIFI(22))
    txt(d, M, 422, "al mundo, que ha dado a su Hijo…»", SERIFI(22))
    txt(d, W - M, 400, "Juan 3:16", SMALLB, anchor="ra")
    rule(d, M, 456, CW, 2)

    # Índice de aplicaciones, dos columnas. "Mi día" no está: la parte de arriba
    # de esta misma hoja ES mi día.
    spaced(d, M, 468, "Aplicaciones", SMALLB, gap=3)
    top, rowh = 494, 30
    colw = (CW - 16) // 2
    for i, (name, _) in enumerate(APPS):
        col, row = i % 2, i // 2
        x = M + col * (colw + 16)
        yy = top + row * rowh
        if i == 1:  # el resalte del sistema: pestaña negra y regla, sin trama
            d.rectangle([x - 11, yy - 2, x - 6, yy + 24], fill=BLACK)
            rule(d, x, yy + 25, colw, 2)
        txt(d, x, yy + 3, f"{i + 1:02d}", SMALLB)
        nm = fit(d, name, SERIF(22), colw - 44)
        txt(d, x + 30, yy, nm, SERIF(22))
        leaders(d, x + 34 + int(tw(d, nm, SERIF(22))), x + colw - 4, yy + 15, 7)
    txt(d, M, 706, "Atrás dos veces: hablar · Atrás mantenido: sincronizar", SMALL)
    buttons(img, d)
    return finish(img)


def diario_list():
    img, d = canvas()
    M = 24
    CW = W - 2 * M
    txt(d, M, 22, "Recordatorios", SERIFB(38))
    txt(d, W - M, 40, "4 pendientes", SMALL, anchor="ra")
    rule(d, M, 78, CW, 3)
    rule(d, M, 84, CW, 1)

    y = 104
    grupos = [("Vencidos", REMS[:1]), ("Esta semana", REMS[1:3]), ("Sin fecha", REMS[3:])]
    for kicker, items in grupos:
        spaced(d, M, y, kicker, SMALLB, gap=3)
        y += 28
        for titulo, cuando, repite, vencido in items:
            sel = titulo == "Sacar la basura"
            if sel:
                d.rectangle([M - 10, y - 6, M - 5, y + 52], fill=BLACK)
            frame(d, [M + 2, y + 6, M + 22, y + 26])   # casilla
            txt(d, M + 36, y, titulo, SERIF(28))
            txt(d, M + 36, y + 34, cuando + ("  ·  " + repite if repite else ""), SMALL)
            if vencido:
                txt(d, W - M, y + 4, "vencido", SMALLB, anchor="ra")
            y += 68
            rule(d, M, y - 10, CW)
        y += 14
    txt(d, M, H - 104, "OK lo da por hecho · Atrás mantenido: opciones", SMALL)
    buttons(img, d, ("Atrás", "Bajar", "Hecho", "Nuevo"))
    return finish(img)


# ═══════════════════════════════════════════════════════════════════════════
# TEMA 2 · BENTO
#   Tarjetas con marco fino y esquinas redondeadas. Menos cosas, más grandes:
#   el clima ocupa una tarjeta entera y las aplicaciones bajan de trece a nueve
#   con una que dice "Más". Es el que más se parece a un reloj o a un panel.
# ═══════════════════════════════════════════════════════════════════════════
def bento_hub():
    img, d = canvas()
    M = 16

    # Barra de estado
    txt(d, M, 14, "21:53", UI10B)
    txt(d, M + 82, 17, "viernes 11 de septiembre", SMALL)
    battery(d, W - M - 30, 18, 73)
    txt(d, W - M - 40, 14, "73 %", SMALL, anchor="ra")

    # Tarjeta del clima
    card = [M, 44, W - M, 196]
    frame(d, card, r=14)
    txt(d, M + 16, 54, "CLIMA · IZTACALCO", SMALLB)
    rule(d, M + 1, 80, W - 2 * M - 1)
    txt(d, M + 16, 84, "17°", F("Ubuntu/Ubuntu-Bold.ttf", 62))
    txt(d, M + 128, 100, "Nublado", UI12)
    txt(d, M + 128, 134, "máx 24°  mín 15°", SMALL)
    icon(img, "hub_weather", W - M - 72, 88, 48)
    rule(d, W - M - 168, 92, 1, 84)   # separador vertical fino
    txt(d, W - M - 156, 96, "Interior", SMALL)
    txt(d, W - M - 156, 118, "29°", UI12B)
    txt(d, W - M - 156, 148, "Humedad 50 %", SMALL)

    # Dos tarjetas
    cw = (W - 2 * M - 12) // 2
    for i, (kick, ic, titulo, pie) in enumerate([
        ("PRÓXIMO", "hub_reminder", "Mandar una factura", "mañana · 06:00"),
        ("HOY", "hub_calendar", "Escuela de trad…", "Francisca Serrano"),
    ]):
        x = M + i * (cw + 12)
        frame(d, [x, 208, x + cw, 312], r=14)
        icon(img, ic, x + 14, 222, 24)
        txt(d, x + 46, 224, kick, SMALLB)
        txt(d, x + 14, 250, fit(d, titulo, UI10B, cw - 28), UI10B)
        txt(d, x + 14, 278, pie, SMALL)

    # Cuadrícula de nueve
    cols, rows = 3, 3
    gx, gy = 10, 10
    tw_ = (W - 2 * M - gx * (cols - 1)) // cols
    th = 108
    top = 328
    nine = APPS[:8] + [("Más", "settings")]
    for i, (name, ic) in enumerate(nine):
        c, r = i % cols, i // cols
        x = M + c * (tw_ + gx)
        y = top + r * (th + gy)
        sel = i == 1
        frame(d, [x, y, x + tw_, y + th], r=12, weight=2 if sel else 1)
        if sel:
            dither(img, (x + 2, y + 2, x + tw_ - 1, y + 60), 3)
        icon(img, f"hub_{ic}", x + (tw_ - 48) // 2, y + 12, 48)
        txt(d, x + tw_ // 2, y + 84, name, SMALLB if sel else SMALL, anchor="ma")
    txt(d, W // 2, H - 96, "Atrás dos veces: hablar · Atrás mantenido: sincronizar",
        SMALL, anchor="ma")
    buttons(img, d)
    return finish(img)


def bento_list():
    img, d = canvas()
    M = 16
    txt(d, M, 16, "Recordatorios", UI14)
    txt(d, W - M, 24, "4 pendientes", SMALL, anchor="ra")
    y = 68
    for titulo, cuando, repite, vencido in REMS:
        sel = titulo == "Sacar la basura"
        box = [M, y, W - M, y + 96]
        frame(d, box, r=14, weight=2 if sel else 1)
        if sel:
            dither(img, (M + 2, y + 2, M + 26, y + 94), 3)
        # casilla redonda
        d.ellipse([M + 40, y + 34, M + 68, y + 62], outline=BLACK, width=2)
        txt(d, M + 88, y + 22, titulo, UI12B)
        txt(d, M + 88, y + 54, cuando, SMALL)
        if repite:
            txt(d, M + 88 + 12 + tw(d, cuando, SMALL), y + 54, "· " + repite, SMALL)
        if vencido:
            frame(d, [W - M - 96, y + 20, W - M - 16, y + 46], r=12)
            txt(d, W - M - 56, y + 33, "vencido", SMALL, anchor="mm")
        y += 108
    frame(d, [M, y, W - M, y + 72], r=14)
    dither(img, (M + 2, y + 2, W - M - 1, y + 70), 4)
    d.rectangle([M + 40, y + 20, W - M - 40, y + 52], fill=WHITE)  # plato blanco
    txt(d, W // 2, y + 36, "+  Dictar uno nuevo", UI10B, anchor="mm")
    txt(d, W // 2, H - 96, "OK lo da por hecho · Atrás mantenido: más opciones",
        SMALL, anchor="ma")
    buttons(img, d, ("Atrás", "Bajar", "Hecho", "Nuevo"))
    return finish(img)


# ═══════════════════════════════════════════════════════════════════════════
# TEMA 3 · ESTACIÓN
#   Tablero de horarios: reloj gigante de siete segmentos (el que ya dibuja el
#   aparato), columnas alineadas, mayúsculas y puntos guía. Se lee de lejos y
#   apoyado en la mesa, que es como este aparato pasa la mayor parte del día.
# ═══════════════════════════════════════════════════════════════════════════
def estacion_hub():
    img, d = canvas()
    M = 20

    seven(d, M, 18, "21:53", h=78, t=9)
    txt(d, W - M, 22, "VIE 11 SEP", UI10B, anchor="ra")
    battery(d, W - M - 30, 54, 73)
    txt(d, W - M - 40, 50, "73 %", SMALL, anchor="ra")
    rule(d, M, 112, W - 2 * M, 4)

    # Renglón del clima, con el mismo formato de columnas que el resto
    txt(d, M, 126, "AHORA", SMALLB)
    txt(d, M + 92, 120, "17°  NUBLADO", UI12B)
    leaders(d, M + 92 + int(tw(d, "17°  NUBLADO", UI12B)) + 12, W - M - 132, 136)
    txt(d, W - M, 122, "INTERIOR 29°", SMALL, anchor="ra")
    rule(d, M, 158, W - 2 * M)

    # Tablero
    y = 176
    filas = [
        ("06:00", "MANDAR UNA FACTURA", "MAÑANA", True),
        ("09:30", "ESCUELA DE TRADUCCIÓN", "F. SERRANO", False),
        ("21:00", "SACAR LA BASURA", "LUN Y JUE", False),
        ("--:--", "COMPRAR REGALO", "SIN FECHA", False),
    ]
    for hora, titulo, nota, sel in filas:
        if sel:
            d.rectangle([M - 12, y - 5, M - 6, y + 27], fill=BLACK)
        f = UI10B if sel else UI10
        txt(d, M, y, hora, UI10B)
        notaw = int(tw(d, nota, SMALLB))
        tmax = (W - M - notaw - 12) - (M + 84)
        titulo = fit(d, titulo, f, tmax)
        txt(d, M + 84, y, titulo, f)
        leaders(d, M + 88 + int(tw(d, titulo, f)), W - M - notaw - 10, y + 14)
        txt(d, W - M, y + 2, nota, SMALLB if sel else SMALL, anchor="ra")
        y += 40
    rule(d, M, y + 6, W - 2 * M, 2)

    # Segundo tablero: las aplicaciones como andenes
    y += 22
    txt(d, M, y, "APLICACIONES", SMALLB)
    y += 30
    cols = 3
    cw = (W - 2 * M) // cols
    for i, (name, ic) in enumerate(APPS):
        c, r = i % cols, i // cols
        x = M + c * cw
        yy = y + r * 52
        d.rectangle([x, yy, x + 26, yy + 26], outline=BLACK, width=1)
        txt(d, x + 13, yy + 13, f"{i + 1}", SMALLB, anchor="mm")
        txt(d, x + 34, yy + 2, fit(d, name, SMALL, cw - 42), SMALL)
    rule(d, M, H - 116, W - 2 * M)
    txt(d, M, H - 106, "ATRÁS ×2: HABLAR · ATRÁS MANTENIDO: SINCRONIZAR", SMALL)
    buttons(img, d)
    return finish(img)


def estacion_list():
    img, d = canvas()
    M = 20
    txt(d, M, 18, "RECORDATORIOS", UI14)
    txt(d, W - M, 26, "4 PENDIENTES", SMALL, anchor="ra")
    rule(d, M, 62, W - 2 * M, 4)

    y = 82
    for kicker, items in [("VENCIDOS", REMS[:1]), ("ESTA SEMANA", REMS[1:3]), ("SIN FECHA", REMS[3:])]:
        txt(d, M, y, kicker, SMALLB)
        rule(d, M + int(tw(d, kicker, SMALLB)) + 12, y + 9, W - M - (M + int(tw(d, kicker, SMALLB)) + 12))
        y += 30
        for titulo, cuando, repite, vencido in items:
            sel = titulo == "Sacar la basura"
            if sel:
                d.rectangle([M - 12, y - 4, M - 6, y + 46], fill=BLACK)
            frame(d, [M, y + 6, M + 22, y + 28], weight=2 if sel else 1)
            txt(d, M + 40, y, titulo.upper(), UI12B if sel else UI12)
            txt(d, M + 40, y + 30, (cuando + ("  ·  " + repite if repite else "")).upper(), SMALL)
            if vencido:
                txt(d, W - M, y + 4, "VENCIDO", SMALLB, anchor="ra")
            y += 66
        y += 10
    rule(d, M, H - 116, W - 2 * M)
    txt(d, M, H - 106, "OK LO DA POR HECHO · ATRÁS MANTENIDO: OPCIONES", SMALL)
    buttons(img, d, ("Atrás", "Bajar", "Hecho", "Nuevo"))
    return finish(img)


# ═══════════════════════════════════════════════════════════════════════════
def sheet(name, left, right, titulo, bajada):
    """Las dos pantallas de un tema, una al lado de la otra, con su rótulo."""
    pad, gap, top = 40, 36, 128
    sw = W
    out = Image.new("L", (pad * 2 + sw * 2 + gap, top + H + 96), WHITE)
    d = ImageDraw.Draw(out)
    txt(d, pad, 34, titulo, F("Ubuntu/Ubuntu-Bold.ttf", 40))
    txt(d, pad, 86, bajada, F("Ubuntu/Ubuntu-Regular.ttf", 24))
    for i, (im, cap) in enumerate([(left, "Hub"), (right, "Recordatorios")]):
        x = pad + i * (sw + gap)
        out.paste(im.convert("L"), (x, top))
        d.rectangle([x - 1, top - 1, x + sw, top + H], outline=0, width=2)
        txt(d, x, top + H + 16, cap, F("NotoSans/NotoSans-Bold.ttf", 22))
    return out


if __name__ == "__main__":
    OUT = "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock"
    sheet("diario", diario_hub(), diario_list(), "Tema 1 · Diario",
          "Sin iconos y sin cuadrícula: serif, reglas finas y las aplicaciones como índice.").save(f"{OUT}/tema1-diario.png")
    sheet("bento", bento_hub(), bento_list(), "Tema 2 · Bento",
          "Tarjetas con marco fino: menos cosas y más grandes, el clima ocupa una entera.").save(f"{OUT}/tema2-bento.png")
    sheet("estacion", estacion_hub(), estacion_list(), "Tema 3 · Estación",
          "Tablero de horarios: reloj de siete segmentos, columnas alineadas y puntos guía.").save(f"{OUT}/tema3-estacion.png")
    print("listo")
