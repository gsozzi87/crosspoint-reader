-- Escenario de examples/Apps/viajes.lua: el flujo entero de punta a punta con
-- el cp falso del harness (docs/ws397/VIAJES_CONTRATO.md). Corre DESPUÉS de
-- cargar la app. Falla con error() si algo no está donde tiene que estar.
--
-- Orden: sin viajes → Actualizar → Inicio → Hoy → Agenda (los nueve días) →
-- Día → Ítem (papel, recordar, preguntar) → Lista (tildar, agregar por voz,
-- sugerir) → Guía entera (preguntas, trabajo, diez secciones, rehacer con
-- salida y retome) → Preguntar por voz → Cambiar de viaje → Actualizar con
-- tildes pendientes → sin reloj → salir.

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

local function avanzar(ms) fake.advance(ms) end

-- Lo que el harness anota en fake.opened: {kind, name, title}.
local function abrio(nombre, titulo)
  for _, o in ipairs(fake.opened) do
    if type(o) == "table" and o.name == nombre and (titulo == nil or o.title == titulo) then return true end
  end
  return false
end

local function archivo(nombre)
  local t = cp.read(nombre)
  if not t then error("no está el archivo " .. nombre) end
  return t
end

-- ------------------------------------------------------------- el viaje de prueba
local function dia(k, date, label, short, items, note)
  return {date = date, n = k, label = label, short = short, note = note or "", items = items or {}}
end

local function viajeLisboa()
  return {
    id = "lis", name = "Lisboa", place = "Lisboa, Portugal", start = "2026-09-14", ["end"] = "2026-09-22",
    when = "14 – 22 de septiembre", hotel = "Hotel Lisboa Plaza", weather = "Nublado · 19°",
    today = "2026-09-16",
    days = {
      dia(1, "2026-09-14", "lunes 14 de septiembre", "lun 14", {
        {id = "i1", at = "10:40", title = "Vuelo IB6251 MAD → LIS", kind = "flight", kindLabel = "Vuelo",
         place = "T4", code = "ABC123", note = "Puerta a confirmar", paperId = "p1", remind = false},
        {id = "i2", at = "15:00", title = "Check-in Hotel Lisboa Plaza", kind = "hotel", kindLabel = "Hotel",
         place = "Av. da Liberdade 12", code = "", note = "", paperId = "", remind = false},
      }),
      dia(2, "2026-09-15", "martes 15 de septiembre", "mar 15"),
      dia(3, "2026-09-16", "miércoles 16 de septiembre", "mié 16", {
        {id = "i3", at = "11:30", title = "Castillo de San Jorge", kind = "visit", kindLabel = "Visita",
         place = "", code = "", note = "entrada 12€", paperId = "", remind = false},
      }, "Día de castillo"),
      dia(4, "2026-09-17", "jueves 17 de septiembre", "jue 17"),
      dia(5, "2026-09-18", "viernes 18 de septiembre", "vie 18"),
      dia(6, "2026-09-19", "sábado 19 de septiembre", "sáb 19"),
      dia(7, "2026-09-20", "domingo 20 de septiembre", "dom 20"),
      dia(8, "2026-09-21", "lunes 21 de septiembre", "lun 21"),
      dia(9, "2026-09-22", "martes 22 de septiembre", "mar 22"),
    },
    packing = {
      {id = "k1", text = "Pasaporte", done = true},
      {id = "k2", text = "Cargador", done = false},
      {id = "k3", text = "Adaptador de enchufe (tipo F)", done = false},
      {id = "k4", text = "Protector solar", done = false},
      {id = "k5", text = "Paraguas", done = false},
      {id = "k6", text = "Auriculares", done = false},
    },
    papers = {{id = "p1", date = "2026-09-14", kind = "flight", title = "Vuelo IB6251", line = "loc. ABC123"}},
    guide = {ready = false, sections = {}},
  }
end

