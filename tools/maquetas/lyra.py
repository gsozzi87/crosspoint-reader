"""LYRA · sistema visual propuesto, y DIARIO v2 con las fuentes que ya están
compiladas. Todo a 480x800 y en 1 bit, con los iconos del firmware.

Nada acá usa un cuerpo que no exista: la cara más grande disponible es
NotoSerif/NotoSans 18, que a 150 dpi son 37,5 px de em y 27 px de mayúscula.
"""
import sys
sys.path.insert(0, "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock")
from base import *
from PIL import Image, ImageDraw

# ── Geometría de Lyra ──────────────────────────────────────────────────────
M = 16            # margen lateral
RAIL = 56         # la columna de estado: icono, casilla, hora o número
CX = 88           # donde empieza SIEMPRE el contenido
CW = W - CX - M   # 376
RIGHT = W - M

APPS = [
    ("Leer", "read"), ("Hablar", "ask"), ("Traductor", "translator"),
    ("Recordatorios", "reminders"), ("Tiempo", "timer"), ("Notas", "notes"),
    ("Biblia", "bible"), ("Música", "music"), ("Noticias", "news"),
    ("Fotos", "photos"), ("Apps", "games"), ("Clima", "weather"),
    ("Ajustes", "settings"),
]


def kicker(d, y, text, x=CX, upto=RIGHT):
    """Antetítulo con su regla: lo que separa un grupo del siguiente."""
    txt(d, x, y, text.upper(), SMALLB)
    x2 = x + int(tw(d, text.upper(), SMALLB)) + 10
    if x2 < upto:
        rule(d, x2, y + 9, upto - x2)


def selbar(d, y, h):
    """El resalte de Lyra: barra en el rail, fondo blanco. Nunca trama."""
    d.rectangle([M, y, M + 6, y + h], fill=BLACK)


