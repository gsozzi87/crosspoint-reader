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
-- DIBUJOS: no son nuestros. Son dibujos de LÍNEA de dominio público (CC0,
-- https://creativecommons.org/publicdomain/zero/1.0/) bajados de
-- openclipart.org e iconos MIT; los detalles están en docs/ws397/MASCOTA.md:
--   * La perrita parada (cuerpo + las cuatro colas, y sefue): "Dachshund", de
--     oksmith, https://openclipart.org/detail/315296/dachshund (CC0). Es una
--     caricatura con contorno: el SVG trae por cada forma un camino relleno y
--     otro sólo de trazo, así que poniendo los rellenos en BLANCO y dejando los
--     trazos negros queda el contorno con el interior blanco. La cola no es un
--     dibujo nuevo: es la misma, DOBLADA alrededor de su raíz (arriba =
--     contenta, abajo = triste), y la base no se mueve ni un píxel.
--   * La perrita dormida (duerme): "Dog dreams", de liftarn,
--     https://openclipart.org/detail/329276/dog-dreams (CC0). Los huesitos
--     sueltos del original se fueron: queda la componente conexa más grande.
--   * Los iconos (comida, pelota, balde, caca) y el huevo: proyecto OpenCritter
--     de SuperMechaCow, licencia MIT, Copyright (c) 2017 SuperMechaCow.
--     https://github.com/SuperMechaCow/OpenCritter (gfx/bitmaps).
-- La conversión la hace gen_final.py (resvg + Pillow), ver MASCOTA.md: reducción
-- al ancho final, umbral y empaquetado MSB primero, 1 = tinta, como pide
-- cp.image(), que los pinta a escala 2 (el cuerpo mide 374 x 206 en el vidrio).

-- Generado por gen_final.py (scratchpad): NO editar a mano.
local SPRITES = {
  cuerpo = {w = 187, h = 103, hex = "0000000ffff80000000000000000000000000000000000000000007fffff800000000000000000000000000000000000000001fff03ff00000000000000000000000000000000000000007ef0001fc000000000000000000000000000000000000000f1e00001f000000000000000000000000000000000000003e38000007800000000000000000000000000000000000007870000001e0000000000000000000000000000000000000f0e0000000f0000000000000000000000000000000000001c1c0000000700000000000000000000000000000000000038380000000380000000000000000000000000000000000070f380006001c00000000000000000000000000000000000e1e78000e000e00000000000000000000000000000000007ffc30000600060000000000000000000000000000000001fff000018600070000000000000000000000000000000007ffc00003c60003000000000000000000000000000000000f87000003c60003800000000000000000000000000000001f03000003860001800000000000000000000000000000003f03000000060001800000000000000000000000000000003b83000000060001c00000000000000000000000000000007387000000060001c000000000000000000000000000000061fe0000000e0000c0000000000000000000000000000000e0fc0000000e0000c0000000000000000000000000000000e0000000000c0000c0000000000000000000000000000000c0000000001c0000c0000000000000000000000000000000c0000000001c0000c0000000000000000000000000000000c000000000180000c0000000000000000000000000000000c000000000380000c0000000000000000000000000000000c000000000300000c0000000000000000000000000000000c00000c000700000c0000000000000000000000000000000c00000c000e00001c0000000000000000000000000000000e00000c000e00001c0000000000000000000000000000000600000c000c0000180000000000000000000000000000000700001c001c0000180000000000000000000000000000000380001c001800003800000000000000000000000000000003c00018003800003000000000000000000000000000000001e00038003000007000000000000000000000000000000000f800700030000070000000000000000000000000000000003f00e00070000060000000000000000000000000000000000fffe000700000f0000000000000000007fff8000000000003fffe00600000f80000000000000007ffffff8000000000003e3fffe00001dc00000000000003ffffc07fc00000000000000fffe00001ce000000000001ffff800001f00000000000000073e0000387f000000001ffff8000000078000000000000007070000703ffffffffffffe0000000001c000000000000003070000e007fffffffffe000000000000e000000000000003030001e00007ffffe0000000000000007000000000000003038003c0000000000000000000000000380000000000000381e00f00000000000000000000000000380000000000000380f87e000000000000000000000000001c00000000000003807ff8000000000000000000000000070c00000000000003801fe0000000000000000000000000070e00000000000003800000000000000000000000000000070700000000000003800000000000000000000000000000070000000000000003800000000000000000000000000000030000000000000003800000000000000000000000000000030000000000000003800000000000000000000000000000030000000000000001800000000000000000000000000000030000000000000001800000000000000000000000000000030000000000000001800000000000000000000000000000030000000000000001c00000000000000000000000000000030000000000000001c00000000000000000000000000000030000000000000000c00000000000000000000000000000010000000000000000e00000000000000000000000000000010000000000000000e000000000000000000000000000000100000000000000006000000000000000000000000000000000000000000000007000000000000000000000000000000000000000000000003800000000000000000000000000000000000000000000003800000000000000000000000000000000000000000000001c00000000000000000000000000000000000000000000001e00000000000000000000000000000000000000000000000f00000000000000000000000000000000000000000000000f00000000000000000000000000000000000000000000000f800000000000000000000000000000000000000000000007c00000000000000000000000000000000000000000000007e00000000000000000001000000000000000000000000007f0000000000000000001f8000000000000000000000000037000e00000000000001ffe000000000000000000000000033800f0000000000001ff8f00000000000000000000000003b800fc00000000000ff807800000000000000000000000039801df8000000000ff8001c00000000000000000000000039c0187f00000003ffc0000f000000000000000000000001f9c0181ffffffffffe000007800007e0000000000000000ff9c03803ffffffffc0000003e0003ff0000000000000003ff8c030000007ff0000000000f800fc70000000000000007838c0300000000000000000007fcff00e03800000000000fe10c070000000000000000000efff800701800000000000fe00c070000000000000000000e1fe000701c00000000001e001c07000000000000000003fc070000301c00000000001c007c0700000000000000000ffc0e0000380e000000000019e1fc0700000000000000001e3c0c0000f80e00000000001ff3cc0300000000000000003fb81c0003f80600000000001f37e00380000000000000007f80180007dc0700000000000e0ff0038000000000000000700038000ffc0700000000000f0fe0018000000000000000ef0030000fe003000000000007ff00018000000000000000ff0030001f0003800000000003fe38038000000000000000ff0070001cc003800000000000fef8070000000000000000f00070001bf0038000000000000ef00f00000000000000007001e0001ff00700000000000007c03e00000000000000007fffc0001e000f0000000000000781f800000000000000003fff80001e003e00000000000003ffe0000000000000000007f800000f01f800000000000000ff8000000000000000000000000007ffe000000000000000000000000000000000000000000003ff800000"},
  cola_quieta1 = {w = 27, h = 32, hex = "07000000038000008380000081c0000080c00000c0e00000c0e00000e0700000e0300000f0380000f01c0000f81c0000fc0e0000ee0700007787800073c3c0003df1e0001e78f0000f3f7800038ffe0001c3ff8001c07fc000e00fe000e0000000600000006000000060000000700000007000000070000080300000c0300000"},
  cola_quieta2 = {w = 27, h = 32, hex = "07000000038000008380000081c0000080c00000c0e00000c0700000e0780000e0380000f01e0000f80f0000fc078000ef03e000e780fe0071f03fe070ff07e03c1fffe01e03ffe00f0000000380000001c0000001c0000000e0000000e0000000600000006000000060000000700000007000000070000080300000c0300000"},
  cola_feliz = {w = 27, h = 32, hex = "07000000038000208380006081c001e080f003e0c0fc1f60c03ffdc0e00fe380e0000700f0001c00be00f000cfffc000e1fe0000e000000070000000700000003c0000001e0000000f0000000380000001c0000001c0000000e0000000e0000000600000006000000060000000700000007000000070000080300000c0300000"},
  cola_triste = {w = 27, h = 32, hex = "07000000038000008380000081c0000080c00000c0c00000c0e00000e0e00000e0600000e0600000f0600000f0700000f8700000f83000007c3000007c3000003e3000001e3000000f38000007b8000003d8000003d8000001f8000001f8000000f8000000f80000007c0000007c0000007e0000007f000080370000c0300000"},
  duerme = {w = 180, h = 95, hex = "00000000000000000000000000000003ff000000000000000000000000000000000000000000020fc0000000000000000000000000000007f8000000000003e000000000000000000000000000000e1e000000000001b000000000000000000000000000003807000000000000d800000000000000000000000000007001c00000000000cc0000000000000000000000000000e000c00000000000440000000000000000000000000001c0006000000000006600000000000000000000000000030000600000000000660000000000000000000000000003001fe000000000006200000000000000000000000000060078e0000000000062000000000000000000000000000c00e0600000000000630000000000000000000000000018018060000000000063000000000000000000000000007003006000000000006300000000000000000000000001e00600c003e00000006300000000000000000000000007800c01c01ff8000000620000000000000000000000003e00180380700e00000066000000000000000000000000f000300e01c007000000660000000000000000000000038000601c0300030000006600000000000000000000000e0001c1f00600018000004600000000000000003000001800070f801c0001800000cc0004000000000000fe00003000fffc003800018000018c003e000000000000c70000600ffe000060000700000398003300000000000083e000403c000000c000ff00000738006180000000000181f80040700000038007c3001ffe7003c1800000000000c19e3ff8c000000f001e0600fc78e07fc1e00000000000c307fe7f800001fc00780e03c003fff981fc000000000063000003e003ffe000e00fff000f7801810e00000000003e00000071ffc00003803ffc00181c0183038000000000300000001f8000000600e07000000e030301c000000000100000000e0000001c07c1e0000006030600e000000000100000001c00000070fe078000000306060030000000001000000018000003fff01e000000038c0c00180000000018000000307ffffffe00f0000000019818001800000001f800000031ffff800007c000000000f030000c0000ffffc8000000079bfc0001fe0000000000c060000e000fe0000c00000006187fffffe00000000000c0c00006003c0000060000000c10c03f800000000000004381f80780600000060000000830c0000000000000000067038c03c0400000030020000030c000000000000000007c0e0c02e0c00000018060000061800000000000000000781c0c0260c000000181c000006180000000000000000060700c063080000000ff000000c300000000000000000060e00c0630800000007e000001830000000000000000004380180630800000003000000186000000000000000008cf0038043080000000180000030600000000000000000ffc00300c30807800000c0000060c000000000000000003c000e00c2080fe00000600000c180000000000000000000001c01860c0ff80000f00001c1800000000000000000000030030c0c0fdf0003d80003030000000000000000000000e0061c0c0e03fffe0e0006060000000000000000000003c00c3c0c07f01f8003000c0c000000000000000018000f00183c0407e0000001c0181800000000000000000f807c00306c040380000000f0703000000000007fff0003ffe000e0cc0601e0000003fce060000000001ffc7ff801c00001c18c06007c00000feff0c000000001fc00001f83800007030c07001fe001fdf3f9802000000f80000003e700000e0608058001ffffe19b1f006000007c000000007e0000380c080580000000079e0e00606001e0000000001c0000e0180804c00000000d9f1f00c060070000000000700003c0701804600000000803f1f880600c0000000000e0000f00e01804700000000c41ff3f8040380000000003c0001c03c010043e00000007f03f8300c060000000000f0000780f8030040fe00000031c018600e1c0000000007c0001e01e00200401f800000007838c01ff8000000007e0000fc078006006001e00000001cf18010fffc0001fff00003e01e00040030007c0000000c63003001fffffffe00003f00f8001c0018000ff000000c66006000000000000003f003c00038000c0003fff00007fc00c00000000000001f800f0001f80006000007ff8003f80180000000000001f800380007f000038000000ff801e0018000000000003f8001e0000c600001e00000003fff800300000000000ff8001f80000cc000007800000001fc000e0000000003fe0001fc0000098000001fc0000007c0001c00000000ff80001fc000000b00000007fc00000c0000300000001fe00001fc0000000e000000030ff8001f0000ff00003ffc00003fc00000000c00000001847fc01e0003c7fffffc000003f80000000018000000008431ff18e00f000ffc0000003f80000000007000000000c6601fffc07c00000000000ff80000000000e00000000043c0000f83e00000000007ff000000000001800000000060000003fffff0000003ff800000000000030000000000300000000001ffff0fffc00000000000000e0000000000180000000000007fffe0000000000000001c00000000000e000000000000000000000000000000007800000000000300000000000000000000000000000000f0000000000001c0000000000000000000000000000001c00000000000007000000000000000000000000000000f000000000000001c00000000000000000000000000303c000000000000000780000000003800000000000000ffe00000000000000000f0000000007e000000000000018f8000000000000000001f00000000c300000000000003fc00000000000000000003f0000000c3000000000001ffe0000000000000000000003f00000181007fc07ffffff0000000000000000000000003fff80787ffffffffff80000000000000000000000000000ffffffffe000000000000000000000000"},
  sefue = {w = 140, h = 78, hex = "000000000000000000000000007fff00000000000000000000000000000003ffffc000000000000000000000000000001fe03ff000000000000000000000000000003e000f7800000000000000000000000000007800039e0000000000000000000000000001e00001cf0000000000000000000000000001c00000e3800000000000000000000000000380000171c000000000000000000000000007006003bce00000000000000000000000000e0060019ffc0000000000000000000000000c00e60007ff0000000000000000000000001c00ef0000ff8000000000000000000000001800e60000c3c000000000000000000000001800e00001c3e000000000000000000000003800e00000c7e000000000000000000000003800600000fe70000000000000000000000030006000007c30000000000000000000000030007000000030000000000000000000000030007000000030000000000000000000000030003000000030000000000000000000000030003800000030000000000000000000000038001800c00030000000000000000000000038001c00c00030000000000000000000000018000c00c00030000000000000000000000018000e00e0007000000000000000000000001c0006006000e000000000000000000000000c0007007001c000000000000000000000000c00030030078000000003000000000000000e0003003c3f00000001ffff8000000000000e0003807ffc00000007ffffff00000000001f0003fffff00000000f8003fffe000000007b0003ffe0000000001c000003ffffe0007ff38003ec00000000003800000007ffffffffc1c0030c0000000000700000000003fffff800e0070c0000000000e0000000000000000000780e1c0000000000c00000000000000000003e1e1c0000000001cc0000000000000000000ffc1c00000000018c00000000000000000003f01c00000000038c00000000000000000000001c00000000030c00000000000000000000001c00000000071c00000000000000000000001c00000000061c000000000000000000000018000000000e1c000000000000000000000018000000000c3c000000000000000000000018000000001c3c0000000000000000000000180000000038780000000000000000000000380000000030780000000000000000000000300000000070f800000000000000000000007000000000e1f000000000000000000000006000000001c7f00000000000000000000000e0000000038fe00000000000000000000000c000000007bfe00000000000000000000001c00000000ffb800000000000000000000003800000003fe700000000000000000000000780000000ff8e00000000000000000000000f80000000fc0e00000001800000000000001f00000000000c00000007f80000000000401f00000000000c0000000fff8000000001c03b00000000001c0000001e0ff000000007e03f00000000001c0000003801ff8000003f603700000000001807c000f0001ffffffffc6037c000000000181ff003e00003ffffffe07077f800000000183cfe1f80000001ff80003077fc0000000038701fff8000000000000030773e00000000306007f9c000000000000030703f00000000706000e1fe0000000000003078070000000060f00070ff000000000000307e3b00000000e0fc0030ff800000000000307f7f00000000e0fe00381fc00000000000701fef00000000c0ff00180fc00000000000601f8e00000000c03f00180fc000000000006003fc00000001c07f00180fc000000000007039fc00000000c0ff001e01c00000000000383fb000000000f04f000fffc000000000001e0f80000000007c070007ff8000000000000fff00000000003ffe00007c00000000000003fe00000000000ffc000000000000000000007800000000"},
  comida = {w = 16, h = 16, hex = "1ff82084400288218001ffff400240023f0240fc40024002ffff800140023ffc"},
  pelota = {w = 16, h = 16, hex = "000007e018182424224442424422481248124812442223c42004181807e00000"},
  balde = {w = 16, h = 16, hex = "00001ff8200440028001c003a0059ff980014002400240024002200420041ff8"},
  caca = {w = 16, h = 16, hex = "000005200220507020f001f003f007e00f901f783efc39fc3ffc3ffc1ff80000"},
  huevo1 = {w = 32, h = 32, hex = "00000000000000000000000000000000000fe000001830000030180000600c0000c00600018003000300018002000080060000c0040000400c0000600800002008000020080000201800003010000010100000101000003010000150180000b008000560080002a004000540030015800182ab0000c95600007abc00000fe000"},
  huevo2 = {w = 32, h = 32, hex = "000fe000001830000030180000600c0000c0060000800200018003000100010003000180060000c004000040040000400c000060080000200800002008000020180000301000003010000010100000501000003010000150180000b008000560080002a00c00056006002ac0030015800182ab0000c95600007abc00000fe000"},
}
-- La cola va pegada sobre el cuerpo, en esta posición (en píxeles del bitmap).
local COLA_X, COLA_Y = 160, 52

-- ================================================================== constantes
local HORA = 3600
local TOPE_AUSENCIA = 12 * HORA   -- al volver se descuenta como mucho medio día
local SE_VA_TRAS = 36 * HORA      -- segundos seguidos con una necesidad en cero
local CUADRO_MS = 4000            -- la animación cambia de cuadro cada 4 s
local AVISO_MS = 4000             -- cuánto dura un aviso en pantalla
local GUARDAR_MS = 5 * 60 * 1000  -- guardado de fondo, además de cada acción
local NOMBRE_DEFAULT = "Pipo"
local RONDAS = 3
local ESCALA = 2                  -- el cuerpo: 187 px de ancho → 374 en el vidrio

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
--
-- Los cuatro cuadros de pie son el MISMO cuerpo con otra cola: se pinta el
-- cuerpo (que tiene el hueco de la cola en blanco) y encima el parche de la
-- cola que toca. cp.image no pinta los ceros, así que uno no borra al otro, y
-- las cuatro colas juntas pesan 512 bytes en vez de cuatro cuerpos enteros.
local function dibujarPerra(g)
  local base = SPRITES[g] and g or "cuerpo"
  local s = SPRITES[base]
  local pw, ph = s.w * ESCALA, s.h * ESCALA
  local px = (W - pw) // 2
  local py = ZONA_Y + (ZONA_H - ph) // 2
  dibujo(base, px, py, ESCALA)
  if base == "cuerpo" then
    dibujo("cola_" .. g, px + COLA_X * ESCALA, py + COLA_Y * ESCALA, ESCALA)
  end
  -- La perra ocupa casi toda la zona, así que lo que la acompaña va en las
  -- esquinas de arriba, que es donde el dibujo tiene blanco: la "z Z" o el
  -- icono de lo que acaba de pasar a la derecha, la caca a la izquierda.
  local iconoY = ZONA_Y
  if m.duerme then
    cp.text(W - MARGEN - 40, iconoY + 22, "z", 12)
    cp.text(W - MARGEN - 26, iconoY, "Z", 14, true)
  elseif accion == "come" then dibujo("comida", W - MARGEN - 48, iconoY, 3)
  elseif accion == "limpia" then dibujo("balde", W - MARGEN - 48, iconoY, 3)
  elseif accion == "juega" then dibujo("pelota", W - MARGEN - 48, iconoY, 3)
  end
  if m.viva and m.higiene < 25 then dibujo("caca", MARGEN, iconoY, 3) end
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
  cp.text(MARGEN, y, "Dibujos: CC0 de openclipart.org (oksmith y liftarn);", 10)
  y = y + 24
  cp.text(MARGEN, y, "iconos de OpenCritter (SuperMechaCow, MIT).", 10)

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
