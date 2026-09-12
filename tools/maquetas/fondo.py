"""Fondo de pantalla de la ws397 en sus estados, con el tema Diario v2.

Es lo ULTIMO que se pinta antes de que el sistema se muera, asi que todo lo que
diga tiene que seguir siendo cierto con el aparato apagado: nada que dependa de
que el reloj siga corriendo. La hora va como SELLO (cuando se pinto), no como
reloj; lo que va grande es la proxima alarma, que no cambia mientras duerme.

Todo se arma con un cursor de arriba a abajo que MIDE antes de dibujar: los
titulares entran los que entren y se corta limpio, nunca a mitad de renglon.
"""
import sys
sys.path.insert(0, "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock")
from base import *
from base import ImageDraw, Image

OUT = "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/mock"
DM = 24
DCW = W - 2 * DM
RIGHT = W - DM
PIE_Y = H - 84          # arriba del pie: nada se dibuja por debajo

TITULARES = [
    ("El País", "hace 40 min",
     "La UE aprueba el reglamento de inteligencia artificial tras dos años de negociación",
     "Entra en vigor por etapas: los sistemas de riesgo alto tienen dos años para adecuarse."),
    ("La Nación", "hace 1 h",
     "El Banco Central bajó la tasa de referencia al 29 % anual",
     "Es el cuarto recorte del año y el más chico; el mercado esperaba medio punto más."),
    ("BBC Mundo", "hace 2 h",
     "Un telescopio detecta agua en la atmósfera de un planeta a 97 años luz",
     "El hallazgo no implica vida, pero sí que el vapor sobrevive a una estrella cercana."),
    ("Reforma", "hace 3 h",
     "Comienza la temporada de lluvias con 40 mm en el valle",
     "Protección Civil pide no cruzar avenidas encharcadas."),
]


def wrap_max(d, texto, font, ancho, maxlineas):
    """Corta en palabras y, si sobran renglones, deja puntos suspensivos."""
    lineas = wrap(d, texto, font, ancho)
    if len(lineas) <= maxlineas:
        return lineas
    cortadas = lineas[:maxlineas]
    resto = " ".join(lineas[maxlineas:])
    cortadas[-1] = fit(d, cortadas[-1] + " " + resto, font, ancho)
    return cortadas


def estado(d, palabra, sello):
    """Rótulo de estado: versalita espaciada en su propio renglón, y debajo el
    sello con la hora. Nunca una pastilla negra (regla de DISENO.md)."""
    spaced(d, DM, 16, palabra, SERIF16B, gap=6)
    txt(d, DM, 60, sello, SMALLB)
    rule(d, DM, 88, DCW, 3)
    rule(d, DM, 94, DCW, 1)
    return 112


def bloque_alarma(d, y, hora, titulo, cuando):
    spaced(d, DM, y, "Próxima alarma", SMALLB, gap=3)
    y += 26
    seven(d, DM, y, hora, h=54)
    x = DM + seven_width(hora, h=54) + 24
    txt(d, x, y + 6, cuando, SMALL)
    y += 62
    txt(d, DM, y, fit(d, titulo, SERIF16, DCW), SERIF16)
    return y + 46


def bloque_sin_alarma(d, y, frase):
    spaced(d, DM, y, frase, SMALLB, gap=3)
    return y + 30


def bloque_clima(d, y, temp, cielo, detalle, interior):
    txt(d, DM, y, temp, SERIF18B)
    txt(d, DM + 82, y + 10, cielo, SERIF16)
    txt(d, RIGHT, y + 6, interior, SMALLB, anchor="ra")
    txt(d, DM, y + 48, detalle, SMALL)
    return y + 74


def bloque_libro(d, y, titulo, autor):
    spaced(d, DM, y, "Continuar leyendo", SMALLB, gap=3)
    txt(d, DM, y + 24, fit(d, titulo, SERIF16, DCW), SERIF16)
    txt(d, DM, y + 66, autor, SMALL)
    return y + 96


def bloque_titulares(d, y, tope, total=14):
    """Titulares mientras entren entre `y` y `tope`. El primero lleva bajada,
    los demas van pelados: en un fondo de pantalla vale mas tener cinco
    titulares que dos titulares con resumen. Devuelve cuantos puso."""
    spaced(d, DM, y, "Titulares", SMALLB, gap=3)
    y += 28
    puestos = 0
    for medio, cuando, titular, bajada in TITULARES:
        con_bajada = puestos == 0
        lt = wrap_max(d, titular, SERIF14, DCW, 3)
        lb = wrap_max(d, bajada, SMALL, DCW, 2) if con_bajada else []
        alto = 22 + len(lt) * 30 + len(lb) * 22 + 20
        if y + alto > tope:
            break
        if puestos:
            rule(d, DM, y - 12, DCW)
        txt(d, DM, y, medio.upper(), SMALLB)
        txt(d, RIGHT, y, cuando, SMALL, anchor="ra")
        y += 22
        for linea in lt:
            txt(d, DM, y, linea, SERIF14)
            y += 30
        for linea in lb:
            txt(d, DM, y + 2, linea, SMALL)
            y += 22
        y += 20
        puestos += 1
    restan = total - puestos
    if restan > 0 and y + 22 <= tope:
        txt(d, DM, y, f"y {restan} titulares más en Noticias", SMALL)
    return puestos


