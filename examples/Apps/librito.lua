-- Librito: dicta un tema y el servidor escribe un libro corto (unos 15 minutos
-- de lectura) que se abre en el lector. Muestra el contrato asíncrono de las
-- apps: cp.listen / cp.call / cp.download piden algo y la respuesta llega por
-- on_heard y on_reply; el trabajo largo (escribir) se consulta desde on_tick
-- cada 5 s y sobrevive a salir de la app gracias a cp.save.
--
-- Pantallas: inicio → tema → enfoque → indice → escribiendo → listo (o error).
-- Contrato: docs/ws397/PLAN_APPS_VIAJES_EPUB.md, "Contrato v1".

local SIDE, PAD = 24, 24          -- margen de la pantalla y borde de fila -> texto
local ROW1, ROW2 = 48, 72         -- fila de uno y de dos renglones
local POLL_MS = 5000
local PALABRAS_MIN = 200          -- palabras por minuto de lectura
local MIN_MINUTOS = 15
local PEDIDO_MAS = "propón más capítulos sobre otros aspectos del tema, sin quitar los que están"

local st = "inicio"
local sel = 1
local tema = ""
local enfoques = {}               -- {id, titulo, linea}
local enfoque = ""                -- el elegido o el dictado, como texto
local titulo = ""
local caps = {}                   -- {titulo, linea, palabras, activo}
local job = {}                    -- {id, titulo, slug, step, total, label, file}
local pend = nil                  -- trabajo guardado de otra vez: {id, titulo, slug}
local libro = nil                 -- nombre del .epub ya bajado
local calls = {}                  -- id de cp.call -> para qué se pidió
local oir = nil                   -- para qué se escucha: "tema" | "enfoque" | "ajuste"
local err = {titulo = "", cuerpo = "", filas = {}, volver = "inicio"}
local ultimoPoll = 0

-- Todo lo que viene del servidor pasa por acá: nunca un nil donde va un texto.
local function s(v, def)
  if type(v) == "string" then return (v:gsub("[\r\n]+", " ")) end
  if type(v) == "number" then return tostring(v) end
  return def or ""
end
local function n(v, def)
  local x = tonumber(v)
  if x then return math.floor(x) end
  return def or 0
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

-- Estado guardado: una clave por renglón. No hay JSON en el cajón y esto alcanza.
local function guardar()
  if job.id then
    cp.save("job=" .. job.id .. "\ntitulo=" .. s(job.titulo) .. "\nslug=" .. s(job.slug) .. "\n")
  else
    cp.save("")
  end
end
local function cargar()
  pend = nil
  local d = cp.load()
  if type(d) ~= "string" then return end
  local t = {}
  for k, v in d:gmatch("(%w+)=([^\n]*)") do t[k] = v end
  if t.job and t.job ~= "" then pend = {id = t.job, titulo = t.titulo or "", slug = t.slug or ""} end
end

local function reset()
  st, sel, tema, enfoques, enfoque, titulo, caps = "inicio", 1, "", {}, "", "", {}
  job, libro, calls, oir = {}, nil, {}, nil
end

-- Capítulos activos, y los minutos que darían (nunca menos de MIN_MINUTOS).
local function cuenta()
  local activos, palabras = 0, 0
  for _, c in ipairs(caps) do
    if c.activo then
      activos = activos + 1
      palabras = palabras + c.palabras
    end
  end
  local crudo = math.floor(palabras / PALABRAS_MIN + 0.5)
  return activos, math.max(MIN_MINUTOS, crudo), crudo
end

local function pedir(servicio, args, que)
  local id = cp.call(servicio, args)
  if not id then
    cp.beep("error")
    return false
  end
  calls[id] = que
  return true
end

local function escuchar(que, pregunta)
  oir = que
  if not cp.listen(15, pregunta) then oir = nil end
  return false  -- el host muestra su propia pantalla de escucha
end

