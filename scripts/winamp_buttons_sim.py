# Mini-simulador de GfxRenderer: 1 bit, mismas primitivas, para poder MIRAR
# la botonera antes de meterla en el aparato.
from PIL import Image, ImageDraw

W, H = 480, 120
SCALE = 2

class R:
    def __init__(s, w, h):
        s.im = Image.new("L", (w, h), 255)
        s.d = ImageDraw.Draw(s.im)
    def fillRect(s, x, y, w, h, black=True):
        s.d.rectangle([x, y, x+w-1, y+h-1], fill=0 if black else 255)
    def drawRect(s, x, y, w, h, t=1, black=True):
        for i in range(t):
            s.d.rectangle([x+i, y+i, x+w-1-i, y+h-1-i], outline=0 if black else 255)
    def fillRoundedRect(s, x, y, w, h, r, col=255):
        s.d.rounded_rectangle([x, y, x+w-1, y+h-1], radius=r, fill=col)
    def drawRoundedRect(s, x, y, w, h, t, r, black=True):
        for i in range(t):
            s.d.rounded_rectangle([x+i, y+i, x+w-1-i, y+h-1-i], radius=max(0, r-i), outline=0 if black else 255)
    def drawLine(s, x0, y0, x1, y1, t=1, black=True):
        s.d.line([x0, y0, x1, y1], fill=0 if black else 255, width=t)
    def dither(s, x, y, w, h, light=True):
        step = 2 if light else 1
        for yy in range(y, y+h):
            for xx in range(x, x+w):
                if (xx + yy) % (3 if light else 2) == 0:
                    s.d.point((xx, yy), fill=0)

def triangle(r, x, y, size, right):
    # mismo helper del firmware: triangulo relleno apuntando a derecha/izquierda
    for i in range(2*size):
        ln = size - abs(i - size) + 1
        ln = max(1, ln)
        half = ln
        if right:
            r.fillRect(x, y+i, half, 1, True)
        else:
            r.fillRect(x + size - half, y+i, half, 1, True)

ACTS = ["prev", "play", "next", "stop", "shuffle", "repeat"]

# ---------------- A: lo que hay hoy -------------------------------------
def bevel_actual(r, x, y, w, h, radius, pressed):
    r.fillRoundedRect(x, y, w, h, radius, 200 if pressed else 255)
    r.drawRoundedRect(x, y, w, h, 3 if pressed else 2, radius, True)
    if not pressed:
        r.fillRect(x+radius, y+3, w-2*radius, 1, True)

def icon(r, act, cx, cy):
    if act == "prev":
        r.fillRect(cx-10, cy-8, 3, 16); triangle(r, cx-6, cy-8, 8, False)
    elif act == "play":
        triangle(r, cx-6, cy-8, 8, True)
    elif act == "next":
        triangle(r, cx-2, cy-8, 8, True); r.fillRect(cx+7, cy-8, 3, 16)
    elif act == "stop":
        r.fillRect(cx-7, cy-7, 14, 14)
    elif act == "shuffle":
        r.drawLine(cx-10, cy-6, cx+4, cy+6, 2); r.drawLine(cx-10, cy+6, cx+4, cy-6, 2)
        triangle(r, cx+4, cy+2, 5, True); triangle(r, cx+4, cy-12, 5, True)
    elif act == "repeat":
        r.drawRect(cx-10, cy-7, 20, 14, 2); r.fillRect(cx+1, cy-9, 10, 4, False)
        triangle(r, cx+3, cy-11, 4, True)

def strip_actual(r, y, h=54):
    x, w, gap = 16, 448, 4
    bw = (w - gap*5)//6
    for i, a in enumerate(ACTS):
        bx = x + i*(bw+gap)
        bevel_actual(r, bx, y, bw, h, 6, i == 1)
        icon(r, a, bx+bw//2, y+h//2-4)
        if a in ("shuffle",):
            r.fillRect(bx+8, y+h-9, bw-16, 4)

r = R(W, H)
r.d.text((16, 6), "A - lo que hay hoy", fill=0)
strip_actual(r, 30)
r.im.resize((W*SCALE, H*SCALE), Image.NEAREST).save("/tmp/claude-0/-home-user-crosspoint-reader/8ba53e74-1f42-5706-9b62-733865469812/scratchpad/btn/a.png")
print("ok")
