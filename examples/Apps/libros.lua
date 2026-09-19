-- LIBRARY: dime un título y te lo bajo.
-- Dicta un título o un autor, el servidor se lo pide a un bot de Telegram
-- (como la cuenta del dueño, conectada desde /board) y devuelve la lista;
-- eliges uno, ves la ficha y "Bajar EPUB" baja el archivo a /Books/libros/.
-- Lo bajado queda anotado en bajados.json y se abre desde Inicio.
-- El transcriptor no conoce a los autores ("angeles mastretas"): si el bot no
-- encuentra nada, el servidor corrige el nombre con el modelo y busca otra vez
-- (`corrected`), y la pantalla sin resultados ofrece "Deletrear el nombre":
-- se dice letra por letra y va con `spelled = true` (`spelled` = la palabra).
--
-- La app NO levanta la red al abrir: la primera cp.listen / cp.call lo hace.
-- Pantallas: inicio → (escucha) → resultados → ficha → bajando → listo (o error).
-- Contrato: docs/ws397/LIBROS_CONTRATO.md. Estilo y helpers: librito.lua.
--
-- REGLA: todo lo que entra por cp.read, cp.load o el servidor pasa por s() o
-- n() antes de concatenarse, compararse, indexarse o medirse.

local SIDE, PAD = 24, 24          -- margen de la pantalla y borde de fila -> texto
local ROW1, ROW2 = 48, 72         -- fila de uno y de dos renglones
local CAB = 32                    -- renglón de sección (no se elige)
local POLL_MS = 3000
local BAJADOS = "bajados.json"
local FICHA_TXT = "ficha.txt"
local MAX_BAJADOS = 30
local PREGUNTA = "¿Qué libro buscas?"
local PREGUNTA_LETRAS = "Deletrea letra por letra"
local SIN_TELEGRAM = "Conecta Telegram en la web (Ajustes → Avanzado → Telegram)"

local st = "inicio"
local sel = 1
local bajados = {}                -- {name, title, author, at}
local q = ""                      -- lo dictado
local porLetras = false           -- q es el nombre deletreado (spelled = true)
local escuchandoLetras = false    -- la escucha abierta es la de deletrear
local corregido = ""              -- con qué buscó el servidor si corrigió lo dicho
local deletreado = ""             -- la palabra que armó el servidor con las letras
local res = {}                    -- {title, code}
local elegido = 0                 -- índice en res del que se abrió
local ficha = nil                 -- {code, title, author, year, pages, genre, desc, formats}
local job = {}                    -- {id, format, step, total, label, file}
local libro = nil                 -- nombre del archivo ya bajado (en /Books/libros/)
local calls = {}                  -- id de cp.call/cp.download -> para qué se pidió
local err = {titulo = "", cuerpo = "", filas = {}, volver = "inicio", re = nil}
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
-- No hay JSON en el cajón; bajados.json es una lista chica. Mismo codificador
-- que viajes.lua: patrones (corren en C) y nada de `error` ni `pcall`, que se
-- tragarían la guardia de instrucciones. Un JSON malo devuelve nil.
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

-- ---------------------------------------------------------------- texto

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

-- El nombre con el que se guarda en la tarjeta. El bot manda cualquier cosa
-- ("Cien años de soledad.epub") y cp.download sólo acepta [A-Za-z0-9._-], 48
-- como mucho y sin punto inicial: se transliteran los acentos, el resto pasa a
-- guion y la extensión sale del nombre o, si no trae, del formato pedido.
local ACENTOS = {["á"] = "a", ["é"] = "e", ["í"] = "i", ["ó"] = "o", ["ú"] = "u", ["ü"] = "u", ["ñ"] = "n",
                 ["Á"] = "A", ["É"] = "E", ["Í"] = "I", ["Ó"] = "O", ["Ú"] = "U", ["Ü"] = "U", ["Ñ"] = "N",
                 ["à"] = "a", ["è"] = "e", ["ì"] = "i", ["ò"] = "o", ["ù"] = "u", ["ç"] = "c", ["ã"] = "a",
                 ["õ"] = "o", ["â"] = "a", ["ê"] = "e", ["ô"] = "o", ["ä"] = "a", ["ö"] = "o", ["ß"] = "ss"}
