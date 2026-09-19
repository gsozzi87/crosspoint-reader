-- Escenario de examples/Apps/librito.lua: el flujo entero de punta a punta con
-- el cp falso del harness (docs/ws397/PLAN_APPS_VIAJES_EPUB.md, "Prueba de
-- escritorio"). Corre DESPUÉS de cargar la app. Falla con error() si algo no
-- está donde tiene que estar.
--
-- Orden: error de clave → enfoque → índice (quitar el 2, "Más temas" con 8) →
-- escribir → salir con el trabajo guardado → recargar → Retomar → running dos
-- veces, done a la tercera → descarga → Abrir en el lector.

local function contiene(lista, sub)
  for _, s in ipairs(lista) do
    if type(s) == "string" and s:find(sub, 1, true) then return true end
  end
  return false
end

local function espera(sub)
  local d = fake.draw()
  if not contiene(d, sub) then
    error("no se dibujó «" .. sub .. "»; pantalla: " .. table.concat(d, " | "))
  end
end

local function noEspera(sub)
  local d = fake.draw()
  if contiene(d, sub) then error("se dibujó «" .. sub .. "» y no debía") end
end

-- El tiempo del on_tick: cp.ms() tiene que avanzar 5 s entre consultas.
local function avanzar(ms)
  if type(fake.advance) == "function" then
    fake.advance(ms)
  elseif type(fake.ms) == "number" then
    fake.ms = fake.ms + ms
  else
    error("el harness no ofrece fake.advance(ms) ni fake.ms para mover cp.ms()")
  end
end

-- Lo que el harness anota en fake.opened puede ser el nombre o una tabla.
local function abrio(nombre)
  for _, o in ipairs(fake.opened or {}) do
    if type(o) == "string" and o:find(nombre, 1, true) then return true end
    if type(o) == "table" then
      for _, v in pairs(o) do
        if type(v) == "string" and v:find(nombre, 1, true) then return true end
      end
    end
  end
  return false
end

local function capitulos(cuantos)
  local c = {}
  for i = 1, cuantos do
    c[i] = {n = i, titulo = "Capítulo " .. i .. " de la peste", linea = "De qué trata el " .. i, palabras = 500}
  end
  return c
end

-- ------------------------------------------------------------- inicio
on_open()
espera("Escritor")
espera("Dictar el tema")
noEspera("Retomar")

-- OK dicta el tema.
fake.heard = "la peste negra"
fake.key("ok")
fake.step()
espera("la peste negra")
espera("Seguir")

-- ------------------------------------------------------------- error de clave
fake.reply["librito.enfoque"] = function(args)
  assert(args.tema == "la peste negra", "enfoque: tema " .. tostring(args.tema))
  return false, {error = "Carga la clave de las apps en la web (Ajustes → Apps de Lua)"}
end
fake.key("ok")          -- Seguir
espera("Esperando al servidor")
fake.step()
espera("Falta la clave")
espera("Volver")
fake.key("ok")          -- Volver → tema
espera("Seguir")

-- ------------------------------------------------------------- enfoque
fake.reply["librito.enfoque"] = function(args)
  return true, {ok = true, tema = "La peste negra", enfoques = {
    {id = "crono", titulo = "Cronología y causas", linea = "De Asia a Europa en cinco años"},
    {id = "vida", titulo = "Cómo se vivía", linea = "La ciudad, el campo, los médicos"},
    {id = "despues", titulo = "Qué cambió después", linea = "Salarios, fe y arte"},
  }}
end
fake.key("ok")          -- Seguir
fake.step()
espera("Cronología y causas")
espera("Cómo se vivía")
espera("Qué cambió después")
espera("Otra cosa (dictar)")

-- Elegir el segundo.
fake.reply["librito.indice"] = function(args)
  assert(args.tema == "La peste negra", "indice: tema " .. tostring(args.tema))
  assert(args.enfoque == "Cómo se vivía", "indice: enfoque " .. tostring(args.enfoque))
  assert(args.minutos == 15, "indice: minutos")
  return true, {ok = true, titulo = "La peste negra: cómo se vivía", minutos = 15, capitulos = capitulos(6)}
end
fake.key("down")
fake.key("ok")
fake.step()

-- ------------------------------------------------------------- índice
espera("La peste negra: cómo se vivía")
espera("Capítulo 1 de la peste")
espera("~500 palabras")
espera("6 de 6 capítulos")
espera("Escribir (6 capítulos, ~15 min)")

-- Desactivar el capítulo 2 con OK: quedan 5 (2500 palabras, o sea 12,5 min)
-- y la pantalla avisa que el resto se alarga para llegar a 15.
fake.key("down")
fake.key("ok")
espera("5 de 6 capítulos")
espera("Escribir (5 capítulos, ~15 min)")
espera("se alargan para llegar a 15 minutos")

-- "Más temas": el servidor recibe los seis con su `activo` y devuelve ocho.
local pedidoMas
fake.reply["librito.ajustar"] = function(args)
  assert(type(args.capitulos) == "table" and #args.capitulos == 6, "ajustar: van los 6 capítulos")
  assert(args.capitulos[1].activo == true, "ajustar: el 1 activo")
  assert(args.capitulos[2].activo == false, "ajustar: el 2 inactivo")
  assert(type(args.pedido) == "string" and args.pedido ~= "", "ajustar: pedido")
  pedidoMas = args.pedido
  return true, {ok = true, titulo = "La peste negra: cómo se vivía", minutos = 20, capitulos = capitulos(8)}
end
-- La lista es: 6 capítulos, Agregar, Más temas, Escribir. Desde el 2 hay que
-- bajar hasta la fila 8.
for _ = 1, 6 do fake.key("down") end
espera("Más temas")
fake.key("ok")
fake.step()
assert(pedidoMas:find("más capítulos", 1, true), "el pedido de Más temas: " .. pedidoMas)
espera("8 de 8 capítulos")
espera("Capítulo 8 de la peste")
espera("Página 1 de 2")

-- Con ocho capítulos la lista no entra en una pantalla: las tres acciones
-- quedan en la página siguiente y la palanca da la vuelta desde la primera.
fake.key("up")
espera("Página 2 de 2")
espera("Escribir (8 capítulos, ~20 min)")

-- ------------------------------------------------------------- escribir
fake.reply["librito.escribir"] = function(args)
  assert(args.titulo == "La peste negra: cómo se vivía", "escribir: titulo")
  assert(#args.capitulos == 8, "escribir: 8 capítulos activos")
  assert(args.capitulos[1].palabras == 500, "escribir: palabras")
  assert(args.minutos >= 15, "escribir: minutos")
  return true, {ok = true, jobId = "job-77", slug = "la-peste-negra"}
end
fake.key("ok")          -- Escribir
fake.step()
espera("Escribiendo")
espera("Preparando")

-- Salir con el trabajo en curso: el estado quedó guardado y se retoma.
assert(type(cp.load()) == "string" and cp.load():find("job-77", 1, true), "no se guardó el jobId")
if type(fake.reload) == "function" then fake.reload() end
on_open()
espera("Retomar: La peste negra: cómo se vivía")
fake.key("ok")          -- Retomar (es la primera fila)
espera("Escribiendo")

-- ------------------------------------------------------------- job.status
local consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "job-77", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  if consultas < 3 then
    return true, {ok = true, state = "running", step = consultas, total = 6,
                  label = "Capítulo " .. consultas .. " de la peste"}
  end
  return true, {ok = true, state = "done", step = 6, total = 6,
                files = {{id = "f-1", name = "la-peste-negra.epub", bytes = 12000}}}
end

fake.tick()             -- al retomar pregunta en el acto
fake.step()
assert(consultas == 1, "primera consulta")
espera("Capítulo 1 de 6")

fake.tick()             -- antes de los 5 s no vuelve a preguntar
assert(consultas == 1, "no debe consultar antes de 5 s")
avanzar(5000)
fake.tick()
fake.step()
assert(consultas == 2, "segunda consulta")
espera("Capítulo 2 de 6")

avanzar(5000)
fake.tick()
fake.step()             -- done → arranca la descarga
assert(consultas == 3, "tercera consulta")
espera("bajando")
fake.step()             -- la descarga termina
espera("Listo")
espera("la-peste-negra.epub")
espera("Abrir en el lector")

-- ------------------------------------------------------------- abrir
fake.key("ok")          -- Abrir en el lector
assert(abrio("la-peste-negra.epub"), "no se abrió el EPUB en el lector")
assert(cp.load() == nil or cp.load() == "", "el estado guardado tenía que borrarse al abrir")

-- Un job que falla se muestra con su texto.
on_open()
fake.heard = "los volcanes"
fake.key("ok")
fake.step()
fake.key("ok")          -- Seguir
fake.step()
fake.reply["librito.indice"] = function(args)
  assert(args.enfoque == "Cronología y causas", "indice: enfoque " .. tostring(args.enfoque))
  return true, {ok = true, titulo = "Los volcanes", minutos = 15, capitulos = capitulos(6)}
end
fake.key("ok")          -- primer enfoque
fake.step()
for _ = 1, 8 do fake.key("down") end  -- los 6 capítulos → Agregar → Más temas → Escribir
espera("Escribir (6 capítulos")
fake.reply["librito.escribir"] = function(args)
  assert(args.titulo == "Los volcanes", "escribir: titulo " .. tostring(args.titulo))
  return true, {ok = true, jobId = 78}   -- un número también vale
end
fake.key("ok")
fake.step()
fake.reply["job.status"] = function(args)
  assert(args.id == "78", "job.status: id " .. tostring(args.id))
  return true, {ok = true, state = "failed", error = "Se acabó el tope mensual"}
end
avanzar(5000)
fake.tick()
fake.step()
espera("No se pudo escribir")
espera("Se acabó el tope mensual")
fake.key("ok")          -- Volver
espera("Dictar el tema")
noEspera("Retomar")
