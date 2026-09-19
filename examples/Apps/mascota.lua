-- Mascota: una perrita salchicha de bolsillo que vive en la tarjeta.
--
-- Un tamagotchi básico: cuatro necesidades (hambre, ánimo, energía e higiene)
-- que bajan con el tiempo REAL —también mientras la app está cerrada— y cinco
-- acciones en la lista de abajo: Alimentar, Jugar (mayor o menor, tres
-- rondas), Dormir/Despertar, Limpiar e Info (donde también se le cambia el
-- nombre). La primera vez pregunta cómo se llama: se dice por el micrófono o
-- se acepta el nombre de fábrica; una escucha que falla vuelve a preguntar,
-- nunca bautiza sola. Si se la descuida mucho tiempo, se va; OK trae un huevo.
--
-- Gestos: sacudir el aparato la despierta de golpe (y se pone de mal humor);
-- boca abajo la manda a dormir.
--
-- Cuándo repinta: on_tick devuelve true SÓLO cuando cambió algo que se ve
-- (el cuadro de la animación cada 4 s, un renglón de las barras, un aviso que
-- vence). Devolver true en cada tick sería un refresco parcial cada 120 ms.
--
-- DIBUJOS: no son nuestros. Son siluetas de dominio público (CC0,
-- https://creativecommons.org/publicdomain/zero/1.0/) bajadas de openclipart.org
-- e iconos MIT; los detalles están en docs/ws397/MASCOTA.md:
--   * La perrita parada (quieta1, quieta2, feliz, triste, sefue):
--     "Dachshund", de eevee93, https://openclipart.org/detail/340936/dachshund
--     (CC0). Los cuadros son la MISMA silueta con la cola girada alrededor de
--     su raíz (arriba = contenta, abajo = triste); "se fue" es la silueta
--     espejada y más chica, yéndose. Transformaciones del original, no dibujos.
--   * La perrita dormida (duerme): "Resting/sleeping dog", de f_featherbrain,
--     https://openclipart.org/detail/163699/restingsleeping-dog (CC0).
--   * Los iconos (comida, pelota, balde, caca) y el huevo: proyecto OpenCritter
--     de SuperMechaCow, licencia MIT, Copyright (c) 2017 SuperMechaCow.
--     https://github.com/SuperMechaCow/OpenCritter (gfx/bitmaps).
-- La conversión la hace convert2.py (resvg + Pillow): reducción a 150 px de
-- ancho, umbral y empaquetado MSB primero, 1 = tinta, como pide cp.image(),
-- que las pinta a escala 2 (300 px de ancho en el vidrio).

-- Generado por convert2.py (scratchpad): NO editar a mano.
local SPRITES = {
  quieta1 = {w = 150, h = 56, hex = "0000000000000000000000000000007f000000000000000000000000000000000001ffe00000000000000000000000000000000007fff0000000000000000000000000000000000ffff8000000000000000000000000000000000ffffc000000000000000000000000000000001ffffe000000000000000000000000000000001fffff000000000000000000000000000000001fffff800000000000000000000000000000003ffffff80000000000000000000000000000003fffffff800000000000000000000000000001ffffffffc0000000000000000000000ffffff3fffffffff000000000000ffffc001ffffffffffffffffff800000000003ffffffffffffffffffffffffffc0000000001fffffffffffffffffffffffffffc0000000007fffffffffffffffffffffffffffc000000000ffffffffffffffffffffffffffff8000000003fffffffffffffffffffffffffffe0000000007ffffffffffffffffffffffffff80000000001fffffffffffffffffffffffff8000000000003fffffffffffffffffffffffff8000000000007fffffffffffffffffffffffff800000000000ffffffffffffffffffffffefff800000000001ffffffffffffffffffffff87ff800000000003fcffffffffffffffffffff03ff000000000007f0ffffffffffffffffffff007e00000000000fe0ffffffffffffffffffff800000000000001fc0ffffffffffffffffffff800000000000003f807fffffffffffffffffff800000000000007f003fffffffffffffffffff80000000000000fe001fffffffffffffffffff80000000000001fc000fffffffffffffffffff80000000000001f80003ffff01ffffffffffff80000000000003f00003fffe001fffffffffff80000000000007e00001fffc0003ffffffffff8000000000001fc00000fff800007fffffffff8000000000003f800000fff000000fffffffff0000000000007f000000fff0000000fffffffe000000000001fe000001ffe000000007fffffc000000000007f8000003ffc000000000000ff800000000000fc0000007ff0000000000000ff800000000003f0000000ffe0000000000000ff80000000000f80000001ffc0000000000000ff00000000003800000003ffc0000000000000ff00000000004000000003fff0000000000000ff00000000000000000007fffe000000000000fe00000000000000000007ffff000000000000ff00000000000000000007ffff800000000000ff00000000000000000007ffffc00000000000ff80000000000000000003ffffc00000000000ff80000000000000000000ffffc00000000000ffe00000000000000000003fff8000000000007ff80000000000000000001ffc0000000000007ffc00000000000000000007fe0000000000007ff000000000000000000001fc0000000000001ff000000000000000000000f8000000000000000000000000"},
  quieta2 = {w = 150, h = 56, hex = "0000000000000000000000000000007f000000000000000000000000000000000001ffe00000000000000000000000000000000007fff0000000000000000000000000000000000ffff8000000000000000000000000000000000ffffc000000000000000000000000000000001ffffe000000000000000000000000000000001fffff000000000000000000000000000000001fffff800000000000000000000000000000003ffffff80000000000000000000000000000003fffffff800000000000000000000000000001ffffffffc0000000000000000000000ffffff3fffffffff000000000000ffffc001ffffffffffffffffff800000000003ffffffffffffffffffffffffffc0000000001fffffffffffffffffffffffffffc0000000007fffffffffffffffffffffffffffc000000000ffffffffffffffffffffffffffff8000000007fffffffffffffffffffffffffffe000000001fffffffffffffffffffffffffff80000000007fffffffffffffffffffffffff800000000001ffffffffffffffffffffffffff800000000003ffffffffffffffffffffffffff80000000000fffffffffffffffffffffffefff80000000001ffd7fffffffffffffffffff87ff80000000007fe17fffffffffffffffffff03ff0000000000ff817fffffffffffffffffff007e0000000001ff007fffffffffffffffffff80000000000003fc007fffffffffffffffffff8000000000000ff8007fffffffffffffffffff8000000000001fe0003fffffffffffffffffff8000000000007fc0001fffffffffffffffffff800000000001ff80000fffffffffffffffffff80000000000ffe000003ffff01ffffffffffff80000000003ff8000003fffe001fffffffffff8000000001ffe0000001fffc0003ffffffffff800000000ffc00000000fff800007fffffffff80000000080000000000fff000000fffffffff00000000000000000000fff0000000fffffffe00000000000000000001ffe000000007fffffc00000000000000000003ffc000000000000ff800000000000000000007ff0000000000000ff80000000000000000000ffe0000000000000ff80000000000000000001ffc0000000000000ff00000000000000000003ffc0000000000000ff00000000000000000003fff0000000000000ff00000000000000000007fffe000000000000fe00000000000000000007ffff000000000000ff00000000000000000007ffff800000000000ff00000000000000000007ffffc00000000000ff80000000000000000003ffffc00000000000ff80000000000000000000ffffc00000000000ffe00000000000000000003fff8000000000007ff80000000000000000001ffc0000000000007ffc00000000000000000007fe0000000000007ff000000000000000000001fc0000000000001ff000000000000000000000f8000000000000000000000000"},
  feliz = {w = 150, h = 56, hex = "0000000000000000000000000000007f000000000000000000000000000000000001ffe00000000000000000000000000000000007fff0000000000000000000000000000000000ffff8000000000000000000000000000000000ffffc000000000000000000000000000000001ffffe000000000000000000000000000000001fffff000000000000000000000000000000001fffff800000000000000000000000000000003ffffff80000000000000000000000000000003fffffff800000000000000000000000000001ffffffffc0000000000000000000000ffffff3fffffffff000000000000ffffc001ffffffffffffffffff800000000003ffffffffffffffffffffffffffc0000000001fffffffffffffffffffffffffffc0000000007fffffffffffffffffffffffffffc0000003ffffffffffffffffffffffffffffff800000fffffffffffffffffffffffffffffffe0e001fffffffffffffffffffffffffffffff800fffffffffffffffffffffffffffffffff800007ffffffffffffffffffffffffffffffff800000ffffff9f7fffffffffffffffffffffff8000001fff80027fffffffffffffffffffefff80000000c000047fffffffffffffffffff87ff800000000000087fffffffffffffffffff03ff000000000000087fffffffffffffffffff007e000000000000007fffffffffffffffffff8000000000000000007fffffffffffffffffff8000000000000000007fffffffffffffffffff8000000000000000003fffffffffffffffffff8000000000000000001fffffffffffffffffff8000000000000000000fffffffffffffffffff80000000000000000003ffff01ffffffffffff80000000000000000003fffe001fffffffffff80000000000000000001fffc0003ffffffffff80000000000000000000fff800007fffffffff80000000000000000000fff000000fffffffff00000000000000000000fff0000000fffffffe00000000000000000001ffe000000007fffffc00000000000000000003ffc000000000000ff800000000000000000007ff0000000000000ff80000000000000000000ffe0000000000000ff80000000000000000001ffc0000000000000ff00000000000000000003ffc0000000000000ff00000000000000000003fff0000000000000ff00000000000000000007fffe000000000000fe00000000000000000007ffff000000000000ff00000000000000000007ffff800000000000ff00000000000000000007ffffc00000000000ff80000000000000000003ffffc00000000000ff80000000000000000000ffffc00000000000ffe00000000000000000003fff8000000000007ff80000000000000000001ffc0000000000007ffc00000000000000000007fe0000000000007ff000000000000000000001fc0000000000001ff000000000000000000000f8000000000000000000000000"},
  triste = {w = 150, h = 56, hex = "0000000000000000000000000000007f000000000000000000000000000000000001ffe00000000000000000000000000000000007fff0000000000000000000000000000000000ffff8000000000000000000000000000000000ffffc000000000000000000000000000000001ffffe000000000000000000000000000000001fffff000000000000000000000000000000001fffff800000000000000000000000000000003ffffff80000000000000000000000000000003fffffff800000000000000000000000000001ffffffffc0000000000000000000000ffffff3fffffffff000000000000ffffc001ffffffffffffffffff800000000003ffffffffffffffffffffffffffc0000000001fffffffffffffffffffffffffffc0000000007fffffffffffffffffffffffffffc0000000017fffffffffffffffffffffffffff80000000037ffffffffffffffffffffffffffe0000000007ffffffffffffffffffffffffff800000000007ffffffffffffffffffffffff8000000000000fffffffffffffffffffffffff8000000000001fffffffffffffffffffffffff8000000000003fffffffffffffffffffffefff8000000000007fffffffffffffffffffff87ff8000000000007f7fffffffffffffffffff03ff000000000000fe7fffffffffffffffffff007e000000000001fc7fffffffffffffffffff8000000000000001f87fffffffffffffffffff8000000000000003f07fffffffffffffffffff8000000000000003f03fffffffffffffffffff8000000000000007e01fffffffffffffffffff8000000000000007e00fffffffffffffffffff800000000000000fc003ffff01ffffffffffff800000000000000f8003fffe001fffffffffff800000000000001f8001fffc0003ffffffffff800000000000001f0000fff800007fffffffff800000000000003f0000fff000000fffffffff000000000000003e0000fff0000000fffffffe000000000000007e0001ffe000000007fffffc00000000000000fc0003ffc000000000000ff800000000000000f80007ff0000000000000ff800000000000001f8000ffe0000000000000ff800000000000003f0001ffc0000000000000ff000000000000007c0003ffc0000000000000ff00000000000000f80003fff0000000000000ff00000000000001f00007fffe000000000000fe00000000000003c00007ffff000000000000ff00000000000007800007ffff800000000000ff0000000000000e000007ffffc00000000000ff8000000000001c000003ffffc00000000000ff80000000000030000000ffffc00000000000ffe00000000000c00000003fff8000000000007ff80000000000000000001ffc0000000000007ffc00000000000000000007fe0000000000007ff000000000000000000001fc0000000000001ff000000000000000000000f8000000000000000000000000"},
  duerme = {w = 120, h = 81, hex = "000000000000000001fc000000000000000000000000003fffe000000000000000000000003fffffffc000000000000000000007fffffffffe000000000000000000ffffffffffffc0000000000000000ffffffffffffff0000000000000007ffffffffffffffe00000000000003ffffffffffffffff0000000000001fffffffffffffffffc000000000007fffffffffffffffffe00000000003fffffffffffffffffff8000000000ffffffffffffffffffffc000000003ffffffffffffffffffffe00000000ffffffffffffffffffffff00000003ffffffffffffffffffffff00000007ffffffffffffffffffffff8000001fffffffffffffffffffffffc000003fc001ffffffffffffffffffc00000fc00007fffffffffffffffffe00001f000003fffffffffffffffffe00003c01ff81ffffffffffffffffff0000780fffe0ffffffffffffffffff0000f03ffff0ffffffffffffffffff8001f0fffff8ffffffffffffffffff8003fffffff8ffffffffffffffffff8007fffffff8ffffffffffffffffff800ffffffff8ffffffffffffffffffc01ffffffff8fffffffffffffe3fffc03ffffffff8fffffffffffffe1fffc03ffffffff8fffffffffffffe0fffc07ffffffff0ffffc0007fffff0fffc07ffffffff1fff000000fffff07ffc0fffffffff1ff00000007ffff87ffe0ffffffffe1f800000003fe3fc7ffe1ffffffffe3c000000001e00fc7ffe1ffffffffc00000000000000387fff3ffffffffc00000000000000007fff3ffffffff80000000000007800ffff3ffffffff800000000000ff801ffff3ffffffff000000000007ffc1fffff7ffffffff00000000000fffe0ffffe7fffffffe00000000001ffff07fffe7e7fffffe00000000003ffff83fffcfc3fffffc00000000003ffffc1fffcfc3fffffc00000000007ff8fe0fffc7c1fffc7c00000000007ff8ff07ffc7c0fff07800000000007ff8ff83ff87c0ffe07880000000007ff8ffc1ff87c03f807880000000007ff8ffe0ff83c00c00f8c000000000fff87ff07f03c00000f84000000003fff87ff83f03e00000f8600000000ffffc7ffc1e03e00000fc200000007ffffc7ffe0e03e00000fc08000001fffffc7fff0401e000007e00000007fffffc3fff8401e000003f0000001ffffffc3fff8001f000001fc000007ffffffc3fff8000f0000003f000007fffffe01fff8000f80000003f80007ffff80007ff000078000000007fc03ff80000007e00007c0000000000fc1fc00000000000003c000000000000000003e0000000003e00000000000000000f80010000001f00000000000000003c0003fc00000f0000000000000001e00007f8000007800000000000001f00000ff0000003c0000000000001e000001fc0000001e000000000003e0000003f80000000f00000000003e0000000fe0000000078000000001f00000001f8000000003e000000007c00000007e0000000001f00000000700000003f8000000000078000000000000001fc000000000001e00000000000000fe000000000000078000000000000fe000000000000001c000000000007f0000000000000000780000000007f000000000000000000e000000001f80000000000000000000000000007e0000000000000000000000000000fc000000000000000000000000000020000000000000"},
  sefue = {w = 100, h = 38, hex = "0003f000000000000000000000000ffc00000000000000000000001ffe00000000000000000000003ffe00000000000000000000003ffe0000000000000000000001ffff000000000000000000000fffff800000000000000000003fffffc1ff80000000000000007fffffffffffc03ffe00000000ffffffffffffffffff80000000ffffffffffffffffffe00000007ffffffffffffffffff800000003fffffffffffffffffc000000001ffffffffffffffffe000000001fffffffffffffffff800000001ff7ffffffffffffffc00000000fe3fffffffffffff7e00000000703fffffffffffff1f00000000003fffffffffffff0f80000000003ffffffffffffe07c0000000003ffffffffffffc03c0000000003ffffffffffff801e0000000003fffffffc0fff000f0000000003ffffffc007fe000f8000000001fffffe0003fe0007c000000000fffff00001fe0001f000000000fff8000000ff0000f8000000007e000000007f80003e000000007e000000001fc00007800000007e000000001fc00000e00000003e00000000ffe00000000000003e00000003ffe00000000000007e00000003ffe00000000000007e00000007ffe0000000000001fc00000003ff80000000000003fc00000000fe00000000000001f800000000fc00000000000000e000000000f000000000"},
  comida = {w = 16, h = 16, hex = "1ff82084400288218001ffff400240023f0240fc40024002ffff800140023ffc"},
  pelota = {w = 16, h = 16, hex = "000007e018182424224442424422481248124812442223c42004181807e00000"},
  balde = {w = 16, h = 16, hex = "00001ff8200440028001c003a0059ff980014002400240024002200420041ff8"},
  caca = {w = 16, h = 16, hex = "000005200220507020f001f003f007e00f901f783efc39fc3ffc3ffc1ff80000"},
  huevo1 = {w = 32, h = 32, hex = "00000000000000000000000000000000000fe000001830000030180000600c0000c00600018003000300018002000080060000c0040000400c0000600800002008000020080000201800003010000010100000101000003010000150180000b008000560080002a004000540030015800182ab0000c95600007abc00000fe000"},
  huevo2 = {w = 32, h = 32, hex = "000fe000001830000030180000600c0000c0060000800200018003000100010003000180060000c004000040040000400c000060080000200800002008000020180000301000003010000010100000501000003010000150180000b008000560080002a00c00056006002ac0030015800182ab0000c95600007abc00000fe000"},
}

-- ================================================================== constantes
local HORA = 3600
local TOPE_AUSENCIA = 12 * HORA   -- al volver se descuenta como mucho medio día
local SE_VA_TRAS = 36 * HORA      -- segundos seguidos con una necesidad en cero
local CUADRO_MS = 4000            -- la animación cambia de cuadro cada 4 s
local AVISO_MS = 4000             -- cuánto dura un aviso en pantalla
local GUARDAR_MS = 5 * 60 * 1000  -- guardado de fondo, además de cada acción
local NOMBRE_DEFAULT = "Pipo"
local RONDAS = 3
local ESCALA = 2                  -- las siluetas: 150 px de ancho → 300 en el vidrio

-- Retícula de la pantalla principal (sistema visual: margen 24, paso 8).
local W, H = 480, 800
local MARGEN = 24
local ANCHO = W - 2 * MARGEN      -- 432
local CABEZAL_Y = 40              -- título UI_14
local REGLA_Y = 78                -- regla de 1 px bajo el cabezal
local ZONA_Y, ZONA_H = 96, 208    -- la perra vive acá, centrada
local ESTADO_Y = 312              -- renglón de estado UI_12
local AVISO_Y = 340               -- aviso UI_10
local BARRAS_Y, BARRA_PASO = 376, 36
local LISTA_Y, FILA_H = 536, 40   -- cinco filas
local LISTA_CORTA_Y = 400         -- las pantallas sin barras (nombre, se fue) suben la lista
local AYUDA_Y = 760               -- renglón de ayuda UI_10

-- ====================================================================== estado
local m = nil                -- la mascota (ver nacer())
local pantalla = "nombre"    -- "nombre" | "casa" | "juego" | "info" | "sefue"
local fila = 1               -- fila elegida en casa
local filaNombre = 1         -- fila elegida en la pantalla del nombre
local aviso, avisoHasta = nil, 0
local accion, accionHasta = nil, 0   -- "come" | "limpia" | "juega"
local cuadro, cuadroMs = 0, 0        -- 0/1: alterna cada CUADRO_MS
local ultimoMs, ultimoEpoch = nil, nil
local guardadoMs = 0
local firma = nil                    -- lo último que se pintó
local juego = nil
local escuchando = false
local nombreMsg = nil                -- "No entendí…" en la pantalla del nombre

local FILAS = {
  {id = "comer",   texto = "Alimentar", meta = "+30 de hambre"},
  {id = "jugar",   texto = "Jugar",     meta = "mayor o menor"},
  {id = "dormir",  texto = "Dormir",    meta = "recupera energía"},
  {id = "limpiar", texto = "Limpiar",   meta = "higiene al 100"},
  {id = "info",    texto = "Info",      meta = "y cambiar el nombre"},
}

-- ==================================================================== dibujos
-- Los strings hexa se decodifican una vez, con gsub sobre una tabla (corre en
-- C: ni una llamada a Lua por byte).
local HEX, BITS = nil, {}

local function bits(nombre)
  local b = BITS[nombre]
  if b then return b end
  if not HEX then
    HEX = {}
    for i = 0, 255 do HEX[string.format("%02x", i)] = string.char(i) end
  end
  b = (SPRITES[nombre].hex:gsub("%x%x", HEX))
  BITS[nombre] = b
  return b
end

local function dibujo(nombre, x, y, escala)
  local s = SPRITES[nombre]
  cp.image(x, y, s.w, s.h, bits(nombre), escala or 1)
end

local function centrado(y, s, tam, negrita)
  cp.text((W - cp.textw(s, tam or 12)) // 2, y, s, tam or 12, negrita)
end

local function derecha(y, s, tam, negrita)
  cp.text(W - MARGEN - cp.textw(s, tam or 12), y, s, tam or 12, negrita)
end

-- Cabezal de pantalla: título UI_14 a la izquierda, metadato a la derecha, regla.
local function cabezal(titulo, meta)
  cp.text(MARGEN, CABEZAL_Y, titulo, 14, true)
  if meta then derecha(CABEZAL_Y + 3, meta, 12) end
  cp.line(MARGEN, REGLA_Y, W - MARGEN, REGLA_Y, 1)
end

-- Una fila de lista de un renglón: resalte del sistema si está elegida, título
-- UI_12 y metadato UI_10 a la derecha. El texto nunca cae sobre la trama: el
-- resalte trama sólo los márgenes y acá el texto arranca 24 px adentro.
local function filaLista(y, elegida, texto, meta)
  if elegida then cp.selection(MARGEN, y, ANCHO, FILA_H) end
  cp.text(MARGEN + 24, y + 5, texto, 12)
  if meta then cp.text(W - MARGEN - 24 - cp.textw(meta, 10), y + 8, meta, 10) end
end

local function ayuda(s)
  cp.text(MARGEN, AYUDA_Y, s, 10)
end

-- ===================================================================== tiempo
local function epochAhora()
  local t = cp.time()
  return t and t.epoch or nil
end

local function tope(v)
  if v < 0 then return 0 elseif v > 100 then return 100 end
  return v
end

local function avisar(texto)
  aviso = texto
  avisoHasta = cp.ms() + AVISO_MS
end

local function mostrar(que)
  accion = que
  accionHasta = cp.ms() + AVISO_MS
end

-- El paso del tiempo, en segundos. Despierta baja todo; dormida recupera
-- energía y lo demás baja despacio. Con hambre o sucia se acumula descuido, y
-- con descuido de sobra se va.
local function pasar(seg)
  if not m or not m.viva or seg <= 0 then return end
  local h = seg / HORA
  m.edad = m.edad + seg
  if m.duerme then
    m.energia = tope(m.energia + 20 * h)
    m.hambre = tope(m.hambre - 3 * h)
    m.animo = tope(m.animo - 1 * h)
    m.higiene = tope(m.higiene - 2 * h)
    if m.energia >= 100 then m.duerme = false end
  else
    m.hambre = tope(m.hambre - 8 * h)
    m.animo = tope(m.animo - 6 * h)
    m.energia = tope(m.energia - 5 * h)
    m.higiene = tope(m.higiene - 4 * h)
  end
  if m.hambre <= 0 or m.higiene <= 0 or (m.animo <= 0 and m.energia <= 0) then
    m.descuido = m.descuido + seg
    if m.descuido >= SE_VA_TRAS then
      m.viva = false
      m.duerme = false
    end
  else
    m.descuido = math.max(0, m.descuido - seg)
  end
end

-- ==================================================================== guardar
local function guardar()
  if not m then
    cp.save("")
    return
  end
  cp.save(table.concat({
    "mascota1", m.nombre,
    string.format("%.2f;%.2f;%.2f;%.2f", m.hambre, m.animo, m.energia, m.higiene),
    m.duerme and 1 or 0, math.floor(m.descuido), math.floor(m.edad),
    m.epoch or 0, m.viva and 1 or 0,
  }, ";"))
  guardadoMs = cp.ms()
end

local function cargar()
  local s = cp.load()
  if type(s) ~= "string" then return nil end
  local p = {}
  for campo in (s .. ";"):gmatch("([^;]*);") do p[#p + 1] = campo end
  if p[1] ~= "mascota1" or #p < 11 then return nil end
  local function num(i, max)
    local v = tonumber(p[i]) or 0
    if v < 0 then v = 0 end
    if max and v > max then v = max end
    return v
  end
  local nombre = p[2]
  if nombre == "" then nombre = NOMBRE_DEFAULT end
  local epoch = num(10)
  return {
    nombre = nombre,
    hambre = num(3, 100), animo = num(4, 100), energia = num(5, 100), higiene = num(6, 100),
    duerme = p[7] == "1", descuido = num(8), edad = num(9),
    epoch = epoch > 0 and epoch or nil, viva = p[11] ~= "0",
  }
end

-- ===================================================================== nombre
-- De lo transcripto se toma la primera palabra (sacando un "se llama" delante),
-- sin puntuación, con mayúscula y hasta 12 letras. nil si no queda nada.
local function limpiarNombre(t)
  if type(t) ~= "string" then return nil end
  t = t:gsub("^%s*[Ss]e llama%s+", ""):gsub("^%s*[Ll]l[áa]ma[lt][oae]%s+", "")
  local palabra = t:match("[^%s%p%c;]+")
  if not palabra or not utf8.len(palabra) then return nil end
  if utf8.len(palabra) > 12 then palabra = palabra:sub(1, utf8.offset(palabra, 13) - 1) end
  return palabra:sub(1, 1):upper() .. palabra:sub(2)
end

local function nacer(nombre)
  m = {
    nombre = nombre, hambre = 80, animo = 80, energia = 80, higiene = 80,
    duerme = false, descuido = 0, edad = 0, epoch = epochAhora(), viva = true,
  }
  pantalla = "casa"
  fila = 1
  escuchando = false
  nombreMsg = nil
  avisar("¡Hola, " .. nombre .. "!")
  cp.beep("ok")
  guardar()
end

local function escuchar()
  escuchando = cp.listen(6, "Di el nombre") and true or false
  if not escuchando then nombreMsg = "No se pudo escuchar" end
end

function on_heard(texto)
  escuchando = false
  local nombre = limpiarNombre(texto)
  if pantalla == "nombre" then
    if nombre then
      nacer(nombre)
    else
      nombreMsg = "No entendí, intenta otra vez"   -- se queda acá, sin bautizar sola
      cp.beep("error")
    end
  elseif pantalla == "info" and m then
    if nombre then
      m.nombre = nombre
      avisar("Ahora se llama " .. nombre)
      cp.beep("ok")
      guardar()
    else
      avisar("No entendí, intenta otra vez")
      cp.beep("error")
    end
  end
end

-- ==================================================================== acciones
local function estadoTexto()
  if not m.viva then return m.nombre .. " se fue" end
  if m.duerme then return "Duerme" end
  if m.hambre < 25 then return "Tiene hambre" end
  if m.higiene < 25 then return "Necesita un baño" end
  if m.energia < 20 then return "Está agotada" end
  if m.animo < 25 then return "Está triste" end
  if m.animo >= 70 and m.hambre >= 50 then return "Está feliz" end
  return "Está tranquila"
end

local function cuadroPerra()
  if not m.viva then return "sefue" end
  if m.duerme then return "duerme" end
  if accion == "juega" or (m.animo >= 70 and m.hambre >= 50 and m.energia >= 30) then
    return cuadro == 0 and "feliz" or "quieta2"
  end
  if m.hambre < 25 or m.animo < 25 or m.higiene < 25 or m.energia < 20 then
    return cuadro == 0 and "triste" or "quieta1"
  end
  return cuadro == 0 and "quieta1" or "quieta2"
end

local function dormida()
  avisar("Duerme: despiértala primero")
  cp.beep("error")
end

local function comer()
  if m.duerme then return dormida() end
  if m.hambre >= 95 then
    avisar("No tiene hambre")
    cp.beep("error")
    return
  end
  m.hambre = tope(m.hambre + 30)
  m.higiene = tope(m.higiene - 3)
  mostrar("come")
  avisar("Ñam, ñam")
  cp.beep("ok")
  guardar()
end

local function limpiar()
  if m.duerme then return dormida() end
  if m.higiene >= 95 then
    avisar("Ya está limpia")
    cp.beep("error")
    return
  end
  m.higiene = 100
  m.animo = tope(m.animo + 3)
  mostrar("limpia")
  avisar("Quedó limpia")
  cp.beep("ok")
  guardar()
end

local function dormirDespertar()
  if m.duerme then
    m.duerme = false
    if m.energia < 50 then
      m.animo = tope(m.animo - 5)
      avisar("Se despertó de mal humor")
    else
      avisar("Buenos días")
    end
  else
    m.duerme = true
    avisar("Se durmió")
  end
  cp.beep("ok")
  guardar()
end

local function empezarJuego()
  if m.duerme then return dormida() end
  if m.energia < 15 then
    avisar("Está agotada: que duerma")
    cp.beep("error")
    return
  end
  if m.hambre < 10 then
    avisar("Tiene demasiada hambre para jugar")
    cp.beep("error")
    return
  end
  juego = {n = math.random(1, 9), ronda = 1, aciertos = 0, ultimo = nil, fin = false}
  pantalla = "juego"
  cp.beep("ok")
end

local function jugada(k)
  if juego.fin then return end
  local sig = math.random(1, 8)
  if sig >= juego.n then sig = sig + 1 end   -- nunca el mismo número
  local acierto = (k == "up" and sig > juego.n) or (k == "down" and sig < juego.n)
  juego.ultimo = {n = juego.n, sig = sig, acierto = acierto}
  if acierto then juego.aciertos = juego.aciertos + 1 end
  cp.beep(acierto and "ok" or "error")
  juego.n = sig
  juego.ronda = juego.ronda + 1
  if juego.ronda > RONDAS then
    juego.fin = true
    m.animo = tope(m.animo + 8 + 8 * juego.aciertos)
    m.energia = tope(m.energia - 12)
    m.hambre = tope(m.hambre - 5)
    guardar()
  end
end

local function huevoNuevo()
  m = nil
  guardar()
  pantalla = "nombre"
  filaNombre = 1
  nombreMsg = nil
  cuadroMs = cp.ms()
  cp.beep("ok")
end

local function gesto()
  local g = cp.motion()
  if not g or not m or not m.viva or pantalla == "nombre" then return end
  g = string.lower(tostring(g))
  if g == "shake" then
    if m.duerme then
      m.duerme = false
      m.animo = tope(m.animo - 10)
      avisar("¡Se despertó de golpe!")
    else
      m.animo = tope(m.animo - 5)
      avisar("No le gusta que la sacudan")
    end
    if pantalla == "juego" then pantalla = "casa" end
    cp.beep("error")
  elseif g == "facedown" then
    if not m.duerme then
      m.duerme = true
      avisar("Se durmió")
      if pantalla == "juego" then pantalla = "casa" end
    end
  end
end

-- ================================================================== callbacks
function on_open()
  math.randomseed(cp.ms())
  ultimoMs = cp.ms()
  ultimoEpoch = epochAhora()
  cuadroMs = ultimoMs
  guardadoMs = ultimoMs
  m = cargar()
  if not m then
    pantalla = "nombre"     -- la pantalla pregunta; no se bautiza sola
    filaNombre = 1
    nombreMsg = nil
    return
  end
  if m.viva then
    local ep = epochAhora()
    if ep and m.epoch and ep > m.epoch then
      local fuera = ep - m.epoch
      if fuera > TOPE_AUSENCIA then fuera = TOPE_AUSENCIA end
      pasar(fuera)
      if fuera >= HORA then avisar("Te esperó " .. math.floor(fuera / HORA) .. " h") end
      if ep then m.epoch = ep end
      guardar()   -- lo descontado queda anotado aunque se salga sin tocar nada
    elseif ep then
      m.epoch = ep
    end
  end
  pantalla = m.viva and "casa" or "sefue"
end

-- Lo que se ve, en un string: si no cambió, no se repinta.
local function firmaActual()
  if pantalla == "nombre" then
    return "nombre" .. cuadro .. filaNombre .. tostring(escuchando) .. (nombreMsg or "")
  end
  if not m then return "?" end
  if pantalla == "juego" then
    local u = juego.ultimo
    return "juego" .. juego.n .. juego.ronda .. juego.aciertos .. tostring(juego.fin)
      .. (u and (u.sig .. tostring(u.acierto)) or "")
  end
  return table.concat({
    pantalla, m.nombre, cuadroPerra(), estadoTexto(), aviso or "", accion or "", fila,
    math.floor(m.hambre), math.floor(m.animo), math.floor(m.energia), math.floor(m.higiene),
    math.floor(m.edad / 86400), m.duerme and "z" or "", tostring(escuchando),
  }, "|")
end

function on_tick()
  local ms = cp.ms()
  local ep = epochAhora()
  local dt = ultimoMs and (ms - ultimoMs) / 1000 or 0
  if ep and ultimoEpoch and ep - ultimoEpoch > dt then dt = ep - ultimoEpoch end
  ultimoMs = ms
  if ep then ultimoEpoch = ep end
  if dt < 0 then dt = 0 elseif dt > TOPE_AUSENCIA then dt = TOPE_AUSENCIA end

  if m and m.viva then
    pasar(dt)
    if ep then m.epoch = ep end
    if not m.viva then
      pantalla = "sefue"
      guardar()
    elseif ms - guardadoMs >= GUARDAR_MS then
      guardar()
    end
  end
  gesto()
  if ms - cuadroMs >= CUADRO_MS then
    cuadro = 1 - cuadro
    cuadroMs = ms
  end
  if aviso and ms >= avisoHasta then aviso = nil end
  if accion and ms >= accionHasta then accion = nil end

  local f = firmaActual()
  if f == firma then return false end
  firma = f
  return true
end

function on_key(k)
  if cp.busy() then return k == "back" end   -- Atrás corta la escucha
  if pantalla == "nombre" then
    if k == "up" or k == "down" then
      filaNombre = filaNombre == 1 and 2 or 1
      cp.beep("nav")
      return true
    elseif k == "ok" then
      if filaNombre == 1 then
        nombreMsg = nil
        escuchar()
      else
        nacer(NOMBRE_DEFAULT)
      end
      return true
    end
    return false            -- Atrás: se sale sin mascota; la próxima vez vuelve a preguntar
  end
  if pantalla == "sefue" then
    if k == "ok" then
      huevoNuevo()
      return true
    end
    return false
  end
  if pantalla == "info" then
    if k == "ok" then
      escuchar()            -- cambiar el nombre: llega por on_heard
      return true
    elseif k == "back" then
      pantalla = "casa"
      cp.beep("back")
      return true
    end
    return false
  end
  if pantalla == "juego" then
    if k == "back" or (k == "ok" and juego.fin) then
      if juego.fin then mostrar("juega") end
      pantalla = "casa"
      juego = nil
      cp.beep("back")
      return true
    elseif (k == "up" or k == "down") and not juego.fin then
      jugada(k)
      return true
    end
    return false
  end
  -- casa
  if k == "up" then
    fila = fila > 1 and fila - 1 or #FILAS
    cp.beep("nav")
    return true
  elseif k == "down" then
    fila = fila < #FILAS and fila + 1 or 1
    cp.beep("nav")
    return true
  elseif k == "ok" then
    local id = FILAS[fila].id
    if id == "comer" then comer()
    elseif id == "jugar" then empezarJuego()
    elseif id == "dormir" then dormirDespertar()
    elseif id == "limpiar" then limpiar()
    else
      pantalla = "info"
      cp.beep("ok")
    end
    return true
  elseif k == "back" then
    guardar()
    return false
  end
  return false
end

-- ===================================================================== pintar
-- Barra con etiqueta: marco de 1 px, relleno macizo (nunca trama bajo texto) y
-- el número a la derecha, alineado a la columna del metadato.
local function barra(y, etiqueta, valor)
  local x, w, h = 128, 256, 16
  local v = math.floor(valor)
  cp.text(MARGEN, y - 4, etiqueta, 10)
  cp.rect(x, y, w, h, false, 1)
  local lleno = v * (w - 4) // 100
  if lleno > 0 then cp.rect(x + 2, y + 2, lleno, h - 4, true) end
  local num = v .. " %"
  cp.text(W - MARGEN - cp.textw(num, 10), y - 4, num, 10)
end

-- La perra, centrada en su zona blanca, con lo que la acompaña (la "z Z" si
-- duerme, el icono de lo que acaba de pasar, la caca si está sucia).
local function dibujarPerra(g)
  local s = SPRITES[g]
  local pw, ph = s.w * ESCALA, s.h * ESCALA
  local px = (W - pw) // 2
  local py = ZONA_Y + (ZONA_H - ph) // 2
  dibujo(g, px, py, ESCALA)
  if m.duerme then
    cp.text(px + pw + 4, py + 8, "z", 12)
    cp.text(px + pw + 16, py - 14, "Z", 14, true)
  end
  local iconoY = py + ph - 48
  local iconoX = px + pw + 8
  if iconoX + 48 > W - MARGEN then iconoX = W - MARGEN - 48 end
  if accion == "come" then dibujo("comida", iconoX, iconoY, 3)
  elseif accion == "limpia" then dibujo("balde", iconoX, iconoY, 3)
  elseif accion == "juega" then dibujo("pelota", iconoX, iconoY, 3)
  end
  if m.viva and m.higiene < 25 then dibujo("caca", math.max(MARGEN, px - 56), iconoY, 3) end
end

local function dibujarCasa()
  cabezal(m.nombre, "Día " .. (math.floor(m.edad / 86400) + 1))
  dibujarPerra(cuadroPerra())
  centrado(ESTADO_Y, estadoTexto(), 12, true)
  if aviso then centrado(AVISO_Y, aviso, 10) end

  barra(BARRAS_Y, "Hambre", m.hambre)
  barra(BARRAS_Y + BARRA_PASO, "Ánimo", m.animo)
  barra(BARRAS_Y + 2 * BARRA_PASO, "Energía", m.energia)
  barra(BARRAS_Y + 3 * BARRA_PASO, "Higiene", m.higiene)

  for i, f in ipairs(FILAS) do
    local texto, meta = f.texto, f.meta
    if f.id == "dormir" and m.duerme then texto, meta = "Despertar", "mejor con energía" end
    filaLista(LISTA_Y + (i - 1) * FILA_H, i == fila, texto, meta)
  end
  ayuda("Palanca: elegir · OK: hacer · Atrás: salir")
end

local function dibujarNombre()
  cabezal("¿Cómo se llama tu perrita?")
  dibujo(cuadro == 0 and "huevo1" or "huevo2", (W - 32 * 6) // 2, 112, 6)
  if escuchando then
    centrado(ESTADO_Y, "Di el nombre…", 12, true)
  elseif nombreMsg then
    centrado(ESTADO_Y, nombreMsg, 12, true)
  else
    centrado(ESTADO_Y, "Un huevo nuevo", 12, true)
  end
  centrado(AVISO_Y, "Se toma la primera palabra que digas.", 10)
  filaLista(LISTA_CORTA_Y, filaNombre == 1, "Decir el nombre", "por el micrófono")
  filaLista(LISTA_CORTA_Y + FILA_H, filaNombre == 2, "Se llama " .. NOMBRE_DEFAULT, "sin hablar")
  ayuda("Palanca: elegir · OK: hacer · Atrás: salir")
end

-- Un dígito grande de siete segmentos para el juego (a, b, c, d, e, f, g).
local SEG = {
  [0] = {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
  {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1},
}
local function digito(x, y, w, h, n, g)
  local s = SEG[n]
  if not s then return end
  local medio = y + h // 2
  if s[1] == 1 then cp.rect(x, y, w, g, true) end
  if s[2] == 1 then cp.rect(x + w - g, y, g, h // 2, true) end
  if s[3] == 1 then cp.rect(x + w - g, medio, g, h // 2, true) end
  if s[4] == 1 then cp.rect(x, y + h - g, w, g, true) end
  if s[5] == 1 then cp.rect(x, medio, g, h // 2, true) end
  if s[6] == 1 then cp.rect(x, y, g, h // 2, true) end
  if s[7] == 1 then cp.rect(x, medio - g // 2, w, g, true) end
end

local function dibujarJuego()
  cabezal("Mayor o menor", "Ronda " .. math.min(juego.ronda, RONDAS) .. " de " .. RONDAS)
  digito((W - 90) // 2, 160, 90, 150, juego.n, 14)
  local u = juego.ultimo
  if u then
    centrado(350, "Salió " .. u.sig .. ": " .. (u.acierto and "¡acierto!" or "no era"), 12, true)
  end
  centrado(390, "Aciertos: " .. juego.aciertos, 12)
  if juego.fin then
    centrado(460, m.nombre .. " se divirtió", 14, true)
    centrado(500, juego.aciertos .. " de " .. RONDAS, 12)
    ayuda("OK: volver")
  else
    centrado(460, "¿El próximo número será mayor o menor?", 12)
    ayuda("ARRIBA: mayor · ABAJO: menor · Atrás: dejar de jugar")
  end
end

local function dibujarInfo()
  cabezal(m.nombre, "Día " .. (math.floor(m.edad / 86400) + 1))
  local dias = math.floor(m.edad / 86400)
  local horas = math.floor((m.edad - dias * 86400) / HORA)
  local y = 104
  local function dato(etiqueta, valor)
    cp.text(MARGEN, y, etiqueta, 12)
    derecha(y, valor, 12)
    y = y + 32
  end
  dato("Edad", dias .. (dias == 1 and " día y " or " días y ") .. horas .. " h")
  dato("Hambre", math.floor(m.hambre) .. " %")
  dato("Ánimo", math.floor(m.animo) .. " %")
  dato("Energía", math.floor(m.energia) .. " %")
  dato("Higiene", math.floor(m.higiene) .. " %")
  dato("Duerme", m.duerme and "sí" or "no")
  dato("Descuido", math.floor(m.descuido / HORA) .. " h de " .. (SE_VA_TRAS // HORA))
  y = y + 8
  cp.text(MARGEN, y, "Cómo cuidarla", 14, true)
  y = y + 32
  cp.text(MARGEN, y, "Las barras bajan con el tiempo, también con la app cerrada.", 10)
  y = y + 24
  cp.text(MARGEN, y, "Con hambre o sucia mucho tiempo, se va.", 10)
  y = y + 24
  cp.text(MARGEN, y, "Sacudir la despierta de golpe; boca abajo se duerme.", 10)
  y = y + 32
  cp.text(MARGEN, y, "Dibujos: siluetas CC0 de openclipart.org (eevee93 y", 10)
  y = y + 24
  cp.text(MARGEN, y, "f_featherbrain); iconos de OpenCritter (SuperMechaCow, MIT).", 10)

  local yFila = AYUDA_Y - 8 - FILA_H - 32
  if escuchando then
    centrado(yFila - 32, "Di el nombre…", 12, true)
  elseif aviso then
    centrado(yFila - 32, aviso, 12, true)
  end
  filaLista(yFila, true, "Cambiar el nombre", "por el micrófono")
  ayuda("OK: cambiar el nombre · Atrás: volver")
end

local function dibujarSeFue()
  cabezal(m.nombre .. " se fue", "Día " .. (math.floor(m.edad / 86400) + 1))
  dibujarPerra("sefue")
  local dias = math.floor(m.edad / 86400)
  centrado(ESTADO_Y, "Vivió " .. dias .. (dias == 1 and " día" or " días"), 12, true)
  centrado(AVISO_Y, "Pasó demasiado tiempo con hambre o sucia.", 10)
  filaLista(LISTA_CORTA_Y, true, "Un huevo nuevo", "empezar de cero")
  ayuda("OK: un huevo nuevo · Atrás: salir")
end

function on_draw()
  firma = firmaActual()
  if pantalla == "nombre" then
    dibujarNombre()
  elseif not m then
    centrado(300, "Sin mascota", 12)
  elseif pantalla == "juego" then
    dibujarJuego()
  elseif pantalla == "info" then
    dibujarInfo()
  elseif pantalla == "sefue" then
    dibujarSeFue()
  else
    dibujarCasa()
  end
end