local function viajeCusco()
  local v = {
    id = "cus", name = "Cusco", place = "Cusco, Perú", start = "2026-11-03", ["end"] = "2026-11-10",
    when = "3 – 10 de noviembre", hotel = "", weather = "", today = "", days = {}, packing = {}, papers = {},
    guide = {ready = true, sections = {{n = 1, title = "Para entender el lugar"}}},
  }
  local nombres = {"martes 3", "miércoles 4", "jueves 5", "viernes 6", "sábado 7", "domingo 8", "lunes 9",
                   "martes 10"}
  for k = 1, 8 do
    v.days[k] = dia(k, string.format("2026-11-%02d", k + 2), nombres[k] .. " de noviembre",
                    nombres[k]:sub(1, 3) .. " " .. (k + 2))
  end
  return v
end

local TRIPS = {
  {id = "lis", name = "Lisboa", place = "Lisboa, Portugal", start = "2026-09-14", ["end"] = "2026-09-22",
   when = "14 – 22 de septiembre", active = true, itemCount = 3, paperCount = 1, packDone = 1, packTotal = 6,
   guideReady = false},
  {id = "cus", name = "Cusco", place = "Cusco, Perú", start = "2026-11-03", ["end"] = "2026-11-10",
   when = "3 – 10 de noviembre", active = false, itemCount = 0, paperCount = 0, packDone = 0, packTotal = 0,
   guideReady = true},
}

local SECCIONES = {"Para entender el lugar", "Barrios", "Imperdibles", "Para una mente curiosa", "Comer",
                   "Moverse", "Ojo con", "Un día perfecto", "Frases útiles", "Por si acaso"}

-- ------------------------------------------------------------- 0. sin viajes
on_open()
espera("No hay viajes cargados")
espera("Actualizar")
assert(cp.read("viaje.json") == nil, "no tenía que haber viaje.json")