def pie(d, pct, accion, nota=""):
    rule(d, DM, PIE_Y, DCW, 2)
    battery(d, DM, PIE_Y + 24, pct)
    txt(d, DM + 44, PIE_Y + 20, f"{pct} %", SMALL)
    if nota:
        txt(d, DM + 104, PIE_Y + 20, nota, SMALL)
    txt(d, RIGHT, PIE_Y + 20, accion, SMALLB, anchor="ra")


def suspendido():
    img, d = canvas()
    y = estado(d, "Suspendido", "21:53 · viernes 11 de septiembre")
    y = bloque_alarma(d, y, "06:00", "Mandar una factura", "mañana · recordatorio")
    rule(d, DM, y, DCW)
    y = bloque_clima(d, y + 24, "17°", "Nublado", "Iztacalco · máxima 24° · mínima 15° · humedad 85 %", "Interior 29°")
    rule(d, DM, y, DCW, 2)
    bloque_titulares(d, y + 18, PIE_Y - 12)
    pie(d, 73, "OK para volver", "6 compras · 2 tareas")
    return finish(img)


def apagado():
    img, d = canvas()
    y = estado(d, "Apagado", "21:53 · viernes 11 de septiembre")
    y = bloque_sin_alarma(d, y, "Apagado no suenan las alarmas")
    rule(d, DM, y, DCW)
    y = bloque_clima(d, y + 24, "17°", "Nublado", "Iztacalco · máxima 24° · mínima 15° · al apagarse", "Interior 29°")
    rule(d, DM, y, DCW, 2)
    bloque_titulares(d, y + 18, PIE_Y - 12)
    pie(d, 73, "PWR 1 s para encender")
    return finish(img)


def suspendido_sin_datos():
    """Primer arranque o sin sincronizar: no hay ni clima ni titulares."""
    img, d = canvas()
    y = estado(d, "Suspendido", "21:53 · viernes 11 de septiembre")
    y = bloque_sin_alarma(d, y, "Sin alarmas puestas")
    rule(d, DM, y, DCW)
    y += 28
    for linea in wrap(d, "Todavía no se sincronizó con tu cuenta. Conéctalo al WiFi y mantén Atrás en el hub: "
                         "después de eso aquí van el clima, lo que viene y los titulares del día.", SERIF14, DCW):
        txt(d, DM, y, linea, SERIF14)
        y += 32
    rule(d, DM, y + 18, DCW, 2)
    bloque_libro(d, y + 42, "Cien años de soledad", "Gabriel García Márquez · página 148 de 471")
    pie(d, 73, "OK para volver")
    return finish(img)


def suspendido_lector():
    """Se suspendió leyendo: el libro manda y los titulares entran igual."""
    img, d = canvas()
    y = estado(d, "Suspendido", "21:53 · viernes 11 de septiembre")
    y = bloque_libro(d, y, "Cien años de soledad", "Gabriel García Márquez · página 148 de 471 · 31 %")
    rule(d, DM, y, DCW)
    y = bloque_alarma(d, y + 24, "06:00", "Mandar una factura", "mañana · recordatorio")
    rule(d, DM, y, DCW, 2)
    bloque_titulares(d, y + 18, PIE_Y - 12)
    pie(d, 73, "OK para volver", "17° nublado")
    return finish(img)


if __name__ == "__main__":
    pantallas = [(suspendido(), "Suspendido"), (apagado(), "Apagado"),
                 (suspendido_lector(), "Suspendido leyendo"),
                 (suspendido_sin_datos(), "Suspendido · sin sincronizar")]
    pad, gap, top, cap = 40, 30, 150, 54
    cols = len(pantallas)
    ancho = pad * 2 + cols * W + (cols - 1) * gap
    out = Image.new("L", (ancho, top + H + cap + pad), WHITE)
    d = ImageDraw.Draw(out)
    txt(d, pad, 34, "Fondo de pantalla · Diario v2", F("Ubuntu/Ubuntu-Bold.ttf", 44))
    txt(d, pad, 92, "Lo último que se pinta antes de que el sistema se muera. La hora es un sello, no un reloj: "
                    "lo grande es la alarma, que no cambia mientras duerme. Los titulares entran los que entren.",
        F("Ubuntu/Ubuntu-Regular.ttf", 24))
    for i, (im, nombre) in enumerate(pantallas):
        x = pad + i * (W + gap)
        out.paste(im.convert("L"), (x, top))
        d.rectangle([x - 1, top - 1, x + W, top + H], outline=0, width=2)
        txt(d, x, top + H + 14, nombre, F("NotoSans/NotoSans-Bold.ttf", 22))
    out.save(f"{OUT}/fondo.png")
    print("ok", out.size)
