-- Mascota: una mascota de bolsillo que vive en la tarjeta.
--
-- Una perrita salchicha de tinta. Un tamagotchi básico: cuatro necesidades (hambre, ánimo, energía e higiene)
-- que bajan con el tiempo REAL —también mientras la app está cerrada— y cinco
-- acciones en la botonera de abajo: Alimentar, Jugar (mayor o menor, tres
-- rondas), Dormir/Despertar, Limpiar e Info. Se le pone nombre por voz la
-- primera vez. Si se la descuida mucho tiempo, se va; OK trae un huevo nuevo.
--
-- Gestos: sacudir el aparato la despierta de golpe (y se pone de mal humor);
-- boca abajo la manda a dormir.
--
-- Cuándo repinta: on_tick devuelve true SÓLO cuando cambió algo que se ve
-- (el cuadro de la animación cada 4 s, un renglón de las barras, un aviso que
-- vence). Devolver true en cada tick sería un refresco parcial cada 120 ms.
--
-- DIBUJOS: no son nuestros, son de dos conjuntos abiertos bajados de la web
-- (los detalles y los enlaces están en docs/ws397/MASCOTA.md):
--   * La perrita salchicha: pets/dachshund/sprites/{idle,walk,sleep}.png del
--     proyecto openclaw-tamagotchi de Artem Katolikov, licencia MIT
--     (Copyright (c) 2026 Artem Katolikov).
--     https://github.com/katolikov/openclaw-tamagotchi
--     Celdas de 32 x 32 recortadas a 32 x 16 y pasadas a 1 bit por paleta
--     (cuerpo y patas = tinta, el ojo = blanco); acá se dibujan a escala 7.
--     "Se fue" es el cuadro quieto dado vuelta, patas para arriba.
--   * Los iconos (comida, pelota, balde, caca, info) y el huevo: proyecto
--     OpenCritter de SuperMechaCow, licencia MIT.
--     https://github.com/SuperMechaCow/OpenCritter (gfx/bitmaps)
--     Copyright (c) 2017 SuperMechaCow. Se usan tal cual (16x16 y 32x32).
-- La conversión la hace convert.py (Pillow): recorte, paleta y empaquetado
-- MSB primero, 1 = tinta, como pide cp.image().