-- ------------------------------------------------------------- 10. actualizar
local pedidosViaje = {}
fake.reply["viajes.lista"] = function(args) return true, {ok = true, trips = TRIPS} end
fake.reply["viajes.viaje"] = function(args)
  pedidosViaje[#pedidosViaje + 1] = args.id
  if args.id == "cus" then return true, {ok = true, trip = viajeCusco()} end
  return true, {ok = true, trip = viajeLisboa()}
end
fake.key("ok")                    -- Actualizar
espera("Actualizando")
espera("Bajando la lista de viajes")
fake.step()                       -- viajes.lista
espera("Bajando el viaje")
fake.step()                       -- viajes.viaje
assert(pedidosViaje[1] == "lis", "tenía que pedir el viaje activo (lis), pidió " .. tostring(pedidosViaje[1]))
assert(not cp.busy(), "después de actualizar no queda nada pendiente")

-- ------------------------------------------------------------- 1. inicio
-- El reloj del harness dice 2026-09-14: día 1 de 9, y gana sobre el `today`
-- del servidor (2026-09-16).
espera("Lisboa")
espera("Día 1 de 9 · lun 14")
espera("Nublado · 19° en Lisboa, Portugal")
espera("Hoy · lun 14 · 2 cosas")
espera("Agenda · 9 días")
espera("Papeles · 1")
espera("Lista para llevar · 1 de 6")
espera("Guía · no bajada")
espera("Preguntar por voz")
espera("Actualizar · hace un momento")
espera("Cambiar de viaje (2)")
assert(archivo("viaje.json"):find('"Lisboa"', 1, true), "viaje.json no tiene el viaje")

-- Recargar desde la tarjeta: la vista pasa por el JSON de ida y vuelta, con
-- acentos y la flecha del vuelo incluidos.
fake.reload()
on_open()
espera("Lisboa")
espera("Hoy · lun 14 · 2 cosas")
espera("Cambiar de viaje (2)")

-- ------------------------------------------------------------- 2. hoy
fake.key("ok")                    -- Hoy
espera("lunes 14 de septiembre")
espera("Día 1 de 9")
espera("Nublado · 19°")
espera("10:40  Vuelo IB6251 MAD → LIS")
espera("Vuelo · T4")
espera("15:00  Check-in Hotel Lisboa Plaza")
espera("Preguntar sobre hoy")

-- Preguntar sobre hoy lleva la fecha del día.
local pregunta
fake.reply["viajes.preguntar"] = function(args)
  pregunta = args
  return true, {ok = true, answer = "El check-in es a las 15:00 en el Hotel Lisboa Plaza.",
                spoken = "A las tres de la tarde."}
end
fake.key("down")
fake.key("down")
fake.heard = "¿a qué hora es el check-in?"
fake.key("ok")
espera("Esperando al servidor")
fake.step()                       -- la escucha
fake.step()                       -- viajes.preguntar
assert(pregunta.id == "lis" and pregunta.question == "¿a qué hora es el check-in?", "preguntar: args")
assert(pregunta.date == "2026-09-14", "preguntar desde Hoy lleva la fecha, llevó " .. tostring(pregunta.date))
assert(pregunta.itemId == nil, "preguntar desde Hoy no lleva ítem")
assert(abrio("respuesta.txt", "Respuesta"), "no se abrió la respuesta en el visor")
assert(archivo("respuesta.txt"):find("El check-in es a las 15:00", 1, true), "respuesta.txt sin la respuesta")
assert(#fake.said == 0 and cp.busy(), "la voz tenía que quedar encolada")
fake.step()                       -- cp.say
assert(fake.said[1] == "A las tres de la tarde.", "no se dijo la versión corta")
assert(not cp.busy())
espera("Preguntar sobre hoy")     -- sigue en Hoy
assert(fake.key("back") == true)
espera("Agenda · 9 días")         -- Inicio

-- ------------------------------------------------------------- 3. agenda
fake.key("down")
fake.key("ok")                    -- Agenda
espera("Agenda · Lisboa")
espera("Día 1 · lunes 14 de septiembre")
espera("10:40  Vuelo IB6251 MAD → LIS")
espera("Día 2 · martes 15 de septiembre")
espera("— libre —")
espera("Día 3 · miércoles 16 de septiembre")
espera("Día de castillo")
espera("Página 1 de 2")
-- La palanca da la vuelta: desde el día 1, arriba cae en el último día.
fake.key("up")
espera("Página 2 de 2")
espera("Día 9 · martes 22 de septiembre")
espera("Día 8 · lunes 21 de septiembre")

-- ------------------------------------------------------------- 3b. día
fake.key("ok")                    -- Día 9
espera("martes 22 de septiembre")
espera("Día 9 de 9")
espera("— libre —")
espera("Preguntar sobre este día")
noEspera("Nublado")               -- el clima sólo va en Hoy
fake.key("back")                  -- vuelve a la agenda, sobre el día de hoy
espera("Página 1 de 2")
fake.key("ok")                    -- Día 1
espera("lunes 14 de septiembre")
espera("10:40  Vuelo IB6251 MAD → LIS")
espera("Preguntar sobre este día")

-- ------------------------------------------------------------- 4. ítem
fake.key("ok")                    -- el vuelo es la primera fila
espera("Vuelo IB6251 MAD → LIS")
espera("Día 1 · lunes 14 de septiembre · 10:40")
espera("Vuelo · T4")
espera("Código")
espera("ABC123")
espera("Puerta a confirmar")
espera("Ver el papel")
espera("Recordar 2 h antes")
espera("Preguntar sobre esto")

-- Ver el papel: la primera vez lo baja y lo guarda; la segunda no va a la red.
local pedidosPapel = 0
fake.reply["viajes.papel"] = function(args)
  pedidosPapel = pedidosPapel + 1
  assert(args.id == "lis" and args.paperId == "p1", "papel: args")
  return true, {ok = true, title = "Vuelo IB6251", text = "Localizador: ABC123\nAsiento: 12A\n\nGracias por volar."}
end
fake.key("ok")
fake.step()
assert(pedidosPapel == 1, "tenía que bajar el papel")
assert(abrio("papel-p1.txt", "Vuelo IB6251"), "no se abrió el papel")
assert(archivo("papel-p1.txt"):find("Localizador: ABC123\nAsiento: 12A", 1, true), "el papel perdió los renglones")
fake.key("ok")                    -- otra vez: de la tarjeta
assert(pedidosPapel == 1 and not cp.busy(), "el segundo Ver el papel no tenía que ir a la red")
assert(#fake.opened == 3, "el papel se tenía que abrir dos veces")

-- Recordar 2 h antes: OK alterna y va al servidor.
local recordar = {}
fake.reply["viajes.recordar"] = function(args)
  recordar[#recordar + 1] = args
  return true, {ok = true, reminderId = args.on and "r1" or nil}
end
fake.key("down")
fake.key("ok")
fake.step()
assert(recordar[1].id == "lis" and recordar[1].itemId == "i1" and recordar[1].on == true, "recordar: encender")
assert(archivo("viaje.json"):find('"remind":true', 1, true), "el recordatorio no quedó en viaje.json")
fake.key("ok")
fake.step()
assert(recordar[2].on == false, "recordar: apagar")
assert(not archivo("viaje.json"):find('"remind":true', 1, true), "el recordatorio tenía que apagarse")

-- Preguntar sobre esto: lleva el día y el ítem.
fake.key("down")
fake.heard = "¿en qué terminal sale?"
fake.key("ok")
fake.step()
fake.step()
assert(pregunta.date == "2026-09-14" and pregunta.itemId == "i1", "preguntar desde el ítem lleva día e ítem")
fake.step()                       -- la voz
assert(#fake.said == 2)

-- Atrás vuelve a donde se abrió el ítem (el Día), y de ahí a la agenda y al inicio.
fake.key("back")
espera("Preguntar sobre este día")
fake.key("back")
espera("Agenda · Lisboa")
fake.key("back")
espera("Agenda · 9 días")

-- ------------------------------------------------------------- 6. lista para llevar
fake.key("down")
fake.key("down")
fake.key("down")
fake.key("ok")                    -- Lista para llevar
espera("Para llevar · 1 de 6")
espera("Pasaporte")
espera("Paraguas")
espera("Agregar por voz")
espera("Sugerir con IA")

-- Tildar es local y en el acto: queda en viaje.json y en pendientes.json.
fake.key("ok")                    -- Pasaporte: hecho → pendiente
espera("Para llevar · 0 de 6")
assert(not cp.busy(), "tildar no va a la red")
assert(archivo("pendientes.json"):find('"k1"', 1, true), "el tilde no quedó en pendientes.json")
fake.key("down")
fake.key("ok")                    -- Cargador: pendiente → hecho
espera("Para llevar · 1 de 6")
assert(archivo("pendientes.json"):find('"k2"', 1, true), "el segundo tilde no quedó pendiente")
fake.key("ok")                    -- Cargador otra vez: dos toques = nada que subir
espera("Para llevar · 0 de 6")
assert(not archivo("pendientes.json"):find('"k2"', 1, true), "dos toques sobre el mismo ítem se anulan")
assert(archivo("pendientes.json"):find('"k1"', 1, true))

-- Agregar por voz: el servidor recibe lo dicho tal cual y devuelve la lista
-- entera; el tilde de Pasaporte que todavía no subió se aplica encima.
local llevar = {}
fake.reply["viajes.llevar"] = function(args)
  llevar[#llevar + 1] = args
  assert(args.id == "lis", "llevar: id")
  if args.action == "add" then
    assert(args.text == "adaptador de corriente y quita el paraguas", "llevar add: text " .. tostring(args.text))
    return true, {ok = true, packing = {
      {id = "k1", text = "Pasaporte", done = true},
      {id = "k2", text = "Cargador", done = false},
      {id = "k3", text = "Adaptador de enchufe (tipo F)", done = false},
      {id = "k4", text = "Protector solar", done = false},
      {id = "k6", text = "Auriculares", done = false},
      {id = "k7", text = "Adaptador de corriente", done = false},
    }}
  end
  return true, {ok = true, packing = {}}
end
for _ = 1, 5 do fake.key("down") end
espera("Agregar por voz")
fake.heard = "adaptador de corriente y quita el paraguas"
fake.key("ok")
fake.step()                       -- la escucha
fake.step()                       -- viajes.llevar add
assert(#llevar == 1 and llevar[1].action == "add", "agregar por voz tenía que llamar a viajes.llevar")
espera("Adaptador de corriente")
noEspera("Paraguas")
espera("Para llevar · 0 de 6")    -- Pasaporte sigue destildado: el pendiente manda

-- Sugerir con IA: nada se agrega solo; OK marca y la última fila manda las marcadas.
fake.reply["viajes.sugerir"] = function(args)
  assert(args.id == "lis")
  return true, {ok = true, suggestions = {"Bloqueador solar", "Zapatos cómodos", "Botella de agua"}}
end
fake.key("up")                    -- desde la primera fila, arriba es la última: Sugerir
espera("Sugerir con IA")
fake.key("ok")
fake.step()
espera("Sugerencias")
espera("Bloqueador solar")
espera("Botella de agua")
espera("Agregar las marcadas")
fake.key("ok")                    -- marca Bloqueador
fake.key("down")
fake.key("ok")                    -- marca Zapatos
fake.key("down")
fake.key("down")                  -- Agregar las marcadas
local agregadas
fake.reply["viajes.sugerir.agregar"] = function(args)
  agregadas = args.texts
  local p = {
    {id = "k1", text = "Pasaporte", done = true}, {id = "k2", text = "Cargador", done = false},
    {id = "k3", text = "Adaptador de enchufe (tipo F)", done = false}, {id = "k4", text = "Protector solar", done = false},
    {id = "k6", text = "Auriculares", done = false}, {id = "k7", text = "Adaptador de corriente", done = false},
    {id = "k8", text = "Bloqueador solar", done = false}, {id = "k9", text = "Zapatos cómodos", done = false},
  }
  return true, {ok = true, packing = p}
end
fake.key("ok")
fake.step()
assert(type(agregadas) == "table" and #agregadas == 2, "tenían que ir dos sugerencias")
assert(agregadas[1] == "Bloqueador solar" and agregadas[2] == "Zapatos cómodos", "las marcadas, en orden")
espera("Para llevar · 0 de 8")
espera("Zapatos cómodos")
noEspera("Botella de agua")
fake.key("back")
espera("Lista para llevar · 0 de 8")

-- ------------------------------------------------------------- 7. guía
for _ = 1, 4 do fake.key("down") end
espera("Guía · no bajada")
fake.reply["viajes.guia.preguntas"] = function(args)
  assert(args.id == "lis")
  return true, {ok = true, questions = {
    {key = "hotel", text = "¿En qué hotel te quedas?"},
    {key = "llegada", text = "¿A qué hora llegas?"},
    {key = "intereses", text = "¿Qué te interesa más?"},
  }}
end
fake.key("ok")                    -- Guía: sin guía en la tarjeta arranca sola
espera("Esperando al servidor")
fake.step()                       -- viajes.guia.preguntas → primera escucha
espera("Pregunta 1 de 3")
espera("¿En qué hotel te quedas?")
fake.heard = "Hotel Lisboa Plaza"
fake.step()
espera("Pregunta 2 de 3")
-- Atrás salta la pregunta: el host corta la escucha y avisa con nil.
assert(fake.key("back") == true, "con la escucha abierta Atrás no sale")
fake.heard = nil
fake.step()
espera("Pregunta 3 de 3")
espera("¿Qué te interesa más?")
local generar
fake.reply["viajes.guia.generar"] = function(args)
  generar = args
  return true, {ok = true, jobId = "g-9"}
end
fake.heard = "historia y comida"
fake.step()                       -- última respuesta → generar
fake.step()                       -- viajes.guia.generar
assert(generar.id == "lis", "generar: id")
assert(generar.answers.hotel == "Hotel Lisboa Plaza", "generar: hotel")
assert(generar.answers.llegada == nil, "la pregunta saltada no viaja")
assert(generar.answers.intereses == "historia y comida", "generar: intereses")
espera("Armando la guía")
espera("Preparando")
assert(cp.load():find("job=g-9", 1, true), "el trabajo de la guía no se guardó")

local consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "g-9", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  if consultas < 3 then
    return true, {ok = true, state = "running", step = consultas + 1, total = 10,
                  label = "Sección " .. (consultas + 1) .. " de 10 · " .. SECCIONES[consultas + 1]}
  end
  return true, {ok = true, state = "done", step = 10, total = 10}
end
fake.tick()
assert(consultas == 0, "no debe consultar antes de 5 s")
avanzar(5000)
fake.tick()
fake.step()
assert(consultas == 1, "primera consulta")
espera("Sección 2 de 10")
espera("Barrios")
avanzar(5000)
fake.tick()
fake.step()
assert(consultas == 2)
espera("Sección 3 de 10")
avanzar(5000)
fake.tick()
fake.step()                       -- done → baja la primera sección
assert(consultas == 3)
espera("Bajando la guía")
espera("Sección 1 de 10")

local secciones = {}
fake.reply["viajes.guia.seccion"] = function(args)
  assert(args.id == "lis", "seccion: id")
  secciones[#secciones + 1] = args.n
  return true, {ok = true, n = args.n, title = SECCIONES[args.n],
                text = "Texto de la sección " .. args.n .. ".\n\nSegundo párrafo."}
end
for k = 1, 10 do
  fake.step()
end
assert(#secciones == 10 and secciones[1] == 1 and secciones[10] == 10, "tenían que bajar las diez, en orden")
assert(not cp.busy())
espera("Guía · Lisboa")
espera("1. Para entender el lugar")
espera("9. Frases útiles")
espera("10. Por si acaso")
espera("Rehacer la guía")
for k = 1, 10 do
  local t = archivo("guia-" .. k .. ".txt")
  assert(t:find("^" .. SECCIONES[k]:gsub("%p", "%%%0") .. "\n\nTexto de la sección " .. k), "guia-" .. k .. ".txt")
end
assert(not cp.load():find("job=g", 1, true), "el trabajo tenía que borrarse al terminar")

-- Abrir una sección en el visor.
fake.key("down")
fake.key("ok")                    -- 2. Barrios
assert(abrio("guia-2.txt", "Barrios"), "no se abrió la sección en el visor")

-- Rehacer sin confirmación; esta vez el servidor no pregunta nada, se sale a
-- mitad del trabajo y se retoma al volver a entrar.
fake.reply["viajes.guia.preguntas"] = function(args) return true, {ok = true, questions = {}} end
fake.reply["viajes.guia.generar"] = function(args)
  assert(next(args.answers) == nil, "sin preguntas, sin respuestas")
  return true, {ok = true, jobId = "g-10"}
end
fake.key("up")                    -- desde Barrios: Para entender el lugar…
fake.key("up")                    -- …y desde la primera fila, arriba es Rehacer
espera("Rehacer la guía")
local abiertas = #fake.opened
fake.key("ok")
assert(#fake.opened == abiertas, "Rehacer no abre nada")
fake.step()                       -- preguntas (ninguna) → generar
fake.step()                       -- generar
espera("Armando la guía")
fake.key("back")                  -- salir; el trabajo sigue
espera("Guía · generando…")
fake.reload()
on_open()
espera("Guía · generando…")
consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "g-10", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  return true, {ok = true, state = "done", step = 10, total = 10}
end
fake.reply["viajes.guia.seccion"] = function(args)
  return true, {ok = true, n = args.n, title = "Nueva " .. args.n, text = "Texto nuevo " .. args.n}
end
for _ = 1, 4 do fake.key("down") end
fake.key("ok")                    -- Guía: retoma el trabajo
espera("Armando la guía")
fake.tick()                       -- al retomar pregunta en el acto
fake.step()
assert(consultas == 1, "al retomar tenía que consultar en el acto")
for _ = 1, 10 do fake.step() end
espera("Guía · Lisboa")
espera("1. Nueva 1")
espera("10. Nueva 10")
assert(archivo("guia-1.txt"):find("^Nueva 1\n\nTexto nuevo 1"), "rehacer tenía que pisar la guía vieja")
fake.key("back")
espera("Guía · 10 secciones")

-- ------------------------------------------------------------- 9. preguntar por voz
for _ = 1, 5 do fake.key("down") end
espera("Preguntar por voz")
fake.heard = "¿cómo se dice la cuenta por favor?"
fake.key("ok")
fake.step()
fake.step()
assert(pregunta.question == "¿cómo se dice la cuenta por favor?" and pregunta.date == nil and
       pregunta.itemId == nil, "preguntar desde el inicio va sin contexto")
assert(abrio("respuesta.txt"))
fake.step()
assert(#fake.said == 3)
-- Sin entender nada no se pregunta nada.
fake.heard = nil
fake.key("ok")
fake.step()
assert(not cp.busy() and #fake.said == 3, "sin texto no hay pregunta")

-- ------------------------------------------------------------- 11. cambiar de viaje
fake.key("down")                  -- Actualizar
fake.key("down")                  -- Cambiar de viaje, la última fila
espera("Cambiar de viaje (2)")
fake.key("ok")
fake.step()                       -- viajes.lista
espera("Cambiar de viaje")
espera("Lisboa")
espera("Cusco")
espera("activo")
espera("3 – 10 de noviembre · Cusco, Perú")
local activado
fake.reply["viajes.activar"] = function(args)
  activado = args.id
  return true, {ok = true, id = args.id}
end
fake.key("down")
fake.key("ok")                    -- Cusco
fake.step()                       -- viajes.activar
fake.step()                       -- viajes.viaje
assert(activado == "cus", "tenía que activar Cusco")
assert(pedidosViaje[#pedidosViaje] == "cus", "y bajar Cusco")
espera("Cusco")
espera("3 – 10 de noviembre · faltan 50 días")
espera("Día 1 · mar 3 · libre")   -- antes del viaje: el primer día
espera("Agenda · 8 días")
espera("Papeles · 0")
espera("Lista para llevar · 0 de 0")
espera("Guía · no bajada")
noEspera("Nublado")
-- Los papeles y la guía eran de Lisboa: se fueron. Los tildes pendientes quedan.
assert(cp.size("guia-1.txt") == nil and cp.size("papel-p1.txt") == nil, "los archivos del otro viaje se borran")
assert(archivo("pendientes.json"):find('"k1"', 1, true), "el tilde pendiente de Lisboa sigue ahí")
assert(archivo("viaje.json"):find('"Cusco"', 1, true))

-- La guía de Cusco ya existe en el servidor: entrar la baja sin preguntas.
for _ = 1, 4 do fake.key("down") end
fake.key("ok")
espera("Bajando la guía")
for _ = 1, 10 do fake.step() end
espera("Guía · Cusco")
espera("1. Nueva 1")
fake.key("back")
espera("Guía · 10 secciones")

-- ------------------------------------------------------------- 10. actualizar con pendientes
llevar = {}
fake.key("down")
for _ = 1, 5 do fake.key("down") end
espera("Actualizar · hace un momento")
fake.key("ok")
espera("Subiendo los tildes pendientes · 0 de 1")
fake.step()                       -- viajes.llevar toggle
assert(#llevar == 1 and llevar[1].action == "toggle" and llevar[1].id == "lis" and llevar[1].itemId == "k1",
       "el tilde pendiente tenía que subir como toggle del viaje de Lisboa")
assert(cp.read("pendientes.json") == nil, "pendientes.json se borra al subir")
espera("Bajando la lista de viajes")
fake.step()                       -- viajes.lista (Lisboa sigue activo en la lista falsa)
fake.step()                       -- viajes.viaje
espera("Lisboa")
espera("Cambiar de viaje (2)")

-- ------------------------------------------------------------- sin reloj
-- Sin hora del aparato manda el `today` del servidor; Lisboa dice 16 → día 3.
local reloj = cp.time
cp.time = function() return nil end
espera("Día 3 de 9 · mié 16")
espera("Hoy · mié 16 · 1 cosa")
-- Y sin ninguno de los dos, la fila Hoy lo dice.
fake.key("up")
fake.key("up")                    -- Actualizar
fake.reply["viajes.lista"] = function(args)
  return true, {ok = true, trips = {TRIPS[2]}}   -- ahora sólo Cusco, sin `today`
end
fake.key("ok")
fake.step()
fake.step()
espera("Cusco")
espera("El aparato no está en hora")
espera("3 – 10 de noviembre")
noEspera("faltan")
noEspera("Cambiar de viaje")     -- un solo viaje: la fila no está
cp.time = reloj

-- ------------------------------------------------------------- errores
-- Un servidor que no contesta deja la pantalla de error con Reintentar y Volver.
fake.reply["viajes.lista"] = function(args) return false, {error = "sin conexión con el servidor (0)"} end
fake.key("up")                    -- Actualizar (última fila sin Cambiar)
espera("Actualizar")
fake.key("ok")
fake.step()
espera("No se pudo")
espera("sin conexión con el servidor")
espera("Reintentar")
fake.key("down")
fake.key("ok")                    -- Volver
espera("Agenda · 8 días")

-- ------------------------------------------------------------- salir
assert(fake.key("back") == false, "Atrás en el inicio sale de la app")