local function capsActivos()
  local lista = {}
  for _, c in ipairs(caps) do
    if c.activo then lista[#lista + 1] = {titulo = c.titulo, linea = c.linea, palabras = c.palabras} end
  end
  return lista
end

local function pedirIndice()
  return pedir("librito.indice", {tema = tema, enfoque = enfoque, minutos = MIN_MINUTOS}, "indice")
end

local function pedirAjuste(pedido)
  local lista = {}
  for _, c in ipairs(caps) do
    lista[#lista + 1] = {titulo = c.titulo, linea = c.linea, activo = c.activo and true or false}
  end
  return pedir("librito.ajustar",
               {tema = tema, enfoque = enfoque, capitulos = lista, pedido = pedido, minutos = MIN_MINUTOS},
               "indice")
end

local function pedirEscribir()
  local activos, minutos = cuenta()
  if activos == 0 then
    cp.beep("error")
    return false
  end
  return pedir("librito.escribir", {titulo = titulo, tema = tema, enfoque = enfoque,
                                    capitulos = capsActivos(), minutos = minutos}, "escribir")
end

local function bajar()
  if not job.file then return false end
  local id = cp.download(job.file.id, job.file.name, "books")
  if not id then
    cp.beep("error")
    return false
  end
  calls[id] = "bajar"
  return true
end

local function fallo(tit, cuerpo, filas, volver)
  err = {titulo = tit, cuerpo = cuerpo, filas = filas, volver = volver}
  st, sel = "error", 1
  cp.beep("error")
end
local function filaVolver(volver)
  return {{t = "Volver", h = ROW1, act = "volver"}}, volver
end
local function filasJob(reintento)
  return {{t = "Reintentar", h = ROW1, act = reintento}, {t = "Descartar el trabajo", h = ROW1, act = "inicio"}}
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

function on_open()
  reset()
  cargar()
end

-- Las filas de la pantalla de turno. Cada una sabe su alto y qué hace OK.
local function filas()
  local f = {}
  if st == "inicio" then
    if pend then
      local t = s(pend.titulo)
      if t == "" then t = "libro pendiente" end
      f[#f + 1] = {t = "Retomar: " .. t, d = "Se sigue escribiendo en el servidor", h = ROW2, act = "retomar"}
    end
    f[#f + 1] = {t = "Dictar el tema", h = ROW1, act = "dictar"}
  elseif st == "tema" then
    f[1] = {t = "Seguir", h = ROW1, act = "seguir"}
    f[2] = {t = "Dictar de nuevo", h = ROW1, act = "dictar"}
  elseif st == "enfoque" then
    for i, e in ipairs(enfoques) do f[#f + 1] = {t = e.titulo, d = e.linea, h = ROW2, act = "enf", i = i} end
    f[#f + 1] = {t = "Otra cosa (dictar)", h = ROW1, act = "otra"}
  elseif st == "indice" then
    for i, c in ipairs(caps) do
      f[#f + 1] = {t = c.titulo, d = c.linea, m = "~" .. c.palabras .. " palabras", h = ROW2, act = "cap", i = i,
                   casilla = c.activo}
    end
    local activos, minutos = cuenta()
    f[#f + 1] = {t = "Agregar o cambiar por voz", h = ROW1, act = "ajustar"}
    f[#f + 1] = {t = "Más temas", h = ROW1, act = "mas"}
    f[#f + 1] = {t = "Escribir (" .. activos .. (activos == 1 and " capítulo" or " capítulos") .. ", ~" .. minutos ..
                 " min)", h = ROW1, act = "escribir"}
  elseif st == "listo" then
    f[1] = {t = "Abrir en el lector", h = ROW1, act = "abrir"}
    f[2] = {t = "Volver al inicio", h = ROW1, act = "inicio"}
  elseif st == "error" then
    f = err.filas
  end
  return f
end

local function retomar()
  job = {id = pend.id, titulo = pend.titulo, slug = pend.slug, step = 0, total = 0, label = ""}
  titulo = pend.titulo
  pend = nil
  st, sel = "escribiendo", 1
  ultimoPoll = cp.ms() - POLL_MS  -- que el primer tick pregunte ya
end

local function accion(it)
  local a = it and it.act
  if a == "dictar" then
    return escuchar("tema", "¿Sobre qué quieres leer?")
  elseif a == "retomar" then
    retomar()
    return true
  elseif a == "seguir" then
    return pedir("librito.enfoque", {tema = tema}, "enfoque")
  elseif a == "enf" then
    local e = enfoques[it.i]
    enfoque = e and e.titulo or ""
    return pedirIndice()
  elseif a == "otra" then
    return escuchar("enfoque", "¿Cómo quieres encararlo?")
  elseif a == "cap" then
    local c = caps[it.i]
    if not c then return false end
    c.activo = not c.activo
    cp.beep("ok")
    return true
  elseif a == "ajustar" then
    return escuchar("ajuste", "¿Qué cambiamos del índice?")
  elseif a == "mas" then
    return pedirAjuste(PEDIDO_MAS)
  elseif a == "escribir" then
    return pedirEscribir()
  elseif a == "bajar" then
    st, sel = "escribiendo", 1
    return bajar()
  elseif a == "abrir" then
    job = {}
    guardar()
    if not cp.open_book(libro or "") then
      fallo("No está el libro", "No se encontró " .. s(libro) .. " en la tarjeta.", filaVolver("inicio"))
    end
    return true
  elseif a == "inicio" then
    reset()
    guardar()
    return true
  elseif a == "volver" then
    st, sel = err.volver, 1
    return true
  end
  return false
end

local function atras()
  if st == "inicio" then
    cp.quit()
    return false
  elseif st == "tema" then st = "inicio"
  elseif st == "enfoque" then st = "tema"
  elseif st == "indice" then st = "enfoque"
  elseif st == "escribiendo" then
    return false  -- sale; el trabajo sigue en el servidor y se retoma la próxima vez
  elseif st == "listo" then
    reset()
    cargar()
  elseif st == "error" then st = err.volver
  end
  sel = 1
  cp.beep("back")
  return true
end

function on_key(k)
  -- Con algo en curso el host sólo manda "back", y lo cancela él. Nunca salir.
  if cp.busy() then return k == "back" end
  local f = filas()
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
  if st == "escribiendo" and job.id and not cp.busy() and cp.ms() - ultimoPoll >= POLL_MS then
    ultimoPoll = cp.ms()
    pedir("job.status", {id = job.id}, "job")
  end
  return false  -- el host repinta solo cuando llega la respuesta
end

function on_heard(texto)
  local que = oir
  oir = nil
  if type(texto) ~= "string" or texto:match("^%s*$") then return end
  texto = s(texto)
  if que == "tema" then
    tema = texto
    st, sel = "tema", 1
  elseif que == "enfoque" then
    enfoque = texto
    pedirIndice()
  elseif que == "ajuste" then
    pedirAjuste(texto)
  end
end

local function llegoIndice(t)
  local lista = type(t.capitulos) == "table" and t.capitulos or {}
  local nuevos = {}
  for _, c in ipairs(lista) do
    if type(c) == "table" then
      nuevos[#nuevos + 1] = {titulo = s(c.titulo, "Capítulo"), linea = s(c.linea), palabras = n(c.palabras),
                            activo = true}
    end
  end
  if #nuevos == 0 then
    return fallo("No se pudo", "El servidor no propuso capítulos.", filaVolver(st))
  end
  caps = nuevos
  titulo = s(t.titulo)
  if titulo == "" then titulo = tema end
  st, sel = "indice", 1
end

local function llegoJob(t)
  local estado = s(t.state)
  if estado == "done" then
    local f = type(t.files) == "table" and t.files[1] or nil
    if type(f) ~= "table" or s(f.id) == "" or s(f.name) == "" then
      job = {}
      guardar()
      return fallo("No se pudo", "El servidor terminó pero no entregó el archivo.", filaVolver("inicio"))
    end
    job.file = {id = s(f.id), name = s(f.name)}
    if not bajar() then fallo("No se pudo", "No se pudo empezar la descarga.", filasJob("bajar"), "escribiendo") end
  elseif estado == "failed" then
    job = {}
    guardar()
    fallo("No se pudo escribir", s(t.error, "El servidor no pudo escribir el libro."), filaVolver("inicio"))
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
    if que == "job" then return errorServidor(t.error, filasJob("volver"), "escribiendo") end
    if que == "bajar" then return errorServidor(t.error, filasJob("bajar"), "escribiendo") end
    return errorServidor(t.error, filaVolver(st))
  end
  if que == "enfoque" then
    local lista = type(t.enfoques) == "table" and t.enfoques or {}
    enfoques = {}
    for _, e in ipairs(lista) do
      if type(e) == "table" then
        enfoques[#enfoques + 1] = {id = s(e.id), titulo = s(e.titulo, "Enfoque"), linea = s(e.linea)}
      end
    end
    if #enfoques == 0 then return fallo("No se pudo", "El servidor no propuso enfoques.", filaVolver("tema")) end
    local normal = s(t.tema)
    if normal ~= "" then tema = normal end
    st, sel = "enfoque", 1
  elseif que == "indice" then
    llegoIndice(t)
  elseif que == "escribir" then
    local jid = s(t.jobId)
    if jid == "" then return fallo("No se pudo", "El servidor no devolvió el trabajo.", filaVolver("indice")) end
    job = {id = jid, titulo = titulo, slug = s(t.slug), step = 0, total = 0, label = ""}
    guardar()
    st, sel = "escribiendo", 1
    ultimoPoll = cp.ms()
  elseif que == "job" then
    llegoJob(t)
  elseif que == "bajar" then
    libro = job.file and job.file.name or nil
    job = {}
    guardar()
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

function on_draw()
  local fin = cp.height() - 88
  if st == "inicio" then
    local y = cabezal("Librito")
    y = parrafo(y + 8, "Dicta un tema y te escribe un libro corto para leer en 15 minutos. " ..
                "Eliges cómo encararlo y qué capítulos van, y el libro queda en la tarjeta, en Leer.", 12, 4)
    lista(filas(), y + 16, fin)
    pie("OK: dictar el tema · Atrás: salir")
  elseif st == "tema" then
    local y = cabezal("Tema")
    cp.text(SIDE, y + 8, "Entendí:", 10)
    y = parrafo(y + 32, tema, 14, 5, true)
    lista(filas(), y + 24, fin)
    pie("OK: elegir · Atrás: volver")
  elseif st == "enfoque" then
    local y = cabezal("Enfoque")
    cp.text(SIDE, y + 8, fit("Tema: " .. tema, cp.width() - 2 * SIDE, 10), 10)
    lista(filas(), y + 40, fin)
    pie("Palanca: elegir · OK: seguir · Atrás: volver")
  elseif st == "indice" then
    local y = cabezal(titulo ~= "" and titulo or "Índice")
    local activos, minutos, crudo = cuenta()
    cp.text(SIDE, y + 8, activos .. " de " .. #caps .. " capítulos · ~" .. minutos .. " min de lectura", 10)
    lista(filas(), y + 40, fin)
    if activos < #caps and crudo < MIN_MINUTOS then
      pie("Los que quedan se alargan para llegar a 15 minutos")
    else
      pie("OK: quitar o poner un capítulo · Atrás: volver")
    end
  elseif st == "escribiendo" then
    local y = cabezal("Escribiendo…")
    y = parrafo(y + 8, titulo ~= "" and titulo or s(job.titulo), 14, 3, true)
    local paso
    if (job.total or 0) > 0 then
      paso = "Capítulo " .. job.step .. " de " .. job.total .. (job.file and " · bajando el libro" or "")
    elseif job.file then
      paso = "Bajando el libro a la tarjeta"
    else
      paso = "Preparando"
    end
    cp.text(SIDE, y + 16, fit(paso, cp.width() - 2 * SIDE, 12), 12)
    y = parrafo(y + 16 + cp.texth(12), job.label or "", 10, 2)
    local w = cp.width() - 2 * SIDE
    cp.rect(SIDE, y + 16, w, 12, false, 1)
    if (job.total or 0) > 0 then
      cp.rect(SIDE + 2, y + 18, math.max(0, math.min(w - 4, (w - 4) * job.step // job.total)), 8, true)
    end
    pie("Atrás: salir; se sigue escribiendo y se retoma al volver")
  elseif st == "listo" then
    local y = cabezal("Listo")
    y = parrafo(y + 8, titulo ~= "" and titulo or s(libro), 14, 3, true)
    cp.text(SIDE, y + 8, fit("En la tarjeta: " .. s(libro), cp.width() - 2 * SIDE, 10), 10)
    lista(filas(), y + 40, fin)
    pie("OK: elegir · Atrás: inicio")
  elseif st == "error" then
    local y = cabezal(err.titulo)
    y = parrafo(y + 8, err.cuerpo, 12, 8)
    lista(filas(), y + 16, fin)
    pie("OK: elegir · Atrás: volver")
  end
end
