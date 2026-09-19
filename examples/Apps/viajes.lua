-- CONCIERGE: la agenda, los papeles y la guía de cada día.
-- Todo se lee de la tarjeta (/Apps/data/viajes/): viaje.json es la vista
-- compacta que baja Actualizar, papel-<id>.txt y guia-<fecha>.txt se bajan una
-- vez y quedan, y pendientes.json guarda los tildes hechos sin red, que
-- Actualizar reproduce. La app NUNCA levanta la red sola: la levantan Actualizar,
-- la guía de un día, Sugerir, Preguntar, Agregar por voz y Recordar, cuando el
-- usuario los elige.
--
-- La guía es POR DÍA y sólo a pedido: no hay guía general del viaje. Cada día
-- tiene su lugar y su hotel (un viaje son varios lugares, trenes, un crucero).
--
-- REGLA: todo lo que entra por cp.read, cp.load, cp.files o el servidor pasa
-- por s() o n() antes de concatenarse, compararse, indexarse o medirse. Un nil
-- que llega de la tarjeta no se ve en el harness y en el aparato rompe la app.
--
-- Contrato: docs/ws397/VIAJES_CONTRATO.md (v2). Pantallas: docs/ws397/VIAJES_APP.md.
-- Estilo y helpers: examples/Apps/librito.lua.

local SIDE, PAD = 24, 24          -- margen de la pantalla y borde de fila -> texto
local ROW1, ROW2 = 48, 72         -- fila de uno y de dos renglones
local POLL_MS = 5000
local ARCHIVO = "viaje.json"
local PENDIENTES = "pendientes.json"
local RESPUESTA = "respuesta.txt"
local PREGUNTA = "¿Qué quieres saber del viaje?"

local st = "sin"                  -- pantalla de turno
local sel = 1
local trip = nil                  -- la vista compacta, normalizada (nunca un nil adentro)
local nViajes = 0                 -- cuántos viajes tiene la cuenta (de la última lista)
local viajes = {}                 -- la lista para Cambiar de viaje
local pend = {}                   -- tildes hechos sin red: {trip=, item=}
local calls = {}                  -- id de cp.call/cp.say -> {que=, ...}
local oir = nil                   -- para qué se escucha: {que=, ...}
local err = {titulo = "", cuerpo = "", filas = {}, volver = "inicio"}
local dia = 1                     -- índice del día abierto (Hoy / Día)
local item = nil                  -- {d=, i=} del ítem abierto
local itemDesde = "agenda"        -- a dónde vuelve Atrás desde el ítem
local sug = {}                    -- sugerencias: {t=, marcada=}
-- La guía del día en curso: qué día (di/date), a qué pantalla volver, la fase
-- (lista, preguntas, generando, bajando), el trabajo del servidor y qué días
-- ya tienen su guia-<fecha>.txt en la tarjeta.
local guia = {fase = "lista", di = 0, date = "", volver = "inicio", preguntas = {}, respuestas = {}, q = 0,
              job = nil, jobDate = "", jobTrip = "", step = 0, total = 0, label = "", tiene = {}, cuantas = 0}
local act = {fase = "", hechos = 0, total = 0}
local upd = {epoch = 0, ms = 0}   -- cuándo fue la última actualización
local ultimoPoll = 0

-- Todo lo que viene de afuera pasa por acá: nunca un nil donde va un texto.
local function s(v, def)
  if type(v) == "string" then return (v:gsub("[\r\n]+", " ")) end
  if type(v) == "number" then return tostring(v) end
  return def or ""
end
-- Igual pero conservando los renglones: para lo que va a un archivo del visor.
local function txt(v)
  if type(v) == "string" then return v end
  if type(v) == "number" then return tostring(v) end
  return ""
end
local function n(v, def)
  local x = tonumber(v)
  if x then return math.floor(x) end
  return def or 0
end

