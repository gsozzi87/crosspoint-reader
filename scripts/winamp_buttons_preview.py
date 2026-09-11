import sys
sys.path.insert(0, "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/btn")
from sim import R, triangle, strip_actual, W
from PIL import Image

H = 260

# ---- B: a lo Winamp de verdad -------------------------------------------
# Botones CUADRADOS, sin redondeo, marco negro de 2 px y cara blanca. El
# transporte va pegado (bordes compartidos, como la botonera original) y los dos
# interruptores van aparte a la derecha. Foco = marco grueso; encendido = cara
# negra con el glifo en blanco, que es como Winamp marca shuffle/repeat.

def bar(r, x, y, w, h, black=True):
    r.fillRect(x, y, w, h, black)

def tri(r, x, y, size, right, black=True):
    for i in range(2 * size):
        ln = max(1, size - abs(i - size) + 1)
        if right:
            r.fillRect(x, y + i, ln, 1, black)
        else:
            r.fillRect(x + size - ln, y + i, ln, 1, black)

def glyph(r, act, cx, cy, on):
    ink = not on  # cara negra -> glifo blanco
    if act == "prev":
        bar(r, cx - 11, cy - 8, 3, 16, ink)
        tri(r, cx - 6, cy - 8, 8, False, ink)
    elif act == "play":
        tri(r, cx - 5, cy - 8, 8, True, ink)
    elif act == "pause":
        bar(r, cx - 7, cy - 8, 5, 16, ink); bar(r, cx + 3, cy - 8, 5, 16, ink)
    elif act == "next":
        tri(r, cx - 3, cy - 8, 8, True, ink)
        bar(r, cx + 8, cy - 8, 3, 16, ink)
    elif act == "stop":
        bar(r, cx - 7, cy - 7, 15, 15, ink)
    elif act == "shuffle":
        # Las dos flechas cruzadas de siempre: cada una entra por la izquierda,
        # cruza en diagonal y sale por la derecha con su punta. Se cruzan en el
        # medio, que es lo que hace que se lea "mezclar".
        r.drawLine(cx - 12, cy - 7, cx - 4, cy - 7, 2, ink)
        r.drawLine(cx - 4, cy - 7, cx + 5, cy + 6, 2, ink)
        tri(r, cx + 5, cy + 2, 5, True, ink)
        r.drawLine(cx - 12, cy + 6, cx - 4, cy + 6, 2, ink)
        r.drawLine(cx - 4, cy + 6, cx + 5, cy - 7, 2, ink)
        tri(r, cx + 5, cy - 11, 5, True, ink)
    elif act == "repeat":
        # Lazo cerrado con un hueco arriba a la derecha y la punta saliendo por
        # ahi. Cerrado se lee como "vuelve a empezar"; abierto parecia una
        # flecha de exportar.
        r.drawLine(cx - 11, cy - 8, cx + 3, cy - 8, 2, ink)
        r.drawLine(cx + 11, cy - 5, cx + 11, cy + 8, 2, ink)
        r.drawLine(cx + 11, cy + 8, cx - 11, cy + 8, 2, ink)
        r.drawLine(cx - 11, cy + 8, cx - 11, cy - 8, 2, ink)
        tri(r, cx + 4, cy - 13, 5, True, ink)

def button(r, x, y, w, h, focused, on):
    r.fillRect(x, y, w, h, on)            # cara: negra si el interruptor esta encendido
    r.drawRect(x, y, w, h, 2, True)       # marco
    if focused:
        r.drawRect(x + 3, y + 3, w - 6, h - 6, 2, not on)  # marco interior = foco

def strip_b(r, y, h=46, playing=True, focus=1, shuffle_on=True, repeat_on=False):
    x = 16
    bw = 60
    acts = ["prev", "pause" if playing else "play", "next", "stop"]
    for i, a in enumerate(acts):
        bx = x + i * (bw - 2)   # -2: bordes compartidos, como la botonera original
        button(r, bx, y, bw, h, focus == i, False)
        glyph(r, a, bx + bw // 2, y + h // 2, False)
    tx = x + 4 * (bw - 2) + 18
    for j, a in enumerate(["shuffle", "repeat"]):
        on = shuffle_on if a == "shuffle" else repeat_on
        bx = tx + j * (bw + 10)
        button(r, bx, y, bw + 10, h, focus == 4 + j, on)
        glyph(r, a, bx + (bw + 10) // 2, y + h // 2, on)

r = R(W, H)
r.d.text((16, 6), "A - lo que hay hoy", fill=0)
strip_actual(r, 26)
r.d.text((16, 100), "B - propuesta (cuadrados, bordes compartidos, glifos nuevos)", fill=0)
strip_b(r, 120)
r.d.text((16, 180), "B con el foco en STOP y repeat encendido", fill=0)
strip_b(r, 200, focus=3, shuffle_on=False, repeat_on=True)
r.im.resize((W * 2, H * 2), Image.NEAREST).save(
    "/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/btn/ab.png")
print("ok")