def check(d, x, y, done=False, size=24):
    frame(d, [x, y, x + size, y + size], r=5, weight=2)
    if done:
        d.line([x + 6, y + size // 2, x + size // 2 - 1, y + size - 7], fill=BLACK, width=3)
        d.line([x + size // 2 - 1, y + size - 7, x + size - 5, y + 6], fill=BLACK, width=3)


def status(img, d, hora="21:53", fecha="viernes 11 de septiembre", wifi=True, pct=73):
    txt(d, M, 8, hora, UI14)
    txt(d, M + 92, 15, fecha, SMALL)
    battery(d, RIGHT - 30, 14, pct)
    txt(d, RIGHT - 40, 10, f"{pct} %", SMALL, anchor="ra")
    if wifi:
        for i in range(3):
            d.rectangle([RIGHT - 108 + i * 7, 24 - i * 5, RIGHT - 104 + i * 7, 26], fill=BLACK)
    rule(d, M, 46, W - 2 * M)


def hint(d, text, y=H - 92):
    txt(d, W // 2, y, text, SMALL, anchor="ma")


def row(d, y, titulo, meta="", derecha="", sel=False, h=52, font=None):
    if sel:
        selbar(d, y - 4, h)
    txt(d, CX, y, fit(d, titulo, font or (UI12B if sel else UI12), CW - (int(tw(d, derecha, SMALL)) + 16 if derecha else 0)),
        font or (UI12B if sel else UI12))
    if meta:
        txt(d, CX, y + 28, meta, SMALL)
    if derecha:
        txt(d, RIGHT, y + 4, derecha, SMALLB if sel else SMALL, anchor="ra")


# ═══════════════════════════════════════════════════════════════════════════
# LYRA
# ═══════════════════════════════════════════════════════════════════════════
def lyra_hub():
    img, d = canvas()
    status(img, d)

    # AHORA: lo único urgente, del tamaño que le corresponde
    kicker(d, 60, "Ahora")
    icon(img, "hub_reminders", 20, 78, 48)
    txt(d, CX, 84, fit(d, "Mandar una factura", NS16B, CW), NS16B)
    txt(d, CX, 124, "mañana 06:00 · una sola vez", UI10)
    rule(d, M, 158, W - 2 * M)

    # HOY: lo demás del día, compacto
    kicker(d, 168, "Hoy")
    filas = [
        ("hub_calendar", "Escuela de traducción", "Francisca Serrano Ruiz", "09:30"),
        ("hub_cart", "Compras: 6 cosas", "leche, pan, café…", ""),
        ("hub_tasks", "Tareas: 2 pendientes", "llamar al banco…", ""),
    ]
    y = 192
    for ic, titulo, meta, der in filas:
        icon(img, ic, 32, y + 6, 24)
        row(d, y, titulo, meta, der)
        y += 54
    rule(d, M, 356, W - 2 * M)

    # Clima y libro: dos renglones con el mismo ritmo
    icon(img, "hub_weather", 28, 374, 24)
    txt(d, CX, 368, "17° Nublado", UI14)
    txt(d, CX, 400, "Iztacalco · máx 24° mín 15° · interior 29°", SMALL)
    rule(d, M, 428, W - 2 * M)
    icon(img, "hub_read", 28, 444, 24)
    txt(d, CX, 438, "El nombre del viento", UI12)
    txt(d, CX, 466, "Patrick Rothfuss · 38 %", SMALL)
    rule(d, M, 494, W - 2 * M)

    # El dock: trece aplicaciones sin etiqueta; sólo la elegida se nombra
    top = 512
    elegida = None
    for fila, yy, n in ((APPS[:7], top, 7), (APPS[7:], top + 62, 6)):
        total = n * 48 + (n - 1) * 18
        x0 = (W - total) // 2
        for j, (name, ic) in enumerate(fila):
            x = x0 + j * 66
            icon(img, f"hub_{ic}", x, yy, 48)
            if name == "Hablar":
                d.rectangle([x - 4, yy + 52, x + 52, yy + 56], fill=BLACK)
                elegida = name
    if elegida:
        txt(d, W // 2, top + 124, elegida, SMALLB, anchor="ma")
    hint(d, "Atrás dos veces: hablar · Atrás mantenido: sincronizar", 676)
    buttons(img, d)
    return finish(img)


def lyra_reminders():
    img, d = canvas()
    txt(d, M, 14, "Recordatorios", UI14)
    txt(d, RIGHT, 22, "4 pendientes", SMALL, anchor="ra")
    rule(d, M, 54, W - 2 * M, 2)

    y = 72
    grupos = [
        ("Vencidos", [("Dentista", "jue 10 sep 10:30", "vencido", False)]),
        ("Esta semana", [("Sacar la basura", "lun 21:00 · los lunes y jueves", "", True),
                         ("Mandar una factura", "mañana 06:00", "", False)]),
        ("Sin fecha", [("Comprar regalo", "", "", False)]),
    ]
    for nombre, items in grupos:
        kicker(d, y, nombre)
        y += 26
        for titulo, meta, der, sel in items:
            icon_y = y + 4
            check(d, 32, icon_y, False)
            row(d, y, titulo, meta, der, sel, h=58)
            y += 62
        y += 10
    # La fila de alta, con el mismo rail
    icon(img, "hub_ask", 32, y + 4, 24)
    txt(d, CX, y, "Dictar uno nuevo", UI12)
    txt(d, CX, y + 28, "OK graba y el servidor entiende la fecha", SMALL)
    hint(d, "OK lo da por hecho · Atrás mantenido: opciones")
    buttons(img, d, ("Atrás", "Bajar", "Hecho", "Nuevo"))
    return finish(img)


def lyra_editor():
    img, d = canvas()
    txt(d, M, 14, "Sacar la basura", UI14)
    rule(d, M, 54, W - 2 * M, 2)
    campos = [
        ("Qué", "Sacar la basura", False),
        ("Día", "lunes 14 de septiembre", False),
        ("Hora", "21:00", True),
        ("Repetir", "Los lunes y jueves", False),
        ("Aviso", "Sonido y voz", False),
    ]
    y = 78
    for etiqueta, valor, sel in campos:
        if sel:
            selbar(d, y - 6, 56)
        txt(d, CX, y, etiqueta, SMALL)
        txt(d, CX, y + 20, valor, UI12B if sel else UI12)
        if sel:
            frame(d, [CX - 8, y + 16, CX + int(tw(d, valor, UI12B)) + 12, y + 48], r=6, weight=2)
            for k, dy in ((0, 0), (1, 18)):
                cx0 = RIGHT - 18
                pts = [(cx0 - 9, y + 30 + dy), (cx0 + 9, y + 30 + dy), (cx0, y + 30 + dy - 11 + k * 22)]
                d.polygon(pts, fill=BLACK)
        y += 66
    rule(d, M, y + 6, W - 2 * M)
    txt(d, CX, y + 22, "Va a sonar el próximo lunes a las 21:00", SMALL)
    hint(d, "La palanca cambia el valor · OK pasa al siguiente")
    buttons(img, d, ("Atrás", "Bajar", "Guardar", "Borrar"))
    return finish(img)


def lyra_notes():
    img, d = canvas()
    txt(d, M, 14, "Notas", UI14)
    txt(d, RIGHT, 22, "7 notas", SMALL, anchor="ra")
    rule(d, M, 54, W - 2 * M, 2)
    notas = [
        ("Idea para el libro", "Un detective que solo trabaja de noche", "sáb 12 sep", True),
        ("Lista de regalos", "Ana: bufanda. Pedro: los audífonos que…", "vie 11 sep", False),
        ("Receta de la abuela", "Tres tazas de harina, dos huevos, una…", "mié 9 sep", False),
        ("Contraseña del portón", "No la anoto acá, está en el llavero", "lun 7 sep", False),
    ]
    y = 74
    for titulo, cuerpo, cuando, sel in notas:
        icon(img, "hub_notes", 30, y + 4, 24)
        if sel:
            selbar(d, y - 6, 72)
        txt(d, CX, y, titulo, UI12B if sel else UI12)
        txt(d, CX, y + 28, fit(d, cuerpo, SMALL, CW - 90), SMALL)
        txt(d, RIGHT, y + 2, cuando, SMALL, anchor="ra")
        y += 80
        rule(d, CX, y - 14, RIGHT - CX)
    hint(d, "OK la abre · Atrás mantenido la borra")
    buttons(img, d, ("Atrás", "Bajar", "Abrir", "Nueva"))
    return finish(img)


def lyra_listening():
    img, d = canvas()
    # Pantalla modal: sin rail, todo centrado. Es el otro modo de Lyra.
    icon(img, "hub_ask", (W - 64) // 2, 108, 64)
    txt(d, W // 2, 196, "Escuchando…", NS18B, anchor="ma")
    # Medidor de nivel: barras reales del micrófono
    n, bw, bh = 21, 12, 64
    x0 = (W - (n * bw + (n - 1) * 6)) // 2
    niveles = [3, 7, 12, 22, 38, 55, 44, 30, 48, 62, 51, 36, 24, 41, 58, 39, 21, 13, 8, 5, 3]
    for i, lv in enumerate(niveles):
        h = max(3, int(bh * lv / 62))
        x = x0 + i * (bw + 6)
        d.rectangle([x, 300 + (bh - h), x + bw, 300 + bh], fill=BLACK)
    rule(d, 64, 384, W - 128)
    txt(d, W // 2, 400, "8 s restantes", SMALL, anchor="ma")

    kicker(d, 452, "Puedes decir", x=M, upto=RIGHT)
    ejemplos = [
        ("Recordatorio", "«recuérdame mandar la factura mañana a las 6»"),
        ("Compras", "«agrega leche y pan a la lista»"),
        ("Temporizador", "«pon diez minutos»"),
        ("Pregunta", "«¿cuánto mide la torre Eiffel?»"),
    ]
    y = 480
    for cat, frase in ejemplos:
        txt(d, M, y, cat, SMALLB)
        txt(d, M + 128, y, fit(d, frase, SMALL, W - 2 * M - 128), SMALL)
        y += 30
    hint(d, "OK termina · Sacudir cancela")
    buttons(img, d, ("Cancelar", "", "Terminar", ""))
    return finish(img)


def lyra_answer():
    img, d = canvas()
    txt(d, M, 14, "Pregunta", UI14)
    rule(d, M, 54, W - 2 * M, 2)
    txt(d, M, 70, "«¿Cuánto mide la torre Eiffel?»", NS16I)
    rule(d, M, 112, W - 2 * M)
    parrafos = [
        "La torre Eiffel mide 330 metros con las antenas y 300 hasta la punta de "
        "la estructura. Cuando se terminó, en 1889, era el edificio más alto del "
        "mundo, y lo fue durante cuarenta y un años.",
        "Pesa unas 10.100 toneladas y en verano se estira hasta 15 centímetros "
        "por el calor.",
    ]
    y = 132
    for parrafo in parrafos:
        for linea in wrap(d, parrafo, NS14, W - 2 * M):
            txt(d, M, y, linea, NS14)
            y += 40
        y += 16
    rule(d, M, 560, W - 2 * M)
    icon(img, "hub_ask", M, 578, 24)
    txt(d, M + 36, 574, "Leyendo la respuesta en voz alta", SMALL)
    hint(d, "Atrás vuelve · OK corta la lectura")
    buttons(img, d, ("Atrás", "Bajar", "Callar", ""))
    return finish(img)


def lyra_timer():
    img, d = canvas()
    txt(d, M, 14, "Tiempo", UI14)
    rule(d, M, 54, W - 2 * M, 2)
    # Pestañas
    for i, t in enumerate(("Temporizador", "Cronómetro", "Pomodoro")):
        x = M + i * 150
        if i == 0:
            rule(d, x, 104, 140, 3)
        txt(d, x + 70, 74, t, SMALLB if i == 0 else SMALL, anchor="ma")
    seven(d, (W - seven_width("07:42", 96, 11)) // 2, 150, "07:42", h=96, t=11)
    txt(d, W // 2, 268, "de 10 minutos", SMALL, anchor="ma")
    # Barra de avance
    frame(d, [M + 24, 300, RIGHT - 24, 324], r=6)
    d.rectangle([M + 28, 304, M + 28 + int((RIGHT - M - 56) * 0.23), 320], fill=BLACK)
    rule(d, M, 356, W - 2 * M)
    kicker(d, 372, "Después", x=M)
    txt(d, M, 396, "Suena el pitido y lo dice en voz alta", SMALL)
    txt(d, M, 424, "Sigue corriendo si sales de esta pantalla", SMALL)
    hint(d, "OK pausa · Atrás sale y sigue · Atrás mantenido cancela")
    buttons(img, d, ("Atrás", "Bajar", "Pausar", "Reiniciar"))
    return finish(img)


def lyra_alarm():
    img, d = canvas()
    icon(img, "hub_reminder", (W - 48) // 2, 96, 24)
    seven(d, (W - seven_width("06:00", 110, 13)) // 2, 150, "06:00", h=110, t=13)
    txt(d, W // 2, 290, "Mandar una factura", NS18B, anchor="ma")
    txt(d, W // 2, 336, "recordatorio de hoy", SMALL, anchor="ma")
    rule(d, 96, 380, W - 192, 2)
    txt(d, W // 2, 400, "Se repite: una sola vez", SMALL, anchor="ma")
    hint(d, "Boca abajo lo calla · Sacudir lo descarta", 600)
    buttons(img, d, ("10 min más", "", "Hecho", ""))
    return finish(img)


def lyra_music():
    img, d = canvas()
    txt(d, M, 14, "Música", UI14)
    txt(d, RIGHT, 22, "3 / 14", SMALL, anchor="ra")
    rule(d, M, 54, W - 2 * M, 2)

    seven(d, M, 74, "1:47", h=64, t=8)
    txt(d, RIGHT, 78, "4:12", UI10, anchor="ra")
    txt(d, RIGHT, 106, "192 kbps · 44 kHz", SMALL, anchor="ra")
    txt(d, M, 152, fit(d, "Bohemian Rhapsody", NS16B, W - 2 * M), NS16B)
    txt(d, M, 190, "Queen · A Night at the Opera", SMALL)
    # Barra de posición con cursor
    frame(d, [M, 222, RIGHT, 242], r=4)
    p = M + int((W - 2 * M) * 0.42)
    d.rectangle([M + 2, 226, p, 238], fill=BLACK)
    d.rectangle([p - 2, 216, p + 2, 248], fill=BLACK)
    # Analizador: picos reales de cada bloque
    niveles = [18, 31, 44, 52, 38, 26, 41, 55, 47, 33, 22, 36, 49, 58, 44, 29, 19, 27, 39, 51, 42, 30, 21, 15]
    bw = (W - 2 * M - 23 * 3) // 24
    for i, lv in enumerate(niveles):
        h = max(2, int(58 * lv / 58))
        x = M + i * (bw + 3)
        d.rectangle([x, 330 - h, x + bw, 330], fill=BLACK)
    rule(d, M, 348, W - 2 * M)

    kicker(d, 362, "Esta carpeta")
    pistas = [("1", "Death on Two Legs", "3:43"), ("2", "Lazing on a Sunday", "1:08"),
              ("3", "Bohemian Rhapsody", "5:55"), ("4", "Love of My Life", "3:38")]
    y = 388
    for n, titulo, dur in pistas:
        sel = n == "3"
        if sel:
            selbar(d, y - 4, 44)
        txt(d, 40, y + 2, n, SMALLB if sel else SMALL)
        txt(d, CX, y, fit(d, titulo, UI12B if sel else UI12, CW - 60), UI12B if sel else UI12)
        txt(d, RIGHT, y + 4, dur, SMALL, anchor="ra")
        y += 48
    hint(d, "OK pausa · Atrás sale y sigue sonando")
    buttons(img, d, ("Atrás", "Bajar", "Pausa", "Volumen"))
    return finish(img)


def lyra_weather():
    img, d = canvas()
    txt(d, M, 14, "Clima", UI14)
    txt(d, RIGHT, 22, "Iztacalco", SMALL, anchor="ra")
    rule(d, M, 54, W - 2 * M, 2)

    icon(img, "hub_weather", M, 76, 48)
    txt(d, M + 68, 72, "17°", NS18B)
    txt(d, M + 140, 80, "Nublado", UI12)
    txt(d, M + 140, 108, "se siente 16°", SMALL)
    txt(d, RIGHT, 74, "máx 24°", SMALL, anchor="ra")
    txt(d, RIGHT, 96, "mín 15°", SMALL, anchor="ra")
    txt(d, RIGHT, 118, "hum 85 %", SMALL, anchor="ra")
    rule(d, M, 146, W - 2 * M)

    kicker(d, 158, "Próximas horas", x=M)
    horas = [("23", "16°", 10), ("01", "15°", 20), ("03", "15°", 35),
             ("05", "14°", 45), ("07", "16°", 20), ("09", "20°", 5)]
    colw = (W - 2 * M) // 6
    for i, (h, t, pp) in enumerate(horas):
        x = M + i * colw + colw // 2
        txt(d, x, 186, h, SMALL, anchor="ma")
        txt(d, x, 208, t, UI10B, anchor="ma")
        alto = max(2, int(pp * 0.62))
        d.rectangle([x - 9, 268 - alto, x + 9, 268], fill=BLACK)
        txt(d, x, 274, f"{pp}%", SMALL, anchor="ma")
    rule(d, M + 8, 269, W - 2 * M - 16)   # la base del gráfico de lluvia
    rule(d, M, 302, W - 2 * M)

    kicker(d, 314, "Seis días", x=M)
    dias = [("sáb", 15, 25), ("dom", 14, 23), ("lun", 13, 21), ("mar", 15, 24), ("mié", 16, 26)]
    y = 344
    for nombre, mn, mx in dias:
        txt(d, M, y + 2, nombre, UI10)
        txt(d, M + 72, y + 4, f"{mn}°", SMALL)
        x0, x1 = M + 108, RIGHT - 52
        rule(d, x0, y + 14, x1 - x0)
        a = x0 + int((x1 - x0) * (mn - 12) / 16)
        b = x0 + int((x1 - x0) * (mx - 12) / 16)
        d.rectangle([a, y + 8, b, y + 20], fill=BLACK)
        txt(d, RIGHT, y + 4, f"{mx}°", SMALLB, anchor="ra")
        y += 48
    hint(d, "Atrás mantenido: actualizar ahora")
    buttons(img, d, ("Atrás", "Bajar", "", "Actualizar"))
    return finish(img)


def lyra_news():
    img, d = canvas()
    txt(d, M, 14, "Noticias", UI14)
    txt(d, RIGHT, 22, "3 feeds", SMALL, anchor="ra")
    rule(d, M, 54, W - 2 * M, 2)
    feeds = [
        ("El País", [("La reforma llega al Congreso con los votos justos", "hace 2 h", True),
                     ("Las claves del acuerdo que nadie quiso firmar", "hace 5 h", False)]),
        ("BBC Mundo", [("Qué se sabe del hallazgo en el desierto", "hace 1 h", False),
                       ("El país que apagó internet por un examen", "ayer", False)]),
        ("Xataka", [("Por qué la tinta electrónica no despega", "hace 8 h", False)]),
    ]
    y = 72
    for nombre, items in feeds:
        kicker(d, y, nombre)
        y += 28
        for titular, cuando, sel in items:
            if sel:
                selbar(d, y - 6, 78)
            icon(img, "hub_news", 32, y + 6, 24)
            f = UI12B if sel else UI12
            lineas = wrap(d, titular, f, CW)[:2]
            for k, linea in enumerate(lineas):
                txt(d, CX, y + k * 28, fit(d, linea, f, CW), f)
            txt(d, CX, y + 56, cuando, SMALL)
            y += 86
        y += 8
    hint(d, "OK abre la nota · Atrás mantenido: actualizar")
    buttons(img, d, ("Atrás", "Bajar", "Abrir", "Actualizar"))
    return finish(img)


def lyra_settings():
    img, d = canvas()
    txt(d, M, 14, "Ajustes", UI14)
    rule(d, M, 54, W - 2 * M, 2)
    grupos = [
        ("Aparato", [("Idioma", "Español", False), ("Voz hablada", "Respuestas cortas", False),
                     ("Sonidos", "Normales", True), ("Volumen", "70 %", False)]),
        ("Sistema", [("Modo de energía", "Ahorro", False), ("Movimiento", "Calibrado", False),
                     ("Memoria", "Heap 112 KB", False), ("Vincular con mi cuenta", "", False)]),
    ]
    y = 72
    for nombre, filas in grupos:
        kicker(d, y, nombre)
        y += 28
        for etiqueta, valor, sel in filas:
            if sel:
                selbar(d, y - 6, 46)
            txt(d, CX, y, etiqueta, UI12B if sel else UI12)
            if valor:
                txt(d, RIGHT, y + 4, valor, SMALLB if sel else SMALL, anchor="ra")
            y += 52
        y += 12
    hint(d, "La palanca cambia el valor de la fila elegida")
    buttons(img, d, ("Atrás", "Bajar", "Abrir", ""))
    return finish(img)


def lyra_sleep():
    """La pantalla que queda puesta al suspender. Es la que más se mira."""
    img, d = canvas()
    seven(d, (W - seven_width("21:53", 104, 12)) // 2, 104, "21:53", h=104, t=12)
    txt(d, W // 2, 232, "viernes 11 de septiembre", UI12, anchor="ma")
    rule(d, 56, 276, W - 112, 2)

    kicker(d, 300, "Mañana", x=56, upto=W - 56)
    filas = [("06:00", "Mandar una factura"), ("09:30", "Escuela de traducción"),
             ("21:00", "Sacar la basura")]
    y = 332
    for hora, titulo in filas:
        txt(d, 56, y, hora, UI12B)
        txt(d, 146, y, fit(d, titulo, UI12, W - 146 - 56), UI12)
        y += 44
    rule(d, 56, y + 6, W - 112)
    icon(img, "hub_cart", 56, y + 24, 24)
    txt(d, 92, y + 22, "6 compras · 2 tareas pendientes", SMALL)
    icon(img, "hub_weather", 56, y + 62, 24)
    txt(d, 92, y + 60, "17° nublado · máx 24° mín 15°", SMALL)

    battery(d, W // 2 - 42, H - 108, 73)
    txt(d, W // 2 + 10, H - 112, "73 %", SMALL)
    txt(d, W // 2, H - 72, "PWR para volver", SMALL, anchor="ma")
    return finish(img)


# ═══════════════════════════════════════════════════════════════════════════
# DIARIO v2 · sólo con las caras compiladas (serif 12/14/16/18)
# ═══════════════════════════════════════════════════════════════════════════
DM = 24
DCW = W - 2 * DM


def d_masthead(d, titulo, derecha="", sub=""):
    txt(d, DM, 16, titulo, SERIF18B)
    if sub:
        spaced(d, DM, 60, sub, SMALL, gap=2)
    if derecha:
        txt(d, RIGHT, 24, derecha, SERIF16B, anchor="ra")
    rule(d, DM, 88, DCW, 3)
    rule(d, DM, 94, DCW, 1)


def diario2_hub():
    img, d = canvas()
    d_masthead(d, "viernes 11", "21:53", "de septiembre de 2026")
    battery(d, RIGHT - 30, 62, 73)
    txt(d, RIGHT - 40, 58, "73 %", SMALL, anchor="ra")

    # Entradilla: el clima con la serif más grande que existe
    txt(d, DM, 106, "17°", SERIF18B)
    txt(d, DM + 78, 114, "Nublado", SERIF16)
    txt(d, RIGHT, 110, "Interior 29° · 50 %", SMALLB, anchor="ra")
    txt(d, DM, 152, "Iztacalco · máxima 24° · mínima 15° · humedad 85 %", SMALL)
    rule(d, DM, 182, DCW)

    spaced(d, DM, 194, "Lo que viene", SMALLB, gap=3)
    for i, (hora, titulo, pie) in enumerate([
        ("06:00", "Mandar una factura", "recordatorio · una sola vez"),
        ("09:30", "Escuela de traducción", "Francisca Serrano Ruiz"),
        ("21:00", "Sacar la basura", "los lunes y jueves"),
    ]):
        y = 220 + i * 64
        txt(d, DM, y + 4, hora, SERIF14B)
        txt(d, DM + 84, y, titulo, SERIF16)
        txt(d, DM + 84, y + 36, pie, SMALL)
    rule(d, DM, 414, DCW, 2)

    spaced(d, DM, 426, "Aplicaciones", SMALLB, gap=3)
    top, rowh = 452, 30
    colw = (DCW - 16) // 2
    for i, (name, _) in enumerate(APPS):
        col, r = i % 2, i // 2
        x = DM + col * (colw + 16)
        yy = top + r * rowh
        if i == 1:
            d.rectangle([x - 11, yy - 2, x - 6, yy + 24], fill=BLACK)
            rule(d, x, yy + 25, colw, 2)
        txt(d, x, yy + 3, f"{i + 1:02d}", SMALLB)
        nm = fit(d, name, SERIF12, colw - 38)
        txt(d, x + 26, yy, nm, SERIF12)
        leaders(d, x + 30 + int(tw(d, nm, SERIF12)), x + colw - 4, yy + 15, 7)
    rule(d, DM, 676, DCW)
    txt(d, DM, 692, "Atrás dos veces: hablar · Atrás mantenido: sincronizar", SMALL)
    buttons(img, d)
    return finish(img)


def diario2_reminders():
    img, d = canvas()
    d_masthead(d, "Recordatorios", "4")
    y = 110
    grupos = [("Vencidos", [("Dentista", "jueves 10 de septiembre, 10:30", "vencido", False)]),
              ("Esta semana", [("Sacar la basura", "lunes 21:00 · los lunes y jueves", "", True),
                               ("Mandar una factura", "mañana 06:00", "", False)]),
              ("Sin fecha", [("Comprar regalo", "", "", False)])]
    for nombre, items in grupos:
        spaced(d, DM, y, nombre, SMALLB, gap=3)
        y += 26
        for titulo, meta, der, sel in items:
            if sel:
                d.rectangle([DM - 10, y - 4, DM - 5, y + 48], fill=BLACK)
            frame(d, [DM, y + 4, DM + 20, y + 24])
            txt(d, DM + 36, y - 4, titulo, SERIF16)
            if meta:
                txt(d, DM + 36, y + 26, meta, SMALL)
            if der:
                txt(d, RIGHT, y, der, SMALLB, anchor="ra")
            y += 62
            rule(d, DM, y - 12, DCW)
        y += 12
    spaced(d, DM, y, "Agregar", SMALLB, gap=3)
    txt(d, DM, y + 24, "Dictar uno nuevo", SERIF14)
    leaders(d, DM + 8 + int(tw(d, "Dictar uno nuevo", SERIF14)), RIGHT, y + 38, 7)
    txt(d, DM, H - 104, "OK lo da por hecho · Atrás mantenido: opciones", SMALL)
    buttons(img, d, ("Atrás", "Bajar", "Hecho", "Nuevo"))
    return finish(img)


def diario2_reader():
    img, d = canvas()
    txt(d, DM, 14, "Idea para el libro", SERIF16B)
    txt(d, RIGHT, 20, "sáb 12 sep", SMALL, anchor="ra")
    rule(d, DM, 54, DCW)
    parrafos = [
        "Un detective que solo trabaja de noche y odia el café. Vive en una "
        "ciudad donde nunca deja de llover y todos los casos le llegan por "
        "carta, nunca por teléfono.",
        "La primera escena: una carta sin remitente debajo de la puerta, "
        "mojada, con una dirección que no existe en ningún mapa.",
        "Se llama Mateo y tiene un perro viejo que se llama Domingo, porque lo "
        "encontró un domingo y no le pareció necesario buscar otro nombre.",
    ]
    y = 76
    cortado = False
    for parrafo in parrafos:
        for linea in wrap(d, parrafo, SERIF16, DCW):
            if y > 556:       # lo que no entra pasa a la página siguiente
                cortado = True
                break
            txt(d, DM, y, linea, SERIF16)
            y += 40
        if cortado:
            break
        y += 14
    rule(d, DM, 616, DCW)
    spaced(d, DM, 630, "Página 1 de 3", SMALL, gap=2)
    txt(d, DM, H - 104, "La palanca pasa la página", SMALL)
    buttons(img, d, ("Atrás", "Bajar", "", ""))
    return finish(img)


def diario2_music():
    img, d = canvas()
    d_masthead(d, "Música", "3 / 14")
    txt(d, DM, 108, "Bohemian Rhapsody", SERIF16B)
    txt(d, DM, 148, "Queen · A Night at the Opera", SMALL)
    txt(d, RIGHT, 176, "1:47 de 4:12", SMALL, anchor="ra")
    frame(d, [DM, 172, RIGHT - 130, 190], r=3)
    d.rectangle([DM + 2, 176, DM + int((RIGHT - 130 - DM) * 0.42), 186], fill=BLACK)
    rule(d, DM, 212, DCW)
    spaced(d, DM, 224, "Esta carpeta", SMALLB, gap=3)
    y = 250
    for n, titulo, dur in [("1", "Death on Two Legs", "3:43"), ("2", "Lazing on a Sunday", "1:08"),
                           ("3", "Bohemian Rhapsody", "5:55"), ("4", "Love of My Life", "3:38"),
                           ("5", "Good Company", "3:26")]:
        sel = n == "3"
        if sel:
            d.rectangle([DM - 10, y - 4, DM - 5, y + 30], fill=BLACK)
        txt(d, DM, y + 2, n, SMALLB)
        txt(d, DM + 26, y - 2, titulo, SERIF14)
        leaders(d, DM + 32 + int(tw(d, titulo, SERIF14)), RIGHT - 44, y + 14, 7)
        txt(d, RIGHT, y, dur, SMALL, anchor="ra")
        y += 40
    rule(d, DM, y + 6, DCW)
    txt(d, DM, H - 104, "OK pausa · Atrás sale y sigue sonando", SMALL)
    buttons(img, d, ("Atrás", "Bajar", "Pausa", "Volumen"))
    return finish(img)


def diario2_sleep():
    img, d = canvas()
    txt(d, DM, 40, "viernes 11", SERIF18B)
    spaced(d, DM, 88, "de septiembre de 2026", SMALL, gap=2)
    txt(d, RIGHT, 48, "21:53", SERIF18B, anchor="ra")
    rule(d, DM, 120, DCW, 3)
    rule(d, DM, 126, DCW, 1)

    spaced(d, DM, 148, "Mañana", SMALLB, gap=3)
    y = 176
    for hora, titulo in [("06:00", "Mandar una factura"), ("09:30", "Escuela de traducción"),
                         ("21:00", "Sacar la basura")]:
        txt(d, DM, y + 2, hora, SERIF14B)
        txt(d, DM + 80, y, titulo, SERIF16)
        y += 44
    rule(d, DM, y + 8, DCW)
    txt(d, DM, y + 26, "6 compras · 2 tareas pendientes", SMALL)
    txt(d, DM, y + 54, "17° nublado · máxima 24° · mínima 15°", SMALL)
    rule(d, DM, y + 92, DCW)
    for k, linea in enumerate(wrap(d, "«Porque de tal manera amó Dios al mundo, que ha dado a su Hijo…»", SERIF14I, DCW)):
        txt(d, DM, y + 108 + k * 34, linea, SERIF14I)
    battery(d, DM, H - 100, 73)
    txt(d, DM + 44, H - 104, "73 %", SMALL)
    txt(d, RIGHT, H - 104, "PWR para volver", SMALL, anchor="ra")
    return finish(img)


# ═══════════════════════════════════════════════════════════════════════════
def sheet(titulo, bajada, pantallas, cols=4):
    pad, gap, top, cap = 40, 30, 140, 54
    filas = (len(pantallas) + cols - 1) // cols
    ancho = pad * 2 + cols * W + (cols - 1) * gap
    probe = ImageDraw.Draw(Image.new("L", (10, 10)))
    ancho = max(ancho, int(probe.textlength(titulo, font=F("Ubuntu/Ubuntu-Bold.ttf", 44))) + 2 * pad,
                int(probe.textlength(bajada, font=F("Ubuntu/Ubuntu-Regular.ttf", 24))) + 2 * pad)
    out = Image.new("L", (ancho, top + filas * (H + cap) + pad), WHITE)
    d = ImageDraw.Draw(out)
    txt(d, pad, 36, titulo, F("Ubuntu/Ubuntu-Bold.ttf", 44))
    txt(d, pad, 92, bajada, F("Ubuntu/Ubuntu-Regular.ttf", 24))
    for i, (im, nombre) in enumerate(pantallas):
        c, r = i % cols, i // cols
        x = pad + c * (W + gap)
        y = top + r * (H + cap)
        out.paste(im.convert("L"), (x, y))
        d.rectangle([x - 1, y - 1, x + W, y + H], outline=0, width=2)
        txt(d, x, y + H + 14, nombre, F("NotoSans/NotoSans-Bold.ttf", 22))
    return out


if __name__ == "__main__":
    OUT = "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock"
    sheet("Lyra · el día manda, las aplicaciones son un dock",
          "Rail de 56 px en toda lista: ahí van el ícono, la casilla y la hora. El contenido siempre arranca en el mismo sitio.",
          [(lyra_hub(), "Hub"), (lyra_reminders(), "Recordatorios"),
           (lyra_editor(), "Recordatorio · editor"), (lyra_notes(), "Notas")],
          cols=4).save(f"{OUT}/lyra-1.png")
    sheet("Lyra · voz, tiempo y avisos",
          "Las pantallas modales no llevan rail: van centradas y con una sola cosa grande.",
          [(lyra_listening(), "Hablar · grabando"), (lyra_answer(), "Hablar · respuesta"),
           (lyra_timer(), "Temporizador"), (lyra_alarm(), "Recordatorio sonando")],
          cols=4).save(f"{OUT}/lyra-2.png")
    sheet("Lyra · contenido y sistema",
          "El mismo rail y los mismos antetítulos con regla en todas.",
          [(lyra_music(), "Música"), (lyra_weather(), "Clima"),
           (lyra_news(), "Noticias"), (lyra_settings(), "Ajustes")],
          cols=4).save(f"{OUT}/lyra-3.png")
    sheet("Lyra · la pantalla apagada",
          "Lo que queda puesto al suspender: la tinta no gasta, así que el día se sigue leyendo con el aparato dormido.",
          [(lyra_sleep(), "Suspendido")], cols=1).save(f"{OUT}/lyra-4.png")
    sheet("Diario v2 · sólo con las fuentes que ya están compiladas",
          "Nada pasa de NotoSerif 18, que son 37,5 px de em. Cero flash nuevo.",
          [(diario2_hub(), "Hub"), (diario2_reminders(), "Recordatorios"),
           (diario2_reader(), "Nota abierta"), (diario2_music(), "Música"),
           (diario2_sleep(), "Suspendido")], cols=5).save(f"{OUT}/diario2.png")
    print("listo")