-- Generado por convert.py (scratchpad): NO editar a mano.
local SPRITES = {
  idle1 = {w = 32, h = 16, hex = "0000000030000000180000000c000ff00ffffff80ffffdf81ffffff81ffffff80ffffff006c06c0006c06c000000000000000000000000000000000000000000"},
  idle2 = {w = 32, h = 16, hex = "000000000000000030000000180000000c000ff00ffffff80ffffdf81ffffff81ffffff80ffffff006c06c0006c06c0000000000000000000000000000000000"},
  feliz1 = {w = 32, h = 16, hex = "0000000030000000180000000c000ff00ffffff80ffffdf81ffffff81ffffff80ffffff006c06c0006c06c000000000000000000000000000000000000000000"},
  feliz2 = {w = 32, h = 16, hex = "00000000600000003000000018001fe01ffffff01ffffbf03ffffff03ffffff01fffffe00d80d8000d80d8000000000000000000000000000000000000000000"},
  triste1 = {w = 32, h = 16, hex = "00000000000000000000000030000000180000000c000ff00ffffff80ffffff81ffffff81ffffff80ffffff006c06c0006c06c00000000000000000000000000"},
  triste2 = {w = 32, h = 16, hex = "0000000000000000000000000000000030000000180000000c000ff00ffffff80ffffff81ffffff81ffffff80ffffff006c06c0006c06c000000000000000000"},
  duerme = {w = 32, h = 16, hex = "00000000000000000000000030000000180000000c000ff00ffffff80ffffff81ffffff81ffffff80ffffff006c06c0006c06c00000000000000000000000000"},
  sefue = {w = 32, h = 16, hex = "000000000000000000000000000000000000000006c06c0006c06c000ffffff01ffffff81ffffff80ffffdf80ffffff80c000ff0180000003000000000000000"},
  comida = {w = 16, h = 16, hex = "1ff82084400288218001ffff400240023f0240fc40024002ffff800140023ffc"},
  pelota = {w = 16, h = 16, hex = "000007e018182424224442424422481248124812442223c42004181807e00000"},
  balde = {w = 16, h = 16, hex = "00001ff8200440028001c003a0059ff980014002400240024002200420041ff8"},
  caca = {w = 16, h = 16, hex = "000005200220507020f001f003f007e00f901f783efc39fc3ffc3ffc1ff80000"},
  info = {w = 16, h = 16, hex = "00007ffe7ffe7ffe000000007ffe7c027ffe000000007ffe7fe27ffe00000000"},
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
local ESCALA = 7                  -- la perra: 32 x 16 → 224 x 112 px

-- ====================================================================== estado
local m = nil                -- la mascota (ver nacer())
local pantalla = "nombre"    -- "nombre" | "casa" | "juego" | "info" | "sefue"
local boton = 1
local aviso, avisoHasta = nil, 0
local accion, accionHasta = nil, 0   -- "come" | "limpia" | "juega"
local cuadro, cuadroMs = 0, 0        -- 0/1: alterna cada CUADRO_MS
local ultimoMs, ultimoEpoch = nil, nil
local guardadoMs = 0
local firma = nil                    -- lo último que se pintó
local juego = nil
local escuchando = false

local BOTONES = {
  {id = "comer",   texto = "Alimentar", icono = "comida"},
  {id = "jugar",   texto = "Jugar",     icono = "pelota"},
  {id = "dormir",  texto = "Dormir",    icono = nil},
  {id = "limpiar", texto = "Limpiar",   icono = "balde"},
  {id = "info",    texto = "Info",      icono = "info"},
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
  cp.text((cp.width() - cp.textw(s, tam or 12)) // 2, y, s, tam or 12, negrita)
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
  boton = 1
  escuchando = false
  avisar("¡Hola, " .. nombre .. "!")
  cp.beep("ok")
  guardar()
end

local function escuchar()
  escuchando = cp.listen(6, "¿Cómo se llama?") and true or false
end

function on_heard(texto)
  escuchando = false
  if pantalla ~= "nombre" then return end
  nacer(limpiarNombre(texto) or NOMBRE_DEFAULT)
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
    return cuadro == 0 and "feliz1" or "feliz2"
  end
  if m.hambre < 25 or m.animo < 25 or m.higiene < 25 or m.energia < 20 then
    return cuadro == 0 and "triste1" or "triste2"
  end
  return cuadro == 0 and "idle1" or "idle2"
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
  cuadroMs = cp.ms()
  cp.beep("ok")
  escuchar()
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
    pantalla = "nombre"
    escuchar()
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
  if pantalla == "nombre" then return "nombre" .. cuadro .. tostring(escuchando) end
  if not m then return "?" end
  if pantalla == "juego" then
    local u = juego.ultimo
    return "juego" .. juego.n .. juego.ronda .. juego.aciertos .. tostring(juego.fin)
      .. (u and (u.sig .. tostring(u.acierto)) or "")
  end
  return table.concat({
    pantalla, cuadroPerra(), estadoTexto(), aviso or "", accion or "", boton,
    math.floor(m.hambre), math.floor(m.animo), math.floor(m.energia), math.floor(m.higiene),
    math.floor(m.edad / 86400), m.duerme and "z" or "",
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
    if k == "ok" then
      escuchar()
      return true
    elseif k == "back" then
      nacer(NOMBRE_DEFAULT)
      return true
    end
    return false
  end
  if pantalla == "sefue" then
    if k == "ok" then
      huevoNuevo()
      return true
    end
    return false
  end
  if pantalla == "info" then
    if k == "back" or k == "ok" then
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
    boton = boton > 1 and boton - 1 or #BOTONES
    cp.beep("nav")
    return true
  elseif k == "down" then
    boton = boton < #BOTONES and boton + 1 or 1
    cp.beep("nav")
    return true
  elseif k == "ok" then
    local id = BOTONES[boton].id
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
local function barra(y, etiqueta, valor)
  local x, w, h = 150, 280, 18
  local v = math.floor(valor)
  cp.text(24, y + 2, etiqueta, 10)
  cp.rect(x, y, w, h, false, 1)
  local lleno = v * (w - 4) // 100
  if lleno > 0 then cp.rect(x + 2, y + 2, lleno, h - 4, true) end
  local num = v .. " %"
  cp.text(x + w + 8, y + 2, num, 10)
end

local function dibujarBotones()
  local y, h = 640, 72
  local cw = (cp.width() - 48) // #BOTONES
  local x0 = (cp.width() - cw * #BOTONES) // 2
  for i, b in ipairs(BOTONES) do
    local x = x0 + (i - 1) * cw
    if i == boton then cp.selection(x, y, cw, h) else cp.rect(x, y, cw, h, false, 1) end
    local texto = b.texto
    if b.id == "dormir" then
      texto = m.duerme and "Despertar" or "Dormir"
      local zz = m.duerme and "Zz" or "zZ"
      cp.text(x + (cw - cp.textw(zz, 14)) // 2, y + 12, zz, 14, true)
    elseif b.icono then
      dibujo(b.icono, x + (cw - 32) // 2, y + 8, 2)
    end
    cp.text(x + (cw - cp.textw(texto, 10)) // 2, y + 48, texto, 10)
  end
end

local function dibujarCasa()
  local W = cp.width()
  cp.text(24, 40, m.nombre, 14, true)
  local dia = "Día " .. (math.floor(m.edad / 86400) + 1)
  cp.text(W - 24 - cp.textw(dia, 12), 43, dia, 12)
  cp.line(24, 78, W - 24, 78, 1)

  local g = cuadroPerra()
  local s = SPRITES[g]
  local pw, ph = s.w * ESCALA, s.h * ESCALA
  local px = (W - pw) // 2
  local py = 190
  dibujo(g, px, py, ESCALA)
  if m.duerme then
    cp.text(px + pw - 40, py - 30, "z", 12)
    cp.text(px + pw - 20, py - 52, "Z", 14, true)
  end
  local iconoY = py + ph - 48
  if accion == "come" then dibujo("comida", px + pw + 6, iconoY, 3)
  elseif accion == "limpia" then dibujo("balde", px + pw + 6, iconoY, 3)
  elseif accion == "juega" then dibujo("pelota", px + pw + 6, iconoY, 3)
  end
  if m.viva and m.higiene < 25 then dibujo("caca", px - 54, iconoY, 3) end

  centrado(372, estadoTexto(), 12, true)
  if aviso then centrado(402, aviso, 10) end

  barra(440, "Hambre", m.hambre)
  barra(480, "Ánimo", m.animo)
  barra(520, "Energía", m.energia)
  barra(560, "Higiene", m.higiene)

  dibujarBotones()
  cp.text(24, cp.height() - 40, "Palanca: elegir · OK: hacer · Atrás: salir", 10)
end

local function dibujarNombre()
  local W = cp.width()
  centrado(60, "Un huevo nuevo", 14, true)
  dibujo(cuadro == 0 and "huevo1" or "huevo2", (W - 32 * 6) // 2, 200, 6)
  if escuchando then
    centrado(440, "Di cómo se llama…", 12)
  else
    centrado(440, "OK: decir el nombre por voz", 12)
    centrado(470, "Atrás: se llamará " .. NOMBRE_DEFAULT, 10)
  end
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
  local W = cp.width()
  cp.text(24, 40, "Mayor o menor", 14, true)
  local r = "Ronda " .. math.min(juego.ronda, RONDAS) .. " de " .. RONDAS
  cp.text(W - 24 - cp.textw(r, 12), 43, r, 12)
  cp.line(24, 78, W - 24, 78, 1)
  digito((W - 90) // 2, 160, 90, 150, juego.n, 14)
  local u = juego.ultimo
  if u then
    centrado(350, "Salió " .. u.sig .. ": " .. (u.acierto and "¡acierto!" or "no era"), 12, true)
  end
  centrado(390, "Aciertos: " .. juego.aciertos, 12)
  if juego.fin then
    centrado(460, m.nombre .. " se divirtió", 14, true)
    centrado(500, juego.aciertos .. " de " .. RONDAS, 12)
    cp.text(24, cp.height() - 40, "OK: volver", 10)
  else
    centrado(460, "¿El próximo número será mayor o menor?", 12)
    cp.text(24, cp.height() - 40, "ARRIBA: mayor · ABAJO: menor · Atrás: dejar de jugar", 10)
  end
end

local function dibujarInfo()
  local W = cp.width()
  cp.text(24, 40, m.nombre, 14, true)
  cp.line(24, 78, W - 24, 78, 1)
  local dias = math.floor(m.edad / 86400)
  local horas = math.floor((m.edad - dias * 86400) / HORA)
  local y = 110
  local function fila(s)
    cp.text(24, y, s, 12)
    y = y + 34
  end
  fila("Edad: " .. dias .. " días y " .. horas .. " h")
  fila("Hambre: " .. math.floor(m.hambre) .. " %")
  fila("Ánimo: " .. math.floor(m.animo) .. " %")
  fila("Energía: " .. math.floor(m.energia) .. " %")
  fila("Higiene: " .. math.floor(m.higiene) .. " %")
  fila("Duerme: " .. (m.duerme and "sí" or "no"))
  fila("Descuido: " .. math.floor(m.descuido / HORA) .. " h de " .. (SE_VA_TRAS // HORA))
  y = y + 20
  cp.text(24, y, "Cómo cuidarla", 12, true)
  y = y + 30
  cp.text(24, y, "Las barras bajan con el tiempo, también con la app cerrada.", 10)
  y = y + 24
  cp.text(24, y, "Con hambre o sucia mucho tiempo, se va.", 10)
  y = y + 24
  cp.text(24, y, "Sacudir la despierta de golpe; boca abajo se duerme.", 10)
  y = y + 40
  cp.text(24, y, "Dibujos: perrita de openclaw-tamagotchi (A. Katolikov, MIT)", 10)
  y = y + 24
  cp.text(24, y, "e iconos de OpenCritter (SuperMechaCow, MIT).", 10)
  cp.text(24, cp.height() - 40, "Atrás: volver", 10)
end

local function dibujarSeFue()
  local W = cp.width()
  local s = SPRITES.sefue
  dibujo("sefue", (W - s.w * ESCALA) // 2, 200, ESCALA)
  centrado(430, m.nombre .. " se fue", 14, true)
  centrado(470, "Vivió " .. math.floor(m.edad / 86400) .. " días", 12)
  centrado(500, "Pasó demasiado tiempo con hambre o sucia.", 10)
  cp.text(24, cp.height() - 40, "OK: un huevo nuevo · Atrás: salir", 10)
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