local function nombreSeguro(nombre, formato)
  local t = s(nombre)
  local base, ext = t:match("^(.*)%.([%w]+)$")
  if not base then base, ext = t, "" end
  ext = ext:lower()
  if ext == "" or #ext > 5 then ext = s(formato, "epub"):lower():gsub("[^%w]", "") end
  if ext == "" then ext = "epub" end
  for a, b in pairs(ACENTOS) do base = base:gsub(a, b) end
  base = base:gsub("[^%w._%-]+", "-"):gsub("%-%-+", "-"):gsub("^[%-.]+", ""):gsub("[%-.]+$", "")
  if base == "" then base = "libro" end
  local tope = 48 - #ext - 1
  if #base > tope then base = base:sub(1, tope):gsub("[%-.]+$", "") end
  if base == "" then base = "libro" end
  return base .. "." .. ext
end

-- ---------------------------------------------------------------- bajados

local function fechaHoy()
  local t = cp.time()
  if type(t) == "table" and n(t.year) > 0 then
    return string.format("%04d-%02d-%02d", n(t.year), n(t.month), n(t.day))
  end
  return ""
end

-- Lee bajados.json y se queda sólo con lo que tiene nombre: lo que llegó a la
-- tarjeta no es necesariamente lo que el código supone.
local function cargarBajados()
  bajados = {}
  local lista = json.decode(cp.read(BAJADOS))
  if type(lista) ~= "table" then return end
  for _, e in ipairs(lista) do
    if type(e) == "table" and s(e.name) ~= "" and #bajados < MAX_BAJADOS then
      bajados[#bajados + 1] = {name = s(e.name), title = s(e.title, s(e.name)), author = s(e.author),
                              at = s(e.at)}
    end
  end
end

local function guardarBajados()
  if not cp.write(BAJADOS, json.encode(bajados)) then cp.log("libros: no se pudo escribir " .. BAJADOS) end
end

-- Lo más nuevo primero, sin repetir el mismo archivo, MAX_BAJADOS como mucho.
local function anotarBajado(name, title, author)
  local nuevos = {{name = name, title = title, author = author, at = fechaHoy()}}
  for _, e in ipairs(bajados) do
    if e.name ~= name and #nuevos < MAX_BAJADOS then nuevos[#nuevos + 1] = e end
  end
  bajados = nuevos
  guardarBajados()
end

local function quitarBajado(i)
  if not bajados[i] then return false end
  table.remove(bajados, i)
  guardarBajados()
  return true
end

-- ---------------------------------------------------------------- pedidos

local function pedir(servicio, args, que)
  local id = cp.call(servicio, args)
  if not id then
    cp.beep("error")
    return false
  end
  calls[id] = que
  return true
end

local function escuchar()
  escuchandoLetras = false
  if not cp.listen(10, PREGUNTA) then
    cp.beep("error")
    return false
  end
  return false  -- el host muestra su propia pantalla de escucha
end

-- Deletrear: más tiempo (un nombre son veinte letras) y el servidor arma la palabra.
local function escucharLetras()
  escuchandoLetras = true
  if not cp.listen(20, PREGUNTA_LETRAS) then
    escuchandoLetras = false
    cp.beep("error")
    return false
  end
  return false
end

local function buscar()
  return pedir("libros.buscar", porLetras and {q = q, spelled = true} or {q = q}, "buscar")
end
local function pedirFicha(code) return pedir("libros.ficha", {code = code}, "ficha") end
local function pedirBajar(formato)
  if not ficha then return false end
  job = {format = formato, step = 0, total = 0, label = ""}
  return pedir("libros.bajar", {code = ficha.code, format = formato}, "bajar")
end
local function descargar()
  if not job.file then return false end
  local id = cp.download(job.file.id, job.file.name, "books")
  if not id then
    cp.beep("error")
    return false
  end
  calls[id] = "descarga"
  return true
end