-- ---------------------------------------------------------------- json
-- No hay JSON en el cajón y la vista compacta se guarda entera en viaje.json,
-- así que va un codificador chico. El decodificador avanza con patrones (que
-- corren en C) y no letra por letra: 40 KB entran holgados en el tope de
-- instrucciones de una llamada.
local json = {}
do
  local ESC = {['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r',
               ['\t'] = '\\t'}
  local function encStr(v)
    return '"' .. v:gsub('[%c"\\]', function(c) return ESC[c] or string.format("\\u%04x", c:byte()) end) .. '"'
  end
  local function enc(v, out)
    local tv = type(v)
    if tv == "string" then
      out[#out + 1] = encStr(v)
    elseif tv == "number" then
      local i = math.tointeger(v)
      out[#out + 1] = i and tostring(i) or string.format("%.7g", v)
    elseif tv == "boolean" then
      out[#out + 1] = v and "true" or "false"
    elseif tv == "table" then
      if #v > 0 or next(v) == nil then
        out[#out + 1] = "["
        for i, x in ipairs(v) do
          if i > 1 then out[#out + 1] = "," end
          enc(x, out)
        end
        out[#out + 1] = "]"
      else
        out[#out + 1] = "{"
        local primero = true
        for k, x in pairs(v) do
          if type(k) == "string" then
            if not primero then out[#out + 1] = "," end
            primero = false
            out[#out + 1] = encStr(k)
            out[#out + 1] = ":"
            enc(x, out)
          end
        end
        out[#out + 1] = "}"
      end
    else
      out[#out + 1] = "null"
    end
  end
  function json.encode(v)
    local out = {}
    enc(v, out)
    return table.concat(out)
  end

  local UNESC = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}
  -- Un JSON malo devuelve nil, -1 y cada llamador lo propaga. Sin `error` ni
  -- `pcall` a propósito: un pcall acá se tragaría también el error de la
  -- guardia de instrucciones del cajón, y el segundo disparo condena al
  -- intérprete entero.
  local function blanco(str, pos)
    local _, e = str:find("^[ \t\r\n]*", pos)
    return e + 1
  end
  local function decStr(str, pos)  -- pos apunta a la comilla que abre
    local partes, i = {}, pos + 1
    while true do
      local trozo, e = str:match('^([^"\\]*)()', i)
      partes[#partes + 1] = trozo
      i = e
      local c = str:sub(i, i)
      if c == '"' then
        return table.concat(partes), i + 1
      elseif c == "\\" then
        local k = str:sub(i + 1, i + 1)
        if k == "u" then
          local cod = tonumber(str:sub(i + 2, i + 5), 16)
          if not cod then return nil, -1 end
          i = i + 6
          if cod >= 0xD800 and cod <= 0xDBFF and str:sub(i, i + 1) == "\\u" then
            local lo = tonumber(str:sub(i + 2, i + 5), 16)
            if lo and lo >= 0xDC00 and lo <= 0xDFFF then
              cod = 0x10000 + (cod - 0xD800) * 0x400 + (lo - 0xDC00)
              i = i + 6
            end
          end
          partes[#partes + 1] = utf8.char(cod)
        else
          partes[#partes + 1] = UNESC[k] or k
          i = i + 2
        end
      else
        return nil, -1  -- cadena sin cerrar
      end
    end
  end
  local dec
  dec = function(str, pos)
    pos = blanco(str, pos)
    local c = str:sub(pos, pos)
    if c == "{" then
      local t = {}
      pos = blanco(str, pos + 1)
      if str:sub(pos, pos) == "}" then return t, pos + 1 end
      while true do
        pos = blanco(str, pos)
        if str:sub(pos, pos) ~= '"' then return nil, -1 end
        local k
        k, pos = decStr(str, pos)
        if pos < 0 then return nil, -1 end
        pos = blanco(str, pos)
        if str:sub(pos, pos) ~= ":" then return nil, -1 end
        local v
        v, pos = dec(str, pos + 1)
        if pos < 0 then return nil, -1 end
        t[k] = v
        pos = blanco(str, pos)
        c = str:sub(pos, pos)
        if c == "," then pos = pos + 1 elseif c == "}" then return t, pos + 1 else return nil, -1 end
      end
    elseif c == "[" then
      local t = {}
      pos = blanco(str, pos + 1)
      if str:sub(pos, pos) == "]" then return t, pos + 1 end
      while true do
        local v
        v, pos = dec(str, pos)
        if pos < 0 then return nil, -1 end
        t[#t + 1] = v
        pos = blanco(str, pos)
        c = str:sub(pos, pos)
        if c == "," then pos = pos + 1 elseif c == "]" then return t, pos + 1 else return nil, -1 end
      end
    elseif c == '"' then
      return decStr(str, pos)
    elseif str:sub(pos, pos + 3) == "true" then
      return true, pos + 4
    elseif str:sub(pos, pos + 4) == "false" then
      return false, pos + 5
    elseif str:sub(pos, pos + 3) == "null" then
      return nil, pos + 4
    end
    local num, e = str:match("^(-?%d+%.?%d*)()", pos)
    if not num then return nil, -1 end
    local exp, e2 = str:match("^([eE][-+]?%d+)()", e)
    if exp then num, e = num .. exp, e2 end
    local v = tonumber(num)
    if not v then return nil, -1 end
    return v, e
  end
  function json.decode(str)
    if type(str) ~= "string" then return nil end
    local v, pos = dec(str, 1)
    if pos < 0 then return nil end
    return v
  end
end

-- ---------------------------------------------------------------- fechas
-- Días desde 1970-01-01 de una fecha civil (Howard Hinnant), sin `os`.
local function diasCivil(y, m, d)
  if m <= 2 then y = y - 1 end
  local era = (y >= 0 and y or y - 399) // 400
  local yoe = y - era * 400
  local mp = (m + 9) % 12
  local doy = (153 * mp + 2) // 5 + d - 1
  local doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
  return era * 146097 + doe - 719468
end
local function diasDe(fecha)  -- "YYYY-MM-DD" -> días, o nil
  local y, m, d = s(fecha):match("^(%d%d%d%d)%-(%d%d)%-(%d%d)")
  if not y then return nil end
  return diasCivil(tonumber(y), tonumber(m), tonumber(d))
end
local function fechaValida(f) return s(f):match("^%d%d%d%d%-%d%d%-%d%d$") ~= nil end
-- La fecha de hoy: del reloj del aparato, y si no está en hora, la que mandó el
-- servidor con el viaje. nil si no hay ninguna.
local function hoy()
  local t = cp.time()
  if type(t) == "table" and n(t.year) > 0 then
    return string.format("%04d-%02d-%02d", n(t.year), n(t.month), n(t.day))
  end
  if trip and trip.today ~= "" then return trip.today end
  return nil
end

-- Recorta con "…" hasta que entre en `ancho`. Corta por caracteres, no por bytes.
local function fit(t, ancho, tam)
  t = s(t)
  if cp.textw(t, tam) <= ancho then return t end
  local k = utf8.len(t) or #t
  while k > 0 do
    k = k - 1
    local off = utf8.offset(t, k + 1)
    local corte = (off and t:sub(1, off - 1) or t:sub(1, k)) .. "…"
    if cp.textw(corte, tam) <= ancho then return corte end
  end
  return "…"
end

-- Parte un texto en renglones que entren en `ancho`; `max` renglones como mucho.
local function wrap(t, ancho, tam, max)
  local lineas, cur = {}, ""
  for pal in s(t):gmatch("%S+") do
    local prueba = cur == "" and pal or (cur .. " " .. pal)
    if cp.textw(prueba, tam) <= ancho then
      cur = prueba
    else
      if cur ~= "" then lineas[#lineas + 1] = cur end
      cur = fit(pal, ancho, tam)
    end
  end
  if cur ~= "" then lineas[#lineas + 1] = cur end
  if max and #lineas > max then
    for i = #lineas, max + 1, -1 do lineas[i] = nil end
    lineas[max] = fit(lineas[max] .. " …", ancho, tam)
  end
  return lineas
end
-- "a · b", salteando lo vacío.
local function junta(a, b)
  a, b = s(a), s(b)
  if a == "" then return b end
  if b == "" then return a end
  return a .. " · " .. b
end

-- ---------------------------------------------------------------- datos
-- Nombre de archivo a partir de un id del servidor: sólo lo que cp.write acepta.
local function saneado(id)
  local v = s(id):gsub("[^A-Za-z0-9_-]", "-"):sub(1, 24)
  if v == "" then v = "x" end
  return v
end
local function archivoPapel(paperId) return "papel-" .. saneado(paperId) .. ".txt" end
local function archivoGuia(fecha) return "guia-" .. saneado(fecha) .. ".txt" end
local function existe(nombre) return cp.size(nombre) ~= nil end

local function normItem(it)
  return {id = s(it.id), at = s(it.at), title = s(it.title, "Sin título"), kind = s(it.kind),
          kindLabel = s(it.kindLabel), place = s(it.place), code = s(it.code), note = s(it.note),
          paperId = s(it.paperId), remind = it.remind == true}
end
local function normPacking(lista)
  local out = {}
  for _, k in ipairs(type(lista) == "table" and lista or {}) do
    if type(k) == "table" then out[#out + 1] = {id = s(k.id), text = s(k.text), done = k.done == true} end
  end
  return out
end
-- La vista compacta del servidor, con cada campo en su tipo y sin agujeros.
local function normalizar(t)
  if type(t) ~= "table" then t = {} end
  local v = {id = s(t.id), name = s(t.name, "Viaje"), place = s(t.place), start = s(t.start),
             ["end"] = s(t["end"]), when = s(t.when), today = s(t.today), days = {},
             packing = normPacking(t.packing), papers = {}}
  for i, d in ipairs(type(t.days) == "table" and t.days or {}) do
    if type(d) == "table" then
      local items = {}
      for _, it in ipairs(type(d.items) == "table" and d.items or {}) do
        if type(it) == "table" then items[#items + 1] = normItem(it) end
      end
      local g = type(d.guide) == "table" and d.guide or {}
      v.days[#v.days + 1] = {date = s(d.date), n = n(d.n, i), label = s(d.label, s(d.date, "Día " .. i)),
                             short = s(d.short, s(d.date, tostring(i))), place = s(d.place), hotel = s(d.hotel),
                             note = s(d.note), guide = {ready = g.ready == true, at = n(g.at)}, items = items}
    end
  end
  for _, p in ipairs(type(t.papers) == "table" and t.papers or {}) do
    if type(p) == "table" then
      v.papers[#v.papers + 1] = {id = s(p.id), date = s(p.date), kind = s(p.kind), title = s(p.title, "Papel"),
                                 line = s(p.line)}
    end
  end
  return v
end

local function guardarViaje()
  if trip then cp.write(ARCHIVO, json.encode(trip)) end
end
local function guardarPend()
  if #pend == 0 then
    cp.remove(PENDIENTES)
  else
    cp.write(PENDIENTES, json.encode(pend))
  end
end
local function cargarPend()
  pend = {}
  local lista = json.decode(cp.read(PENDIENTES))
  for _, p in ipairs(type(lista) == "table" and lista or {}) do
    if type(p) == "table" and s(p.item) ~= "" then pend[#pend + 1] = {trip = s(p.trip), item = s(p.item)} end
  end
end
-- Un tilde hecho sin red: si ya estaba pendiente, se anula (dos toques = nada).
local function anotarPend(itemId)
  for i, p in ipairs(pend) do
    if p.trip == trip.id and p.item == itemId then
      table.remove(pend, i)
      guardarPend()
      return
    end
  end
  pend[#pend + 1] = {trip = trip.id, item = itemId}
  guardarPend()
end
-- Lo que el servidor manda de la lista no sabe de los tildes que todavía no
-- subieron: se aplican encima, hasta que Actualizar los reproduzca.
local function aplicarPend()
  if not trip then return end
  for _, p in ipairs(pend) do
    if p.trip == trip.id then
      for _, k in ipairs(trip.packing) do
        if k.id == p.item then k.done = not k.done end
      end
    end
  end
end

-- Estado chico: una clave por renglón (no hay JSON en cp.save y esto alcanza).
-- El trabajo de la guía se guarda con su día y su viaje: al volver a entrar
-- se ofrece "Retomar la guía del día N".
local function guardar()
  cp.save("trip=" .. (trip and trip.id or "") .. "\nviajes=" .. nViajes .. "\nupdE=" .. upd.epoch ..
          "\nupdM=" .. upd.ms .. "\njob=" .. s(guia.job) .. "\njobDate=" .. s(guia.jobDate) ..
          "\njobTrip=" .. s(guia.jobTrip) .. "\n")
end
local function cargar()
  local d = cp.load()
  if type(d) ~= "string" then return end
  local t = {}
  for k, v in d:gmatch("(%w+)=([^\n]*)") do t[k] = v end
  nViajes = n(t.viajes)
  upd.epoch, upd.ms = n(t.updE), n(t.updM)
  guia.job = s(t.job) ~= "" and s(t.job) or nil
  guia.jobDate, guia.jobTrip = s(t.jobDate), s(t.jobTrip)
end
local function olvidarJob()
  guia.job, guia.jobDate, guia.jobTrip = nil, "", ""
  guardar()
end

-- Los papeles y las guías son de UN viaje: al cambiar de viaje se van.
local function limpiarArchivos()
  for _, nombre in ipairs(cp.files()) do
    nombre = s(nombre)
    if nombre:match("^papel%-") or nombre:match("^guia%-") or nombre == RESPUESTA then cp.remove(nombre) end
  end
  guia.tiene, guia.cuantas = {}, 0
  guia.job, guia.jobDate, guia.jobTrip = nil, "", ""
end

-- ---------------------------------------------------------------- días
local function diaPorFecha(fecha)
  fecha = s(fecha)
  if not trip or fecha == "" then return nil end
  for i, d in ipairs(trip.days) do
    if d.date == fecha then return i end
  end
  return nil
end

-- Qué días tienen su guía en la tarjeta. Una sola lectura del directorio, al
-- abrir la app y cada vez que una guía llega; el inicio muestra `guia.cuantas`
-- sin volver a mirar. Un trabajo guardado que no es de este viaje se olvida.
local function guiaRevisar()
  guia.tiene = {}
  for _, nombre in ipairs(cp.files()) do
    local fecha = s(nombre):match("^guia%-(%d%d%d%d%-%d%d%-%d%d)%.txt$")
    if fecha then guia.tiene[fecha] = true end
  end
  local cuantas = 0
  for _, d in ipairs(trip and trip.days or {}) do
    if guia.tiene[d.date] then cuantas = cuantas + 1 end
  end
  guia.cuantas = cuantas
  if guia.job and (not trip or guia.jobTrip ~= trip.id or not diaPorFecha(guia.jobDate)) then
    guia.job, guia.jobDate, guia.jobTrip = nil, "", ""
    guardar()
  end
  return cuantas
end

local function llegoViaje(raw)
  local nuevo = normalizar(raw)
  if trip and trip.id ~= nuevo.id then limpiarArchivos() end
  trip = nuevo
  aplicarPend()
  guardarViaje()
  guiaRevisar()
  local t = cp.time()
  upd.epoch = (type(t) == "table" and n(t.epoch) > 0) and n(t.epoch) or 0
  upd.ms = cp.ms()
  guardar()
end

-- El día que corresponde a "Hoy": el de hoy si el viaje está en curso; antes
-- del viaje, el primer día con cosas; después, nada. Devuelve índice, estado.
local function diaHoy()
  local h = hoy()
  if not h then return nil, "sinhora" end
  if #trip.days == 0 then return nil, "vacio" end
  if trip["end"] ~= "" and h > trip["end"] then return nil, "termino" end
  if trip.start ~= "" and h < trip.start then
    for i, d in ipairs(trip.days) do
      if #d.items > 0 then return i, "antes" end
    end
    return 1, "antes"
  end
  return diaPorFecha(h) or 1, "hoy"
end
local function cosas(d)
  if #d.items == 0 then return "libre" end
  return #d.items == 1 and "1 cosa" or (#d.items .. " cosas")
end
local function faltan(h)
  local a, b = diasDe(h), diasDe(trip.start)
  if not a or not b then return "" end
  local k = b - a
  if k <= 0 then return "" end
  return k == 1 and "falta 1 día" or ("faltan " .. k .. " días")
end
-- La segunda línea del inicio: fechas y en qué punto del viaje estamos.
local function subtitulo()
  local h = hoy()
  local base = trip.when ~= "" and trip.when or (trip.start .. " – " .. trip["end"])
  if not h then return base end
  if trip.start ~= "" and h < trip.start then
    local f = faltan(h)
    return f ~= "" and (base .. " · " .. f) or base
  end
  if trip["end"] ~= "" and h > trip["end"] then return base .. " · terminó" end
  local i = diaPorFecha(h)
  if i then return "Día " .. trip.days[i].n .. " de " .. #trip.days .. " · " .. trip.days[i].short end
  return base
end
local function llevar()
  local hechos = 0
  for _, k in ipairs(trip.packing) do
    if k.done then hechos = hechos + 1 end
  end
  return hechos, #trip.packing
end
local function haceCuanto()
  local t = cp.time()
  local seg
  if upd.epoch > 0 and type(t) == "table" and n(t.epoch) > 0 then
    seg = n(t.epoch) - upd.epoch
  elseif upd.ms > 0 and cp.ms() >= upd.ms then
    seg = (cp.ms() - upd.ms) // 1000
  elseif upd.ms > 0 or upd.epoch > 0 then
    return "hace un rato"
  else
    return "nunca"
  end
  if seg < 60 then return "hace un momento" end
  if seg < 3600 then return "hace " .. seg // 60 .. " min" end
  if seg < 86400 then return "hace " .. seg // 3600 .. " h" end
  return "hace " .. seg // 86400 .. " días"
end
local function tituloGuia(d) return "Guía · Día " .. d.n end

-- ---------------------------------------------------------------- pedidos
local function pedir(servicio, args, que, extra)
  local id = cp.call(servicio, args)
  if not id then
    cp.beep("error")
    return false
  end
  extra = extra or {}
  extra.que = que
  calls[id] = extra
  return true
end

local function escuchar(que, pregunta, extra)
  extra = extra or {}
  extra.que = que
  oir = extra
  if not cp.listen(15, pregunta) then
    oir = nil
    cp.beep("error")
  end
  return false  -- el host muestra su propia pantalla de escucha
end

-- Cambia de pantalla. "guia" es la lista de días (7); las fases del flujo
-- las ponen las funciones de la guía.
local function irA(pantalla)
  st, sel = pantalla, 1
  if st == "guia" then guia.fase = "lista" end
end

local function fallo(tit, cuerpo, filas, volver)
  err = {titulo = tit, cuerpo = cuerpo, filas = filas, volver = volver or "inicio"}
  st, sel = "error", 1
  cp.beep("error")
end
local function filaVolver(volver)
  return {{t = "Volver", h = ROW1, act = "volver"}}, volver
end
local function filasReintento(reintento, volver)
  return {{t = "Reintentar", h = ROW1, act = reintento}, {t = "Volver", h = ROW1, act = "volver"}}, volver
end

-- Un error con "clave" adentro es la clave de las apps que falta en la web.
local function errorServidor(texto, filas, volver)
  texto = s(texto)
  if texto == "" then texto = "Sin conexión con el servidor" end
  if texto:lower():find("clave", 1, true) then
    fallo("Falta la clave de las apps",
          "El servidor no tiene la clave para las apps de Lua. Se carga en la web: Ajustes → Apps de Lua. " ..
          "(" .. texto .. ")", filas, volver)
  else
    fallo("No se pudo", texto, filas, volver)
  end
end
-- Un error que puso el firmware (sin red, sin token, cancelado) se distingue
-- de uno del servidor: al reproducir los tildes pendientes, el del servidor
-- ("ese ítem ya no existe") se descarta y se sigue; el otro para todo.
local function errorDelHost(texto)
  texto = s(texto)
  return texto == "cancelado" or texto == "sin vincular" or texto:find("^sin conexión") ~= nil or
         texto:find("^respuesta ilegible") ~= nil
end

local function pantallaBase() return trip and "inicio" or "sin" end

-- ---------------------------------------------------------------- actualizar
local function actPaso()
  if act.fase == "pendientes" and #pend > 0 then
    local p = pend[1]
    return pedir("viajes.llevar", {id = p.trip, action = "toggle", itemId = p.item}, "act.toggle")
  end
  act.fase = "lista"
  return pedir("viajes.lista", {}, "act.lista")
end
local function actualizar()
  st, sel = "actualizar", 1
  act = {fase = "pendientes", hechos = 0, total = #pend}
  if not actPaso() then fallo("No se pudo", "No se pudo empezar la actualización.", filaVolver(pantallaBase())) end
  return true
end
-- El viaje activo de la lista, o el primero si la cuenta no marcó ninguno.
local function activoDe(lista)
  local trips = {}
  for _, v in ipairs(type(lista) == "table" and lista or {}) do
    if type(v) == "table" and s(v.id) ~= "" then
      trips[#trips + 1] = {id = s(v.id), name = s(v.name, "Viaje"), when = s(v.when), place = s(v.place),
                           active = v.active == true}
    end
  end
  local activo = nil
  for _, v in ipairs(trips) do
    if v.active then activo = v end
  end
  return trips, activo or trips[1]
end
local function sinViajes()
  trip = nil
  cp.remove(ARCHIVO)
  limpiarArchivos()
  guardar()
  st, sel = "sin", 1
end

-- ---------------------------------------------------------------- guía del día: flujo
-- preguntas por voz → viajes.guia.generar (trabajo) → job.status cada 5 s →
-- viajes.guia.dia → guia-<fecha>.txt → visor. Termina volviendo a la pantalla
-- desde la que se pidió (guia.volver), con el visor encima.
-- Vuelve a la pantalla desde la que se pidió la guía, con el resalte sobre la
-- fila de ese día (la que se eligió), no sobre la primera.
local filas
local function guiaSalir()
  if (guia.volver == "dia" or guia.volver == "hoy") and guia.di > 0 then dia = guia.di end
  irA(guia.volver)
  if st == "inicio" then return end
  for k, f in ipairs(filas()) do
    if f.act == "guia.abrir" and (st == "guia" and f.i == guia.di or st ~= "guia") then
      sel = k
      return
    end
  end
end
local function guiaGenerar()
  guia.fase = "generando"
  guia.step, guia.total, guia.label = 0, 0, "Pidiendo la guía"
  return pedir("viajes.guia.generar", {id = trip.id, date = guia.date, answers = guia.respuestas}, "guia.generar")
end
local function guiaPreguntar()
  guia.q = guia.q + 1
  local p = guia.preguntas[guia.q]
  if p then return escuchar("guia", p.text) end
  return guiaGenerar()
end
-- Apunta el flujo al día `di`. false si ese día no puede tener guía (sin fecha).
local function guiaApuntar(di, volver, fase)
  local d = trip.days[di]
  if not d or not fechaValida(d.date) then
    cp.beep("error")
    return false
  end
  guia.di, guia.date, guia.volver, guia.fase = di, d.date, volver, fase
  st, sel = "guia", 1
  return true
end
local function guiaEmpezar(di, volver)
  if not guiaApuntar(di, volver, "preguntas") then return false end
  guia.preguntas, guia.respuestas, guia.q = {}, {}, 0
  olvidarJob()
  return pedir("viajes.guia.preguntas", {id = trip.id, date = guia.date}, "guia.preguntas")
end
-- Bajar la guía que el servidor ya tiene: la que acaba de armar, o una hecha
-- desde la web (day.guide.ready). No pregunta nada.
local function guiaBajar(di, volver)
  if not guiaApuntar(di, volver, "bajando") then return false end
  return pedir("viajes.guia.dia", {id = trip.id, date = guia.date}, "guia.dia")
end
-- Seguir mirando un trabajo que quedó andando en el servidor.
local function guiaRetomar(volver)
  local di = diaPorFecha(guia.jobDate)
  if not guia.job or not di then
    olvidarJob()
    return false
  end
  if not guiaApuntar(di, volver, "generando") then return false end
  guia.step, guia.total, guia.label = 0, 0, ""
  ultimoPoll = cp.ms() - POLL_MS  -- que el primer tick pregunte ya
  return true
end
-- La fila "Guía de este día": en la tarjeta → visor; en el servidor → bajar;
-- si no hay → armarla. Sin confirmaciones, por decisión del dueño.
local function guiaAbrir(di, volver)
  local d = trip.days[di]
  if not d then return false end
  if guia.job and guia.jobDate == d.date then return guiaRetomar(volver) end
  if guia.tiene[d.date] then return cp.view(archivoGuia(d.date), tituloGuia(d)) end
  if d.guide.ready then return guiaBajar(di, volver) end
  return guiaEmpezar(di, volver)
end

-- ---------------------------------------------------------------- papeles
local function papelDe(paperId)
  for _, p in ipairs(trip.papers) do
    if p.id == paperId then return p end
  end
  return nil
end
local function abrirPapel(paperId, titulo)
  local nombre = archivoPapel(paperId)
  if existe(nombre) then
    cp.view(nombre, titulo)
    return true
  end
  return pedir("viajes.papel", {id = trip.id, paperId = paperId}, "papel", {nombre = nombre, titulo = titulo})
end

local function preguntar(ctx)
  return escuchar("preguntar", PREGUNTA, {ctx = ctx or {}})
end

function on_open()
  st, sel, calls, oir, viajes, sug = "sin", 1, {}, nil, {}, {}
  guia = {fase = "lista", di = 0, date = "", volver = "inicio", preguntas = {}, respuestas = {}, q = 0,
          job = nil, jobDate = "", jobTrip = "", step = 0, total = 0, label = "", tiene = {}, cuantas = 0}
  cargar()
  cargarPend()
  local raw = json.decode(cp.read(ARCHIVO))
  if type(raw) == "table" and s(raw.id) ~= "" then
    trip = normalizar(raw)
    st = "inicio"
  else
    trip = nil
  end
  guiaRevisar()
end

-- ---------------------------------------------------------------- filas
local function filaItem(d, i)
  local it = trip.days[d].items[i]
  local t = it.at ~= "" and (it.at .. "  " .. it.title) or it.title
  local det = junta(it.kindLabel, it.place)
  return {t = t, d = det ~= "" and det or nil, m = it.paperId ~= "" and "papel" or nil,
          h = det ~= "" and ROW2 or ROW1, act = "item", d_ = d, i = i}
end
-- Las filas de la guía de un día (en Hoy y en Día): ver, bajar o armar, y
-- rehacer si ya existe.
local function filasGuiaDia(f, di)
  local d = trip.days[di]
  if not d or not fechaValida(d.date) then return end
  if guia.job and guia.jobDate == d.date then
    f[#f + 1] = {t = "Retomar la guía del día", d = "Se sigue armando en el servidor", h = ROW2,
                 act = "guia.retomar"}
  elseif guia.tiene[d.date] then
    f[#f + 1] = {t = "Guía de este día", d = "En la tarjeta", h = ROW2, act = "guia.abrir", i = di}
    f[#f + 1] = {t = "Rehacer la guía del día", h = ROW1, act = "guia.rehacer", i = di}
  elseif d.guide.ready then
    f[#f + 1] = {t = "Guía de este día", d = "Bajar del servidor", h = ROW2, act = "guia.abrir", i = di}
    f[#f + 1] = {t = "Rehacer la guía del día", h = ROW1, act = "guia.rehacer", i = di}
  else
    f[#f + 1] = {t = "Guía de este día", d = "Se arma con unas preguntas por voz", h = ROW2, act = "guia.abrir",
                 i = di}
  end
end

-- Las filas de la pantalla de turno. Cada una sabe su alto y qué hace OK.
filas = function()
  local f = {}
  if st == "sin" then
    f[1] = {t = "Actualizar", h = ROW1, act = "actualizar"}
  elseif st == "inicio" then
    local ji = guia.job and diaPorFecha(guia.jobDate)
    if ji then
      f[#f + 1] = {t = "Retomar la guía del día " .. trip.days[ji].n, d = "Se sigue armando en el servidor",
                   h = ROW2, act = "guia.retomar"}
    end
    local i, como = diaHoy()
    if como == "sinhora" then
      f[#f + 1] = {t = "Hoy", d = "El aparato no está en hora", h = ROW2, act = "agenda"}
    elseif como == "termino" then
      f[#f + 1] = {t = "Hoy", d = "El viaje terminó", h = ROW2, act = "agenda"}
    elseif como == "vacio" then
      f[#f + 1] = {t = "Hoy", d = "El viaje no tiene días", h = ROW2, act = "agenda"}
    else
      local d = trip.days[i]
      local t = junta(junta((como == "hoy" and "Hoy" or ("Día " .. d.n)) .. " · " .. d.short, d.place), cosas(d))
      f[#f + 1] = {t = t, h = ROW1, act = "hoy", i = i}
    end
    f[#f + 1] = {t = "Agenda · " .. #trip.days .. (#trip.days == 1 and " día" or " días"), h = ROW1, act = "agenda"}
    f[#f + 1] = {t = "Papeles · " .. #trip.papers, h = ROW1, act = "papeles"}
    local hechos, total = llevar()
    f[#f + 1] = {t = "Lista para llevar · " .. hechos .. " de " .. total, h = ROW1, act = "lista"}
    f[#f + 1] = {t = "Guía · " .. guia.cuantas .. " de " .. #trip.days .. (#trip.days == 1 and " día" or " días"),
                 h = ROW1, act = "guia"}
    f[#f + 1] = {t = "Preguntar por voz", h = ROW1, act = "preg"}
    f[#f + 1] = {t = "Actualizar · " .. haceCuanto(), h = ROW1, act = "actualizar"}
    if nViajes > 1 then f[#f + 1] = {t = "Cambiar de viaje (" .. nViajes .. ")", h = ROW1, act = "cambiar"} end
  elseif st == "hoy" or st == "dia" then
    local d = trip.days[dia]
    if d then
      for i = 1, #d.items do f[#f + 1] = filaItem(dia, i) end
      if #d.items == 0 then f[#f + 1] = {t = "— libre —", h = ROW1} end
      filasGuiaDia(f, dia)
      f[#f + 1] = {t = st == "hoy" and "Preguntar sobre hoy" or "Preguntar sobre este día", h = ROW1,
                   act = "preg", ctx = {date = d.date}}
    end
  elseif st == "agenda" then
    for di, d in ipairs(trip.days) do
      local det = junta(d.place, d.note)
      if det == "" and #d.items == 0 then det = "— libre —" end
      f[#f + 1] = {t = "Día " .. d.n .. " · " .. d.label, d = det ~= "" and det or nil, h = det ~= "" and ROW2 or ROW1,
                   act = "dia", i = di}
      for i = 1, #d.items do f[#f + 1] = filaItem(di, i) end
    end
  elseif st == "item" then
    local it = trip.days[item.d].items[item.i]
    if it.paperId ~= "" then f[#f + 1] = {t = "Ver el papel", h = ROW1, act = "verpapel"} end
    f[#f + 1] = {t = "Recordar 2 h antes", h = ROW1, act = "recordar", casilla = it.remind}
    f[#f + 1] = {t = "Preguntar sobre esto", h = ROW1, act = "preg",
                 ctx = {date = trip.days[item.d].date, itemId = it.id}}
  elseif st == "papeles" then
    for i, p in ipairs(trip.papers) do
      local det = junta(p.date, p.line)
      f[#f + 1] = {t = p.title, d = det ~= "" and det or nil, h = det ~= "" and ROW2 or ROW1, act = "papel", i = i}
    end
    if #trip.papers == 0 then f[1] = {t = "No hay papeles. Se cargan en la web.", h = ROW1} end
  elseif st == "lista" then
    for i, k in ipairs(trip.packing) do
      f[#f + 1] = {t = k.text, h = ROW1, act = "tildar", i = i, casilla = k.done}
    end
    f[#f + 1] = {t = "Agregar por voz", h = ROW1, act = "agregar"}
    f[#f + 1] = {t = "Sugerir con IA", h = ROW1, act = "sugerir"}
  elseif st == "sug" then
    for i, x in ipairs(sug) do f[#f + 1] = {t = x.t, h = ROW1, act = "sug", i = i, casilla = x.marcada} end
    f[#f + 1] = {t = "Agregar las marcadas", h = ROW1, act = "sug.agregar"}
  elseif st == "guia" and guia.fase == "lista" then
    -- Un día por fila; el detalle dice si la guía ya está.
    for di, d in ipairs(trip.days) do
      local det
      if guia.job and guia.jobDate == d.date then
        det = "generando…"
      elseif guia.tiene[d.date] then
        det = "guía lista"
      elseif d.guide.ready then
        det = "en el servidor"
      end
      f[#f + 1] = {t = junta("Día " .. d.n .. " · " .. d.short, d.place), d = det, h = det and ROW2 or ROW1,
                   act = "guia.abrir", i = di}
    end
    if #trip.days == 0 then f[1] = {t = "El viaje no tiene días.", h = ROW1} end
  elseif st == "cambiar" then
    for i, v in ipairs(viajes) do
      local det = junta(v.when, v.place)
      f[#f + 1] = {t = v.name, d = det ~= "" and det or nil, m = v.active and "activo" or nil,
                   h = det ~= "" and ROW2 or ROW1, act = "cambiar.a", i = i}
    end
  elseif st == "error" then
    f = err.filas
  end
  return f
end

-- En la agenda el resalte arranca en el día de hoy, que es el que se busca.
local function selAgenda()
  local i, como = diaHoy()
  if como ~= "hoy" or not i then return 1 end
  local f = filas()
  for k, fila in ipairs(f) do
    if fila.act == "dia" and fila.i == i then return k end
  end
  return 1
end

-- ---------------------------------------------------------------- acciones
local function accion(it)
  local a = it and it.act
  if not a then
    cp.beep("error")
    return false
  end
  if a == "actualizar" then
    return actualizar()
  elseif a == "hoy" then
    dia, st, sel = it.i, "hoy", 1
  elseif a == "agenda" then
    st = "agenda"
    sel = selAgenda()
  elseif a == "papeles" or a == "lista" then
    st, sel = a, 1
  elseif a == "guia" then
    irA("guia")
  elseif a == "preg" then
    return preguntar(it.ctx)
  elseif a == "cambiar" then
    st, sel, viajes = "cambiar", 1, {}
    return pedir("viajes.lista", {}, "cambiar.lista")
  elseif a == "dia" then
    dia, st, sel = it.i, "dia", 1
  elseif a == "item" then
    item, itemDesde, st, sel = {d = it.d_, i = it.i}, st, "item", 1
  elseif a == "papel" then
    local p = trip.papers[it.i]
    if not p then return false end
    return abrirPapel(p.id, p.title)
  elseif a == "verpapel" then
    local x = trip.days[item.d].items[item.i]
    local p = papelDe(x.paperId)
    return abrirPapel(x.paperId, p and p.title or x.title)
  elseif a == "recordar" then
    local x = trip.days[item.d].items[item.i]
    return pedir("viajes.recordar", {id = trip.id, itemId = x.id, on = not x.remind}, "recordar",
                 {d = item.d, i = item.i, on = not x.remind})
  elseif a == "tildar" then
    local k = trip.packing[it.i]
    if not k then return false end
    k.done = not k.done
    anotarPend(k.id)
    guardarViaje()
  elseif a == "agregar" then
    return escuchar("agregar", "¿Qué agregamos a la lista?")
  elseif a == "sugerir" then
    return pedir("viajes.sugerir", {id = trip.id}, "sugerir")
  elseif a == "sug" then
    local x = sug[it.i]
    if not x then return false end
    x.marcada = not x.marcada
  elseif a == "sug.agregar" then
    local textos = {}
    for _, x in ipairs(sug) do
      if x.marcada then textos[#textos + 1] = x.t end
    end
    if #textos == 0 then
      st, sel = "lista", 1
      cp.beep("back")
      return true
    end
    return pedir("viajes.sugerir.agregar", {id = trip.id, texts = textos}, "packing", {volver = "lista"})
  elseif a == "guia.abrir" then
    return guiaAbrir(it.i, st)
  elseif a == "guia.rehacer" then
    return guiaEmpezar(it.i, st)
  elseif a == "guia.retomar" then
    return guiaRetomar(st)
  elseif a == "guia.reintentar" then
    st, sel = "guia", 1
    if guia.fase == "bajando" then return pedir("viajes.guia.dia", {id = trip.id, date = guia.date}, "guia.dia") end
    if guia.fase == "generando" and guia.job then
      ultimoPoll = cp.ms() - POLL_MS
      return true
    end
    return guiaEmpezar(guia.di, guia.volver)
  elseif a == "cambiar.a" then
    local v = viajes[it.i]
    if not v then return false end
    return pedir("viajes.activar", {id = v.id}, "cambiar.activar", {id = v.id})
  elseif a == "volver" then
    irA(err.volver)
  elseif a == "inicio" then
    irA(pantallaBase())
  else
    return false
  end
  cp.beep("ok")
  return true
end

local function atras()
  if st == "sin" or st == "inicio" then
    cp.quit()
    return false
  elseif st == "hoy" or st == "agenda" or st == "papeles" or st == "lista" or st == "cambiar" then
    st = "inicio"
  elseif st == "dia" then
    st = "agenda"
    sel = selAgenda()
    cp.beep("back")
    return true
  elseif st == "item" then
    st = itemDesde
    if st == "agenda" then
      sel = selAgenda()
      cp.beep("back")
      return true
    end
  elseif st == "sug" then
    st = "lista"
  elseif st == "guia" then
    if guia.fase == "lista" then
      st = "inicio"
    else
      guiaSalir()  -- generando sigue en el servidor y se retoma al volver a entrar
    end
  elseif st == "actualizar" then
    st = pantallaBase()
  elseif st == "error" then
    irA(err.volver)
  end
  sel = 1
  cp.beep("back")
  return true
end

function on_key(k)
  -- Con algo en curso el host sólo manda "back", y lo cancela él. Nunca salir.
  if cp.busy() then return k == "back" end
  local f = filas()
  if sel > #f then sel = math.max(1, #f) end
  if k == "up" or k == "down" then
    if #f <= 1 then return false end
    if k == "up" then sel = sel > 1 and sel - 1 or #f else sel = sel < #f and sel + 1 or 1 end
    cp.beep("nav")
    return true
  elseif k == "ok" then
    return accion(f[sel])
  elseif k == "back" then
    return atras()
  end
  return false
end

function on_tick()
  if st == "guia" and guia.fase == "generando" and guia.job and not cp.busy() and
     cp.ms() - ultimoPoll >= POLL_MS then
    ultimoPoll = cp.ms()
    pedir("job.status", {id = guia.job}, "guia.job")
  end
  return false  -- el host repinta solo cuando llega la respuesta
end

function on_heard(texto)
  local que = oir
  oir = nil
  if not que then return end
  local vacio = type(texto) ~= "string" or texto:match("^%s*$") ~= nil
  if que.que == "guia" then
    -- Atrás (o no entender) salta la pregunta; con todas contestadas o
    -- saltadas se genera igual.
    if not vacio then
      local p = guia.preguntas[guia.q]
      if p then guia.respuestas[p.key] = s(texto) end
    end
    guiaPreguntar()
    return
  end
  if vacio then return end
  texto = s(texto)
  if que.que == "preguntar" then
    local args = {id = trip.id, question = texto}
    if type(que.ctx) == "table" then
      if s(que.ctx.date) ~= "" then args.date = s(que.ctx.date) end
      if s(que.ctx.itemId) ~= "" then args.itemId = s(que.ctx.itemId) end
    end
    pedir("viajes.preguntar", args, "preguntar", {q = texto})
  elseif que.que == "agregar" then
    pedir("viajes.llevar", {id = trip.id, action = "add", text = texto}, "packing", {volver = "lista"})
  end
end

local function llegoPacking(t, volver)
  trip.packing = normPacking(t.packing)
  aplicarPend()
  guardarViaje()
  st, sel = volver or "lista", 1
  cp.beep("ok")
end

local function volverGuia() return guia.volver end

local function llegoJob(t)
  local estado = s(t.state)
  if estado == "done" then
    guia.fase = "bajando"
    if not pedir("viajes.guia.dia", {id = trip.id, date = guia.date}, "guia.dia") then
      fallo("No se pudo", "No se pudo empezar a bajar la guía.", filasReintento("guia.reintentar", volverGuia()))
    end
  elseif estado == "failed" then
    olvidarJob()
    guia.fase = "preguntas"
    fallo("No se pudo armar la guía", s(t.error, "El servidor no pudo armar la guía."),
          filasReintento("guia.reintentar", volverGuia()))
  else
    guia.step, guia.total, guia.label = n(t.step), n(t.total), s(t.label)
    ultimoPoll = cp.ms()
  end
end

-- La guía del día llegó: a la tarjeta, a la vista, y al visor.
local function llegoGuia(t)
  local texto = txt(t.text)
  if texto == "" then
    return fallo("Sin guía", "El servidor no tiene la guía de ese día.", filasReintento("guia.reintentar", volverGuia()))
  end
  local d = trip.days[guia.di]
  local nombre = archivoGuia(guia.date)
  local cabeza = d and junta("Día " .. d.n .. " · " .. d.label, d.place) or "Guía"
  if not cp.write(nombre, cabeza .. "\n\n" .. texto) then
    return fallo("No se pudo", "No se pudo guardar la guía en la tarjeta.",
                 filasReintento("guia.reintentar", volverGuia()))
  end
  if d then
    d.guide = {ready = true, at = n(t.at)}
    guardarViaje()
  end
  olvidarJob()
  guiaRevisar()
  cp.beep("ok")
  guiaSalir()
  cp.view(nombre, d and tituloGuia(d) or "Guía")
end

function on_reply(id, ok, t)
  local c = calls[id]
  calls[id] = nil
  if not c then return end
  local que = c.que
  if type(t) ~= "table" then t = {} end
  if not ok then
    local e = s(t.error)
    if que == "decir" then return end  -- sin voz no pasa nada: el texto ya está en el visor
    if que == "act.toggle" and not errorDelHost(e) then
      -- El servidor no quiso ese tilde (el ítem ya no existe): se descarta y se sigue.
      table.remove(pend, 1)
      guardarPend()
      act.hechos = act.hechos + 1
      actPaso()
      return
    end
    if e == "cancelado" then
      if que:find("^act%.") then st, sel = pantallaBase(), 1 end
      if que == "guia.preguntas" or que == "guia.generar" or que == "guia.dia" then guiaSalir() end
      if que == "cambiar.lista" or que == "cambiar.activar" or que == "cambiar.viaje" then st, sel = "inicio", 1 end
      return
    end
    if que:find("^act%.") then return errorServidor(e, filasReintento("actualizar", pantallaBase())) end
    if que:find("^guia%.") then
      if que == "guia.generar" or que == "guia.preguntas" then guia.fase = "preguntas" end
      return errorServidor(e, filasReintento("guia.reintentar", volverGuia()))
    end
    if que:find("^cambiar%.") then return errorServidor(e, filaVolver("inicio")) end
    return errorServidor(e, filaVolver(st))
  end

  if que == "act.toggle" then
    table.remove(pend, 1)
    guardarPend()
    act.hechos = act.hechos + 1
    actPaso()
  elseif que == "act.lista" then
    local trips, activo = activoDe(t.trips)
    nViajes = #trips
    if not activo then return sinViajes() end
    act.fase = "viaje"
    pedir("viajes.viaje", {id = activo.id}, "act.viaje")
  elseif que == "act.viaje" or que == "cambiar.viaje" then
    if type(t.trip) ~= "table" then
      return fallo("No se pudo", "El servidor no mandó el viaje.", filasReintento("actualizar", pantallaBase()))
    end
    llegoViaje(t.trip)
    st, sel = "inicio", 1
    cp.beep("ok")
  elseif que == "cambiar.lista" then
    viajes = activoDe(t.trips)
    nViajes = #viajes
    guardar()
    sel = 1
    if #viajes == 0 then return sinViajes() end
  elseif que == "cambiar.activar" then
    pedir("viajes.viaje", {id = c.id}, "cambiar.viaje")
  elseif que == "papel" then
    local cuerpo = txt(t.text)
    if cuerpo == "" then
      return fallo("No se pudo", "El servidor no mandó el texto del papel.", filaVolver(st))
    end
    local titulo = s(t.title)
    if titulo == "" then titulo = s(c.titulo, "Papel") end
    if not cp.write(c.nombre, titulo .. "\n\n" .. cuerpo) then
      return fallo("No se pudo", "No se pudo guardar el papel en la tarjeta.", filaVolver(st))
    end
    cp.view(c.nombre, titulo)
  elseif que == "recordar" then
    local x = trip.days[c.d] and trip.days[c.d].items[c.i]
    if x then
      x.remind = c.on
      guardarViaje()
    end
    cp.beep("ok")
  elseif que == "packing" then
    llegoPacking(t, c.volver)
  elseif que == "sugerir" then
    sug = {}
    for _, x in ipairs(type(t.suggestions) == "table" and t.suggestions or {}) do
      local texto = s(x)
      if texto ~= "" then sug[#sug + 1] = {t = texto, marcada = false} end
    end
    if #sug == 0 then return fallo("Sin sugerencias", "El servidor no sugirió nada.", filaVolver("lista")) end
    st, sel = "sug", 1
  elseif que == "guia.preguntas" then
    guia.preguntas = {}
    for _, q in ipairs(type(t.questions) == "table" and t.questions or {}) do
      if type(q) == "table" and s(q.text) ~= "" then
        guia.preguntas[#guia.preguntas + 1] = {key = s(q.key, "q" .. (#guia.preguntas + 1)), text = s(q.text)}
      end
    end
    guia.q = 0
    guiaPreguntar()
  elseif que == "guia.generar" then
    local jid = s(t.jobId)
    if jid == "" then
      guia.fase = "preguntas"
      return fallo("No se pudo", "El servidor no devolvió el trabajo.",
                   filasReintento("guia.reintentar", volverGuia()))
    end
    guia.job, guia.jobDate, guia.jobTrip = jid, guia.date, trip.id
    guia.fase = "generando"
    guardar()
    ultimoPoll = cp.ms()
  elseif que == "guia.job" then
    llegoJob(t)
  elseif que == "guia.dia" then
    llegoGuia(t)
  elseif que == "preguntar" then
    local respuesta = txt(t.answer)
    if respuesta == "" then return fallo("Sin respuesta", "El servidor no contestó nada.", filaVolver(st)) end
    if not cp.write(RESPUESTA, s(c.q) .. "\n\n" .. respuesta) then
      return fallo("No se pudo", "No se pudo guardar la respuesta en la tarjeta.", filaVolver(st))
    end
    -- La voz sale primero y el visor encima: la app sigue mientras suena.
    local spoken = s(t.spoken)
    if spoken ~= "" and type(cp.say) == "function" then
      local sid = cp.say(spoken)
      if sid then calls[sid] = {que = "decir"} end
    end
    cp.view(RESPUESTA, "Respuesta")
  end
end

-- ---------------------------------------------------------------- dibujo

local function cabezal(t)
  local w = cp.width()
  cp.text(SIDE, 24, fit(t, w - 2 * SIDE, 14), 14, true)
  cp.line(SIDE, 64, w - SIDE, 64, 1)
  return 80
end

local function parrafo(y, t, tam, max, negrita)
  local paso = cp.texth(tam)
  for _, l in ipairs(wrap(t, cp.width() - 2 * SIDE, tam, max)) do
    cp.text(SIDE, y, l, tam, negrita)
    y = y + paso
  end
  return y
end

local function fila(it, y, elegida)
  local w = cp.width()
  if elegida then cp.selection(SIDE, y, w - 2 * SIDE, it.h) end
  local x, der = SIDE + PAD, w - SIDE - PAD
  if it.casilla ~= nil then
    cp.rect(x, y + 15, 18, 18, false, 2)
    if it.casilla then cp.rect(x + 5, y + 20, 8, 8, true) end
    x = x + 18 + 14
  end
  local anchoT = der - x
  if it.m then
    local mw = cp.textw(it.m, 10)
    cp.text(der - mw, y + 11, it.m, 10)
    anchoT = anchoT - mw - 16
  end
  cp.text(x, y + (it.d and 8 or 10), fit(it.t, anchoT, 12), 12)
  if it.d then cp.text(x, y + 37, fit(it.d, der - x, 10), 10) end
  cp.line(SIDE, y + it.h - 1, w - SIDE, y + it.h - 1, 1)
end

-- Reparte las filas en páginas por alto y devuelve la lista de {desde, hasta}.
local function paginas(f, alto)
  local p, ini, usado = {}, 1, 0
  for i, it in ipairs(f) do
    if usado + it.h > alto and i > ini then
      p[#p + 1] = {ini, i - 1}
      ini, usado = i, 0
    end
    usado = usado + it.h
  end
  p[#p + 1] = {ini, #f}
  return p
end

-- Dibuja la lista desde y0 hasta yFin, con la página donde cae `sel`, y el
-- paginador "Página x de y" si hay más de una.
local function lista(f, y0, yFin)
  if #f == 0 then return end
  local p = paginas(f, yFin - y0)
  local actual = 1
  for i, r in ipairs(p) do
    if sel >= r[1] and sel <= r[2] then actual = i end
  end
  local y = y0
  for i = p[actual][1], p[actual][2] do
    fila(f[i], y, i == sel)
    y = y + f[i].h
  end
  if #p > 1 then
    local w = cp.width()
    local t = "Página " .. actual .. " de " .. #p
    local ty = cp.height() - 80
    cp.text(SIDE, ty, t, 10)
    local bx = SIDE + cp.textw(t, 10) + 16
    local bw = w - SIDE - bx
    cp.line(bx, ty + 12, bx + bw, ty + 12, 1)
    cp.rect(bx + bw * (actual - 1) // #p, ty + 11, math.max(4, bw // #p), 3, true)
  end
end

local function pie(t)
  if cp.busy() then t = "Esperando al servidor…" end
  cp.text(SIDE, cp.height() - 40, fit(t, cp.width() - 2 * SIDE, 10), 10)
end

local function barra(y, paso, total)
  local w = cp.width() - 2 * SIDE
  cp.rect(SIDE, y, w, 12, false, 1)
  if total > 0 then cp.rect(SIDE + 2, y + 2, math.max(0, math.min(w - 4, (w - 4) * paso // total)), 8, true) end
end

-- El día que la guía tiene apuntado, para los cabezales del flujo.
local function diaDeLaGuia()
  local d = trip.days[guia.di]
  return d and ("día " .. d.n) or "día"
end

local function dibujarGuia(fin)
  if guia.fase == "lista" then
    local y = cabezal("Guía · " .. trip.name)
    lista(filas(), y + 8, fin)
    pie("OK: leer o armar la guía del día · Atrás: volver")
  elseif guia.fase == "preguntas" then
    local y = cabezal("Guía del " .. diaDeLaGuia())
    local p = guia.preguntas[guia.q]
    if p then
      cp.text(SIDE, y + 8, "Pregunta " .. guia.q .. " de " .. #guia.preguntas, 10)
      parrafo(y + 32, p.text, 14, 4, true)
    else
      parrafo(y + 8, "Antes de armar la guía, unas preguntas por voz sobre lo que falta de ese día.", 12, 4)
    end
    pie("Atrás: saltar la pregunta")
  elseif guia.fase == "generando" then
    local y = cabezal("Armando la guía del " .. diaDeLaGuia() .. "…")
    local paso = guia.total > 0 and ("Paso " .. guia.step .. " de " .. guia.total) or "Preparando"
    cp.text(SIDE, y + 16, fit(paso, cp.width() - 2 * SIDE, 12), 12)
    y = parrafo(y + 16 + cp.texth(12), guia.label, 10, 2)
    barra(y + 16, guia.step, guia.total)
    pie("Tarda unos minutos · Atrás: salir; se retoma al volver")
  elseif guia.fase == "bajando" then
    local y = cabezal("Bajando la guía del " .. diaDeLaGuia())
    cp.text(SIDE, y + 16, "Se guarda en la tarjeta y se abre en el visor.", 12)
    pie("Esperando al servidor…")
  end
end

function on_draw()
  local fin = cp.height() - 88
  if st == "sin" then
    local y = cabezal("Viajes")
    y = parrafo(y + 8, "No hay viajes cargados. Cárgalos en la web: /board → Viajes, y después elige Actualizar.",
                12, 4)
    lista(filas(), y + 16, fin)
    pie("OK: actualizar · Atrás: salir")
  elseif st == "inicio" then
    local y = cabezal(trip.name)
    cp.text(SIDE, y, fit(subtitulo(), cp.width() - 2 * SIDE, 12), 12)
    y = y + cp.texth(12) + 4
    if trip.place ~= "" then
      cp.text(SIDE, y, fit(trip.place, cp.width() - 2 * SIDE, 10), 10)
      y = y + cp.texth(10) + 4
    end
    lista(filas(), y + 12, fin)
    pie("OK: abrir · Atrás: salir")
  elseif st == "hoy" or st == "dia" then
    local d = trip.days[dia]
    if not d then
      cabezal("Día")
    else
      local y = cabezal(d.label)
      cp.text(SIDE, y, "Día " .. d.n .. " de " .. #trip.days, 10)
      y = y + cp.texth(10) + 4
      -- El lugar y el hotel del día: "Barcelona · Hotel Praktik". Sin lugar, nada.
      local donde = junta(d.place, d.hotel)
      if donde ~= "" then
        cp.text(SIDE, y, fit(donde, cp.width() - 2 * SIDE, 12), 12)
        y = y + cp.texth(12) + 4
      end
      if d.note ~= "" then y = parrafo(y + 4, d.note, 12, 3) end
      lista(filas(), y + 12, fin)
    end
    pie("OK: abrir · Atrás: volver")
  elseif st == "agenda" then
    local y = cabezal("Agenda · " .. trip.name)
    lista(filas(), y + 8, fin)
    pie("OK: abrir · Atrás: volver")
  elseif st == "item" then
    local d = trip.days[item.d]
    local it = d.items[item.i]
    local y = cabezal(it.title)
    local cuando = "Día " .. d.n .. " · " .. d.label
    if it.at ~= "" then cuando = cuando .. " · " .. it.at end
    cp.text(SIDE, y, fit(cuando, cp.width() - 2 * SIDE, 12), 12)
    y = y + cp.texth(12) + 4
    local donde = junta(it.kindLabel, it.place)
    if donde ~= "" then
      cp.text(SIDE, y, fit(donde, cp.width() - 2 * SIDE, 12), 12)
      y = y + cp.texth(12) + 4
    end
    if it.code ~= "" then
      cp.text(SIDE, y + 8, "Código", 10)
      y = y + cp.texth(10) + 8
      cp.text(SIDE, y, fit(it.code, cp.width() - 2 * SIDE, 14), 14, true)
      y = y + cp.texth(14) + 8
    end
    if it.note ~= "" then y = parrafo(y + 4, it.note, 12, 4) end
    lista(filas(), y + 16, fin)
    pie("OK: elegir · Atrás: volver")
  elseif st == "papeles" then
    local y = cabezal("Papeles · " .. #trip.papers)
    lista(filas(), y + 8, fin)
    pie("OK: ver el papel · Atrás: volver")
  elseif st == "lista" then
    local hechos, total = llevar()
    local y = cabezal("Para llevar · " .. hechos .. " de " .. total)
    lista(filas(), y + 8, fin)
    pie("OK: tildar · Atrás: volver")
  elseif st == "sug" then
    local y = cabezal("Sugerencias")
    cp.text(SIDE, y + 8, "Marca las que quieras y elige Agregar las marcadas.", 10)
    lista(filas(), y + 40, fin)
    pie("OK: marcar · Atrás: volver sin agregar")
  elseif st == "guia" then
    dibujarGuia(fin)
  elseif st == "actualizar" then
    local y = cabezal("Actualizando…")
    local paso
    if act.fase == "pendientes" then
      paso = "Subiendo los tildes pendientes · " .. act.hechos .. " de " .. act.total
    elseif act.fase == "lista" then
      paso = "Bajando la lista de viajes"
    else
      paso = "Bajando el viaje"
    end
    cp.text(SIDE, y + 16, fit(paso, cp.width() - 2 * SIDE, 12), 12)
    pie("Atrás: cancelar")
  elseif st == "cambiar" then
    local y = cabezal("Cambiar de viaje")
    if #viajes == 0 and not cp.busy() then cp.text(SIDE, y + 8, "No hay viajes en la cuenta.", 12) end
    lista(filas(), y + 8, fin)
    pie("OK: hacerlo el activo · Atrás: volver")
  elseif st == "error" then
    local y = cabezal(err.titulo)
    y = parrafo(y + 8, err.cuerpo, 12, 8)
    lista(filas(), y + 16, fin)
    pie("OK: elegir · Atrás: volver")
  end
end