-- Error con "Reintentar" (`re` dice qué repetir) y "Volver" (a `volver`).
local function fallo(tit, cuerpo, volver, re)
  local filas = {}
  if re then filas[#filas + 1] = {t = "Reintentar", h = ROW1, act = "reintentar"} end
  filas[#filas + 1] = {t = "Volver", h = ROW1, act = "volver"}
  err = {titulo = tit, cuerpo = cuerpo, filas = filas, volver = volver, re = re}
  st, sel = "error", 1
  cp.beep("error")
end

-- Un error del servidor: sin Telegram conectado tiene su propio cartel.
local function errorServidor(t, volver, re)
  local texto = s(t.error)
  if s(t.code) == "no_telegram" or texto:lower():find("telegram", 1, true) then
    return fallo("Sin Telegram", SIN_TELEGRAM, volver, re)
  end
  if texto == "" then texto = "Sin conexión con el servidor" end
  fallo("No se pudo", texto, volver, re)
end

local function reintentar()
  local re = err.re
  if re == "buscar" then
    st = "inicio"
    return buscar()
  elseif re == "ficha" then
    st, sel = "resultados", math.max(1, elegido)
    return res[elegido] and pedirFicha(res[elegido].code) or false
  elseif re == "bajar" then
    st = "ficha"
    return pedirBajar(s(job.format, "epub"))
  elseif re == "job" then
    st, sel = "bajando", 1
    ultimoPoll = cp.ms() - POLL_MS  -- que el próximo tick pregunte ya
    return true
  elseif re == "descarga" then
    st, sel = "bajando", 1
    return descargar()
  end
  return false
end

function on_open()
  st, sel, q, res, elegido, ficha, job, libro, calls = "inicio", 1, "", {}, 0, nil, {}, nil, {}
  porLetras, escuchandoLetras, corregido, deletreado = false, false, "", ""
  cargarBajados()
end

-- ---------------------------------------------------------------- filas

local function metaFicha()
  local partes = {}
  if ficha.year > 0 then partes[#partes + 1] = tostring(ficha.year) end
  if ficha.pages > 0 then partes[#partes + 1] = ficha.pages .. " páginas" end
  if ficha.genre ~= "" then partes[#partes + 1] = ficha.genre end
  return table.concat(partes, " · ")
end

-- Las filas de la pantalla de turno. Cada una sabe su alto y qué hace OK;
-- `cab` es un renglón de sección que la palanca saltea.
local function filas()
  local f = {}
  if st == "inicio" then
    f[1] = {t = "Buscar por voz", h = ROW1, act = "dictar"}
    if #bajados > 0 then
      f[#f + 1] = {t = "Bajados", h = CAB, cab = true}
      for i, b in ipairs(bajados) do
        local d = b.author
        if b.at ~= "" then d = d == "" and b.at or (d .. " · " .. b.at) end
        f[#f + 1] = {t = b.title, d = d ~= "" and d or b.name, h = ROW2, act = "abrirBajado", i = i}
      end
    end
  elseif st == "resultados" then
    if #res == 0 then
      f[1] = {t = "Deletrear el nombre", h = ROW1, act = "deletrear"}
      f[2] = {t = "Buscar de nuevo", h = ROW1, act = "dictar"}
    else
      for i, r in ipairs(res) do f[#f + 1] = {t = r.title, h = ROW1, act = "elegir", i = i} end
    end
  elseif st == "ficha" and ficha then
    for _, fm in ipairs(ficha.formats) do
      f[#f + 1] = {t = "Bajar " .. fm:upper(), h = ROW1, act = "bajar", fm = fm}
    end
    if #ficha.formats == 0 then f[#f + 1] = {t = "Este bot no da el archivo", h = ROW1} end
    if ficha.desc ~= "" then f[#f + 1] = {t = "Leer la descripción", h = ROW1, act = "desc"} end
    f[#f + 1] = {t = "Otra búsqueda", h = ROW1, act = "dictar"}
  elseif st == "listo" then
    f[1] = {t = "Abrir en el lector", h = ROW1, act = "abrir"}
    f[2] = {t = "Buscar otro", h = ROW1, act = "dictar"}
  elseif st == "error" then
    f = err.filas
  end
  return f
end

local function abrirLibro(nombre)
  if cp.open_book(nombre) then return true end
  fallo("No está el libro", "No se encontró " .. nombre .. " en /Books/libros/ de la tarjeta.", st)
  return true
end

local function accion(it)
  local a = it and it.act
  if a == "dictar" then
    return escuchar()
  elseif a == "deletrear" then
    return escucharLetras()
  elseif a == "abrirBajado" then
    local b = bajados[it.i]
    if not b then return false end
    if cp.open_book(b.name) then return true end
    -- Ya no está en la tarjeta: se saca de la lista y se dice.
    quitarBajado(it.i)
    sel = 1
    fallo("No está el libro", "Ya no está " .. b.name .. " en /Books/libros/ de la tarjeta. " ..
          "Se quitó de la lista.", "inicio")
    return true
  elseif a == "elegir" then
    local r = res[it.i]
    if not r then return false end
    elegido = it.i
    return pedirFicha(r.code)
  elseif a == "bajar" then
    return pedirBajar(it.fm)
  elseif a == "desc" then
    local cuerpo = ficha.title .. "\n" .. ficha.author .. "\n" .. metaFicha() .. "\n\n" .. ficha.desc
    if not cp.write(FICHA_TXT, cuerpo) or not cp.view(FICHA_TXT, ficha.title) then
      cp.beep("error")
      return false
    end
    return true
  elseif a == "abrir" then
    return abrirLibro(libro or "")
  elseif a == "reintentar" then
    return reintentar()
  elseif a == "volver" then
    st, sel = err.volver, 1
    return true
  end
  cp.beep("error")
  return false
end

local function atras()
  if st == "inicio" then
    cp.quit()
    return false
  elseif st == "resultados" then st, porLetras = "inicio", false
  elseif st == "ficha" then
    st, sel = "resultados", math.max(1, elegido)  -- vuelve sobre el elegido
    cp.beep("back")
    return true
  elseif st == "bajando" then
    job = {}  -- se abandona el trabajo; el servidor lo poda solo
    st = "ficha"
  elseif st == "listo" then
    st, q, res, elegido, ficha, libro = "inicio", "", {}, 0, nil, nil
  elseif st == "error" then st = err.volver
  end
  sel = 1
  cp.beep("back")
  return true
end

local function mover(f, dir)
  local k = sel
  for _ = 1, #f do
    k = k + dir
    if k < 1 then k = #f elseif k > #f then k = 1 end
    if not f[k].cab then
      sel = k
      return true
    end
  end
  return false
end

function on_key(k)
  -- Con algo en curso el host sólo manda "back", y lo cancela él. Nunca salir.
  if cp.busy() then return k == "back" end
  local f = filas()
  if k == "up" or k == "down" then
    if #f <= 1 then return false end
    if not mover(f, k == "up" and -1 or 1) then return false end
    cp.beep("nav")
    return true
  elseif k == "ok" then
    return accion(f[sel])
  elseif k == "back" then
    return atras()
  elseif k == "backlong" then
    -- Atrás mantenido sobre un bajado lo saca de la lista (no borra el archivo).
    -- Hoy el firmware no manda esta tecla (Atrás largo sale de la app);
    -- queda listo para cuando lo haga.
    local it = f[sel]
    if st == "inicio" and it and it.act == "abrirBajado" and quitarBajado(it.i) then
      local nf = filas()
      if sel > #nf then sel = #nf end
      if nf[sel].cab then sel = 1 end
      cp.beep("back")
      return true
    end
    return false
  end
  return false
end

function on_tick()
  if st == "bajando" and job.id and not cp.busy() and cp.ms() - ultimoPoll >= POLL_MS then
    ultimoPoll = cp.ms()
    pedir("job.status", {id = job.id}, "job")
  end
  return false  -- el host repinta solo cuando llega la respuesta
end

function on_heard(texto)
  local letras = escuchandoLetras
  escuchandoLetras = false
  if type(texto) ~= "string" or texto:match("^%s*$") then return end
  q, porLetras, corregido, deletreado = s(texto), letras, "", ""
  st, sel = "inicio", 1
  buscar()
end

-- ---------------------------------------------------------------- respuestas

local function llegoBusqueda(t)
  local lista = type(t.results) == "table" and t.results or {}
  corregido, deletreado = s(t.corrected), s(t.spelled)
  res = {}
  for _, r in ipairs(lista) do
    if type(r) == "table" and s(r.title) ~= "" and s(r.code) ~= "" then
      res[#res + 1] = {title = s(r.title), code = s(r.code)}
    end
  end
  st, sel = "resultados", 1
  if #res == 0 then cp.beep("error") end
end

-- Los formatos que el bot ofrece, en minúsculas, sin repetir y epub primero.
local function formatos(lista)
  local out, visto = {}, {}
  if type(lista) ~= "table" then return out end
  for _, fm in ipairs(lista) do
    fm = s(fm):lower():gsub("[^%w]", "")
    if fm ~= "" and not visto[fm] then
      visto[fm] = true
      if fm == "epub" then table.insert(out, 1, fm) else out[#out + 1] = fm end
    end
  end
  return out
end

local function llegoFicha(t, code)
  local r = res[elegido]
  ficha = {code = code, title = s(t.title, r and r.title or "Sin título"), author = s(t.author),
           year = n(t.year), pages = n(t.pages), genre = s(t.genre), desc = txt(t.desc):sub(1, 2048),
           formats = formatos(t.formats)}
  if ficha.title == "" then ficha.title = "Sin título" end
  st, sel = "ficha", 1
end

local function llegoJob(t)
  local estado = s(t.state)
  if estado == "done" then
    local f = type(t.files) == "table" and t.files[1] or nil
    if type(f) ~= "table" or s(f.id) == "" then
      job = {}
      return fallo("No se pudo", "El servidor terminó pero no entregó el archivo.", "ficha", "bajar")
    end
    -- Sin nombre del bot, el título de la ficha con el formato pedido.
    local nombre = s(f.name)
    if nombre == "" then nombre = ficha and ficha.title or "" end
    job.file = {id = s(f.id), name = nombreSeguro(nombre, job.format)}
    job.step, job.total, job.label = n(t.step), n(t.total), ""  -- el label viejo ya no vale
    if not descargar() then fallo("No se pudo", "No se pudo empezar la descarga.", "ficha", "descarga") end
  elseif estado == "failed" then
    job.id = nil
    fallo("No se pudo bajar", s(t.error, "El bot no entregó el archivo."), "ficha", "bajar")
  else
    job.step, job.total, job.label = n(t.step), n(t.total), s(t.label)
    ultimoPoll = cp.ms()
  end
end

function on_reply(id, ok, t)
  local que = calls[id]
  calls[id] = nil
  if not que then return end
  if type(t) ~= "table" then t = {} end
  if not ok then
    if s(t.error) == "cancelado" then return end  -- se queda donde estaba
    if que == "buscar" then return errorServidor(t, "inicio", "buscar") end
    if que == "ficha" then return errorServidor(t, "resultados", "ficha") end
    if que == "bajar" then return errorServidor(t, "ficha", "bajar") end
    if que == "job" then return errorServidor(t, "ficha", "job") end
    if que == "descarga" then return errorServidor(t, "ficha", "descarga") end
    return errorServidor(t, "inicio")
  end
  if que == "buscar" then
    llegoBusqueda(t)
  elseif que == "ficha" then
    llegoFicha(t, res[elegido] and res[elegido].code or "")
  elseif que == "bajar" then
    local jid = s(t.jobId)
    if jid == "" then return fallo("No se pudo", "El servidor no devolvió el trabajo.", "ficha", "bajar") end
    job.id = jid
    st, sel = "bajando", 1
    ultimoPoll = cp.ms()
  elseif que == "job" then
    llegoJob(t)
  elseif que == "descarga" then
    libro = job.file and job.file.name or nil
    if libro and ficha then anotarBajado(libro, ficha.title, ficha.author) end
    job = {}
    st, sel = "listo", 1
    cp.beep("ok")
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
  if it.cab then
    cp.text(SIDE, y + 12, it.t, 10, true)
    return
  end
  if elegida then cp.selection(SIDE, y, w - 2 * SIDE, it.h) end
  local x, der = SIDE + PAD, w - SIDE - PAD
  cp.text(x, y + (it.d and 8 or 10), fit(it.t, der - x, 12), 12)
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

function on_draw()
  local fin = cp.height() - 88
  if st == "inicio" then
    local y = cabezal("LIBRARY")
    y = parrafo(y + 8, "Presiona OK y di el título o el autor. El servidor se lo pide al bot y el libro " ..
                "queda en la tarjeta, en Leer.", 12, 4)
    lista(filas(), y + 16, fin)
    pie(#bajados > 0 and "OK: buscar o abrir · Atrás: salir" or "OK: buscar por voz · Atrás: salir")
  elseif st == "resultados" then
    -- Deletreado, el cabezal es la palabra armada y no la ristra de letras.
    local dicho = deletreado ~= "" and deletreado or q
    local y = cabezal(dicho ~= "" and dicho or "Resultados")
    local ancho = cp.width() - 2 * SIDE
    if deletreado ~= "" then
      cp.text(SIDE, y + 8, fit("Deletreado: " .. deletreado, ancho, 10), 10)
      y = y + cp.texth(10) + 4
    end
    if corregido ~= "" then
      cp.text(SIDE, y + 8, fit("Buscando «" .. corregido .. "»", ancho, 10), 10)
      y = y + cp.texth(10) + 4
    end
    if #res == 0 then
      y = parrafo(y + 8, "No encontré nada con «" .. (corregido ~= "" and corregido or dicho) .. "». " ..
                  "Puedes deletrear el nombre letra por letra.", 12, 4)
      lista(filas(), y + 16, fin)
      pie("OK: elegir · Atrás: volver")
    else
      cp.text(SIDE, y + 8, #res .. (#res == 1 and " resultado" or " resultados"), 10)
      lista(filas(), y + 40, fin)
      pie("Palanca: elegir · OK: ver la ficha · Atrás: volver")
    end
  elseif st == "ficha" and ficha then
    local y = cabezal(ficha.title)
    if ficha.author ~= "" then
      cp.text(SIDE, y + 8, fit(ficha.author, cp.width() - 2 * SIDE, 12), 12)
      y = y + cp.texth(12) + 4
    end
    local meta = metaFicha()
    if meta ~= "" then
      cp.text(SIDE, y + 8, fit(meta, cp.width() - 2 * SIDE, 10), 10)
      y = y + cp.texth(10) + 4
    end
    if ficha.desc ~= "" then y = parrafo(y + 16, ficha.desc, 10, 5) end
    lista(filas(), y + 24, fin)
    pie("OK: elegir · Atrás: volver a la lista")
  elseif st == "bajando" then
    local y = cabezal("Bajando…")
    y = parrafo(y + 8, ficha and ficha.title or "", 14, 3, true)
    local paso = s(job.label)
    if paso == "" then paso = job.file and "Bajando el archivo a la tarjeta" or "Pidiendo el libro al bot" end
    y = parrafo(y + 16, paso, 12, 2)
    local w = cp.width() - 2 * SIDE
    cp.rect(SIDE, y + 16, w, 12, false, 1)
    if n(job.total) > 0 then
      cp.rect(SIDE + 2, y + 18, math.max(0, math.min(w - 4, (w - 4) * n(job.step) // n(job.total))), 8, true)
      cp.text(SIDE, y + 40, "Paso " .. n(job.step) .. " de " .. n(job.total), 10)
    end
    pie("Atrás: cancelar y volver a la ficha")
  elseif st == "listo" then
    local y = cabezal("Listo")
    y = parrafo(y + 8, ficha and ficha.title or s(libro), 14, 3, true)
    y = parrafo(y + 8, "Guardado en /Books/libros/" .. s(libro), 10, 2)
    lista(filas(), y + 24, fin)
    pie("OK: elegir · Atrás: inicio")
  elseif st == "error" then
    local y = cabezal(err.titulo)
    y = parrafo(y + 8, err.cuerpo, 12, 8)
    lista(filas(), y + 16, fin)
    pie("OK: elegir · Atrás: volver")
  end
end
