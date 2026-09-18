-- Escenario de examples/Apps/viajes.lua (contrato v2: lugar y hotel por día,
-- guía POR DÍA y sólo a pedido): el flujo entero de punta a punta con el cp
-- falso del harness (docs/ws397/VIAJES_CONTRATO.md). Corre DESPUÉS de cargar
-- la app. Falla con error() si algo no está donde tiene que estar.
--
-- Orden: sin viajes → Actualizar → Inicio → Hoy (lugar y hotel, preguntar con
-- fecha) → Agenda (trece días, lugar en el detalle) → Día → Ítem (papel,
-- recordar, preguntar) → Lista (tildar, agregar por voz, sugerir) → Guía del
-- día 1 desde Día (dos preguntas: una contestada, una saltada; trabajo;
-- bajar; visor; Rehacer) → Guía (7) como lista de días → bajar una guía que
-- ya estaba en el servidor → Rehacer con salida a mitad del trabajo, Retomar
-- desde el inicio tras recargar → Preguntar por voz (con cp.say) → Cambiar de
-- viaje → Actualizar con tildes pendientes → sin reloj → errores → un
-- viaje.json INCOMPLETO por todas las pantallas → salir.

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
local function ultimoAbierto()
  local o = fake.opened[#fake.opened]
  return o and o.name or nil
end

local function archivo(nombre)
  local t = cp.read(nombre)
  if not t then error("no está el archivo " .. nombre) end
  return t
end

-- ------------------------------------------------------------- el viaje de prueba
-- Trece días: Roma (3) → crucero (4, con Santorini y Mykonos) → Roma (1) →
-- tren (1) → Madrid (4). Hotel distinto casi cada noche; noches sin hotel
-- (desembarco, tren, vuelta); días sin lugar (navegación, tren).
local function dia(k, date, label, short, place, hotel, items, note, guide)
  return {date = date, n = k, label = label, short = short, place = place, hotel = hotel, note = note or "",
          guide = guide or {ready = false, at = 0}, items = items or {}}
end
local MSC = "MSC Fantasia · cabina 8123"

local function viajeItalia()
  return {
    id = "ita", name = "Italia y España", place = "Roma · crucero · Madrid", start = "2026-09-14",
    ["end"] = "2026-09-26", when = "14 – 26 de septiembre", today = "2026-09-16",
    days = {
      dia(1, "2026-09-14", "sábado 14 de septiembre", "sáb 14", "Roma", "Hotel Artemide", {
        {id = "i1", at = "10:40", title = "Vuelo IB3230 MAD → FCO", kind = "flight", kindLabel = "Vuelo",
         place = "T4", code = "ABC123", note = "Puerta a confirmar", paperId = "p1", remind = false},
        {id = "i2", at = "15:00", title = "Check-in Hotel Artemide", kind = "hotel", kindLabel = "Hotel",
         place = "Via Nazionale 22", code = "", note = "", paperId = "", remind = false},
      }),
      dia(2, "2026-09-15", "domingo 15 de septiembre", "dom 15", "Roma", "Hotel Artemide", {
        {id = "i3", at = "09:30", title = "Coliseo y Foro", kind = "visit", kindLabel = "Visita",
         place = "", code = "", note = "entrada 18€", paperId = "", remind = false},
      }),
      -- Este día ya tiene guía en el servidor (hecha desde la web).
      dia(3, "2026-09-16", "lunes 16 de septiembre", "lun 16", "Roma", "Hotel Artemide", nil,
          "Vaticano por la mañana", {ready = true, at = 1789000000}),
      dia(4, "2026-09-17", "martes 17 de septiembre", "mar 17", "Civitavecchia", MSC, {
        {id = "i4", at = "13:00", title = "Embarque MSC Fantasia", kind = "other", kindLabel = "Otro",
         place = "Terminal de cruceros", code = "MSC77", note = "", paperId = "p2", remind = false},
      }),
      dia(5, "2026-09-18", "miércoles 18 de septiembre", "mié 18", "", MSC),   -- navegación: sin lugar
      dia(6, "2026-09-19", "jueves 19 de septiembre", "jue 19", "Santorini", MSC, {
        {id = "i5", at = "10:00", title = "Excursión a Oia", kind = "visit", kindLabel = "Visita",
         place = "", code = "", note = "", paperId = "", remind = false},
      }),
      dia(7, "2026-09-20", "viernes 20 de septiembre", "vie 20", "Mykonos", MSC),
      dia(8, "2026-09-21", "sábado 21 de septiembre", "sáb 21", "Roma", "", {   -- sin hotel
        {id = "i6", at = "08:00", title = "Desembarco", kind = "other", kindLabel = "Otro",
         place = "", code = "", note = "", paperId = "", remind = false},
      }),
      dia(9, "2026-09-22", "domingo 22 de septiembre", "dom 22", "", "", {   -- tren: sin lugar ni hotel
        {id = "i7", at = "09:15", title = "Tren Roma → Madrid", kind = "train", kindLabel = "Tren",
         place = "Termini", code = "TR55", note = "", paperId = "", remind = false},
      }),
      dia(10, "2026-09-23", "lunes 23 de septiembre", "lun 23", "Madrid", "Hotel Praktik"),
      dia(11, "2026-09-24", "martes 24 de septiembre", "mar 24", "Madrid", "Hotel Praktik"),
      dia(12, "2026-09-25", "miércoles 25 de septiembre", "mié 25", "Madrid", "Hotel Praktik", {
        {id = "i8", at = "10:00", title = "Museo del Prado", kind = "visit", kindLabel = "Visita",
         place = "", code = "", note = "", paperId = "", remind = false},
      }),
      dia(13, "2026-09-26", "jueves 26 de septiembre", "jue 26", "Madrid", "", {
        {id = "i9", at = "18:30", title = "Vuelo de vuelta", kind = "flight", kindLabel = "Vuelo",
         place = "T4", code = "", note = "", paperId = "", remind = false},
      }),
    },
    packing = {
      {id = "k1", text = "Pasaporte", done = true},
      {id = "k2", text = "Cargador", done = false},
      {id = "k3", text = "Adaptador de enchufe (tipo F)", done = false},
      {id = "k4", text = "Protector solar", done = false},
      {id = "k5", text = "Paraguas", done = false},
      {id = "k6", text = "Auriculares", done = false},
    },
    papers = {
      {id = "p1", date = "2026-09-14", kind = "flight", title = "Vuelo IB3230", line = "loc. ABC123"},
      {id = "p2", date = "2026-09-17", kind = "ticket", title = "Crucero MSC", line = "reserva MSC77"},
    },
  }
end

local function viajeCusco()
  local v = {
    id = "cus", name = "Cusco", place = "Cusco, Perú", start = "2026-11-03", ["end"] = "2026-11-10",
    when = "3 – 10 de noviembre", today = "", days = {}, packing = {}, papers = {},
  }
  local nombres = {"martes 3", "miércoles 4", "jueves 5", "viernes 6", "sábado 7", "domingo 8", "lunes 9",
                   "martes 10"}
  for k = 1, 8 do
    v.days[k] = dia(k, string.format("2026-11-%02d", k + 2), nombres[k] .. " de noviembre",
                    nombres[k]:sub(1, 3) .. " " .. (k + 2), "Cusco", k < 8 and "Hostal Qorikancha" or "")
  end
  return v
end

local TRIPS = {
  {id = "ita", name = "Italia y España", place = "Roma · crucero · Madrid", start = "2026-09-14",
   ["end"] = "2026-09-26", when = "14 – 26 de septiembre", active = true, itemCount = 9, paperCount = 2,
   packDone = 1, packTotal = 6, guideReady = false},
  {id = "cus", name = "Cusco", place = "Cusco, Perú", start = "2026-11-03", ["end"] = "2026-11-10",
   when = "3 – 10 de noviembre", active = false, itemCount = 0, paperCount = 0, packDone = 0, packTotal = 0,
   guideReady = false},
}

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
  return true, {ok = true, trip = viajeItalia()}
end
fake.key("ok")                    -- Actualizar
espera("Actualizando")
espera("Bajando la lista de viajes")
fake.step()                       -- viajes.lista
espera("Bajando el viaje")
fake.step()                       -- viajes.viaje
assert(pedidosViaje[1] == "ita", "tenía que pedir el viaje activo (ita), pidió " .. tostring(pedidosViaje[1]))
assert(not cp.busy(), "después de actualizar no queda nada pendiente")

-- ------------------------------------------------------------- 1. inicio
-- El reloj del harness dice 2026-09-14: día 1 de 13, y gana sobre el `today`
-- del servidor (2026-09-16).
espera("Italia y España")
espera("Día 1 de 13 · sáb 14")
espera("Roma · crucero · Madrid")
espera("Hoy · sáb 14 · Roma · 2 cosas")
espera("Agenda · 13 días")
espera("Papeles · 2")
espera("Lista para llevar · 1 de 6")
espera("Guía · 0 de 13 días")
espera("Preguntar por voz")
espera("Actualizar · hace un momento")
espera("Cambiar de viaje (2)")
noEspera("Retomar")
noEspera("Nublado")               -- no hay clima del viaje en v2
assert(archivo("viaje.json"):find('"Italia y España"', 1, true), "viaje.json no tiene el viaje")
assert(archivo("viaje.json"):find('"hotel":"Hotel Artemide"', 1, true), "viaje.json no guarda el hotel del día")

-- Recargar desde la tarjeta: la vista pasa por el JSON de ida y vuelta, con
-- acentos y la flecha del vuelo incluidos.
fake.reload()
on_open()
espera("Italia y España")
espera("Hoy · sáb 14 · Roma · 2 cosas")
espera("Cambiar de viaje (2)")

-- ------------------------------------------------------------- 2. hoy
fake.key("ok")                    -- Hoy
espera("sábado 14 de septiembre")
espera("Día 1 de 13")
espera("Roma · Hotel Artemide")   -- el lugar y el hotel del día
espera("10:40  Vuelo IB3230 MAD → FCO")
espera("Vuelo · T4")
espera("15:00  Check-in Hotel Artemide")
espera("Guía de este día")
espera("Se arma con unas preguntas por voz")
noEspera("Rehacer")
espera("Preguntar sobre hoy")

-- Preguntar sobre hoy lleva la fecha del día.
local pregunta
fake.reply["viajes.preguntar"] = function(args)
  pregunta = args
  return true, {ok = true, answer = "El check-in es a las 15:00 en el Hotel Artemide.",
                spoken = "A las tres de la tarde."}
end
fake.key("up")                    -- desde la primera fila, arriba es la última: Preguntar
espera("Preguntar sobre hoy")
fake.heard = "¿a qué hora es el check-in?"
fake.key("ok")
espera("Esperando al servidor")
fake.step()                       -- la escucha
fake.step()                       -- viajes.preguntar
assert(pregunta.id == "ita" and pregunta.question == "¿a qué hora es el check-in?", "preguntar: args")
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
espera("Agenda · 13 días")        -- Inicio

-- ------------------------------------------------------------- 3. agenda
fake.key("down")
fake.key("ok")                    -- Agenda
espera("Agenda · Italia y España")
espera("Día 1 · sábado 14 de septiembre")
espera("Roma")                    -- el lugar en el detalle del día
espera("10:40  Vuelo IB3230 MAD → FCO")
espera("Día 2 · domingo 15 de septiembre")
espera("Día 3 · lunes 16 de septiembre")
espera("Roma · Vaticano por la mañana")
fake.key("up")                    -- da la vuelta: el último día
espera("Día 13 · jueves 26 de septiembre")
espera("Madrid")
espera("18:30  Vuelo de vuelta")

-- ------------------------------------------------------------- 3b. día
-- Día 9: tren, sin lugar ni hotel. La línea del lugar no aparece.
for _ = 1, 5 do fake.key("up") end  -- 13, i9, 12, i8, 11, 10 … contamos hasta llegar al día 9
espera("Día 10 · lunes 23 de septiembre")
for _ = 1, 2 do fake.key("up") end
fake.key("ok")                    -- Día 9
espera("domingo 22 de septiembre")
espera("Día 9 de 13")
noEspera("Hotel")
noEspera("Roma · ")               -- ni lugar ni hotel: no hay línea de lugar (el "Roma" del tren sí está)
espera("09:15  Tren Roma → Madrid")
espera("Tren · Termini")
espera("Guía de este día")
espera("Preguntar sobre este día")
fake.key("back")                  -- vuelve a la agenda, sobre el día de hoy (día 1)
fake.key("ok")                    -- Día 1
espera("sábado 14 de septiembre")
espera("Roma · Hotel Artemide")
espera("10:40  Vuelo IB3230 MAD → FCO")
espera("Preguntar sobre este día")

-- Día 5 (navegación): hotel sin lugar → sólo el hotel.
fake.key("back")
for _ = 1, 6 do fake.key("down") end   -- i1, i2, día 2, i3, día 3, día 4
espera("Día 4 · martes 17 de septiembre")
fake.key("down")                       -- i4
fake.key("down")                       -- día 5
fake.key("ok")
espera("miércoles 18 de septiembre")
espera(MSC)
espera("— libre —")
fake.key("back")                  -- la agenda vuelve a resaltar el día de hoy
fake.key("ok")                    -- Día 1 otra vez
espera("Día 1 de 13")

-- ------------------------------------------------------------- 4. ítem
fake.key("ok")                    -- el vuelo es la primera fila
espera("Vuelo IB3230 MAD → FCO")
espera("Día 1 · sábado 14 de septiembre · 10:40")
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
  assert(args.id == "ita" and args.paperId == "p1", "papel: args")
  return true, {ok = true, title = "Vuelo IB3230", text = "Localizador: ABC123\nAsiento: 12A\n\nGracias por volar."}
end
fake.key("ok")
fake.step()
assert(pedidosPapel == 1, "tenía que bajar el papel")
assert(abrio("papel-p1.txt", "Vuelo IB3230"), "no se abrió el papel")
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
assert(recordar[1].id == "ita" and recordar[1].itemId == "i1" and recordar[1].on == true, "recordar: encender")
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
espera("Agenda · Italia y España")
fake.key("back")
espera("Agenda · 13 días")

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
  assert(args.id == "ita", "llevar: id")
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
  assert(args.id == "ita")
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

-- ------------------------------------------------------------- 7. la guía del día 1, desde Día
fake.key("ok")                    -- Hoy = día 1
espera("Guía de este día")
local pedidosPreguntas, pedidosDia = {}, {}
fake.reply["viajes.guia.preguntas"] = function(args)
  pedidosPreguntas[#pedidosPreguntas + 1] = args
  assert(args.id == "ita", "guia.preguntas: id")
  return true, {ok = true, questions = {
    {key = "llegada", text = "¿Cómo y a qué hora llegas a Roma?"},
    {key = "intereses", text = "¿Qué te interesa más ese día?"},
  }}
end
fake.key("down")
fake.key("down")                  -- Guía de este día (tercera fila: i1, i2, guía)
fake.key("ok")                    -- sin guía en la tarjeta ni en el servidor: arranca sola
espera("Esperando al servidor")
fake.step()                       -- viajes.guia.preguntas → primera escucha
assert(pedidosPreguntas[1].date == "2026-09-14", "las preguntas son POR DÍA: faltó la fecha")
espera("Guía del día 1")
espera("Pregunta 1 de 2")
espera("¿Cómo y a qué hora llegas a Roma?")
-- Atrás salta la pregunta: el host corta la escucha y avisa con nil.
assert(fake.key("back") == true, "con la escucha abierta Atrás no sale")
fake.heard = nil
fake.step()
espera("Pregunta 2 de 2")
espera("¿Qué te interesa más ese día?")
local generar
fake.reply["viajes.guia.generar"] = function(args)
  generar = args
  return true, {ok = true, jobId = "g-1"}
end
fake.heard = "historia y comida"
fake.step()                       -- última respuesta → generar
fake.step()                       -- viajes.guia.generar
assert(generar.id == "ita" and generar.date == "2026-09-14", "generar: id y fecha")
assert(generar.answers.llegada == nil, "la pregunta saltada no viaja")
assert(generar.answers.intereses == "historia y comida", "generar: intereses")
espera("Armando la guía del día 1")
espera("Preparando")
assert(cp.load():find("job=g-1", 1, true), "el trabajo de la guía no se guardó")
assert(cp.load():find("jobDate=2026-09-14", 1, true), "el día del trabajo no se guardó")

local consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "g-1", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  if consultas < 3 then
    return true, {ok = true, state = "running", step = consultas, total = 3,
                  label = consultas == 1 and "Buscando cerca del Hotel Artemide…" or "Escribiendo la guía…"}
  end
  return true, {ok = true, state = "done", step = 3, total = 3}
end
fake.reply["viajes.guia.dia"] = function(args)
  pedidosDia[#pedidosDia + 1] = args
  assert(args.id == "ita", "guia.dia: id")
  return true, {ok = true, text = "Guía del " .. args.date .. ".\n\nCerca del hotel: Termini, Santa Maria Maggiore.",
                at = 1789001000}
end
fake.tick()
assert(consultas == 0, "no debe consultar antes de 5 s")
avanzar(5000)
fake.tick()
fake.step()
assert(consultas == 1, "primera consulta")
espera("Paso 1 de 3")
espera("Buscando cerca del Hotel Artemide")
avanzar(5000)
fake.tick()
fake.step()
assert(consultas == 2)
espera("Escribiendo la guía")
avanzar(5000)
fake.tick()
fake.step()                       -- done → pide la guía del día
assert(consultas == 3)
espera("Bajando la guía del día 1")
fake.step()                       -- viajes.guia.dia
assert(#pedidosDia == 1 and pedidosDia[1].date == "2026-09-14", "tenía que bajar la guía de ese día")
assert(not cp.busy())
local g1 = archivo("guia-2026-09-14.txt")
assert(g1:find("Guía del 2026-09-14.\n\nCerca del hotel", 1, true), "guia-2026-09-14.txt sin el texto")
assert(g1:find("^Día 1 · sábado 14 de septiembre · Roma"), "la guía lleva el día y el lugar arriba")
assert(ultimoAbierto() == "guia-2026-09-14.txt" and abrio("guia-2026-09-14.txt", "Guía · Día 1"),
       "la guía tenía que abrirse en el visor")
assert(not cp.load():find("job=g", 1, true), "el trabajo tenía que borrarse al terminar")
assert(archivo("viaje.json"):find('"ready":true', 1, true), "day.guide.ready tenía que quedar en viaje.json")

-- Volvió al Día, que ahora tiene "Rehacer". OK sobre la guía la abre de la
-- tarjeta, sin ir al servidor.
espera("sábado 14 de septiembre")
espera("Guía de este día")
espera("En la tarjeta")
espera("Rehacer la guía del día")
local abiertas = #fake.opened
fake.key("ok")                    -- el resalte sigue en Guía de este día
assert(#fake.opened == abiertas + 1 and ultimoAbierto() == "guia-2026-09-14.txt", "OK abre la guía guardada")
assert(not cp.busy() and #pedidosDia == 1 and #pedidosPreguntas == 1, "abrir de la tarjeta no va a la red")
fake.key("back")
espera("Guía · 1 de 13 días")

-- ------------------------------------------------------------- 7. Guía (7): la lista de los días
for _ = 1, 4 do fake.key("down") end
espera("Guía · 1 de 13 días")
fake.key("ok")
espera("Guía · Italia y España")
espera("Día 1 · sáb 14 · Roma")
espera("guía lista")
espera("Día 3 · lun 16 · Roma")
espera("en el servidor")
espera("Día 5 · mié 18")          -- sin lugar: sin el " · "
noEspera("Día 5 · mié 18 ·")
espera("Día 9 · dom 22")
espera("Página 1 de 2")
fake.key("up")                    -- da la vuelta: la segunda página
espera("Página 2 de 2")
espera("Día 13 · jue 26 · Madrid")
fake.key("down")                  -- y vuelve a la primera fila

-- El día 3 tiene guía en el servidor (hecha desde la web): se baja sin preguntas.
fake.key("down")
fake.key("down")                  -- Día 3
fake.key("ok")
espera("Bajando la guía del día 3")
fake.step()                       -- viajes.guia.dia
assert(#pedidosDia == 2 and pedidosDia[2].date == "2026-09-16", "la guía del servidor se baja directo")
assert(#pedidosPreguntas == 1, "bajar del servidor no pregunta nada")
assert(ultimoAbierto() == "guia-2026-09-16.txt", "y se abre")
assert(archivo("guia-2026-09-16.txt"):find("^Día 3 · lunes 16 de septiembre · Roma"))
espera("Guía · Italia y España")  -- volvió a la lista
fake.key("back")
espera("Guía · 2 de 13 días")

-- ------------------------------------------------------------- 7. Rehacer, salir a mitad, Retomar
-- Rehacer desde la lista de la Guía no confirma nada; esta vez el servidor
-- no pregunta, se sale a mitad del trabajo y se retoma desde el inicio.
for _ = 1, 4 do fake.key("down") end
fake.key("ok")                    -- Guía (7)
fake.key("ok")                    -- Día 1: tiene guía → la abre de la tarjeta
assert(ultimoAbierto() == "guia-2026-09-14.txt" and not cp.busy())
fake.key("back")
espera("Guía · 2 de 13 días")
fake.key("ok")                    -- Hoy (día 1)
fake.reply["viajes.guia.preguntas"] = function(args)
  pedidosPreguntas[#pedidosPreguntas + 1] = args
  return true, {ok = true, questions = {}}
end
fake.reply["viajes.guia.generar"] = function(args)
  assert(next(args.answers) == nil, "sin preguntas, sin respuestas")
  return true, {ok = true, jobId = "g-2"}
end
for _ = 1, 3 do fake.key("down") end  -- i1, i2, Guía, Rehacer
espera("Rehacer la guía del día")
abiertas = #fake.opened
fake.key("ok")
assert(#fake.opened == abiertas, "Rehacer no abre nada")
fake.step()                       -- preguntas (ninguna) → generar
fake.step()                       -- generar
espera("Armando la guía del día 1")
fake.key("back")                  -- salir; el trabajo sigue
espera("sábado 14 de septiembre") -- de vuelta en Hoy
espera("Retomar la guía del día")
espera("Se sigue armando en el servidor")
noEspera("Rehacer")
fake.key("back")
espera("Retomar la guía del día 1")
espera("Guía · 2 de 13 días")
fake.key("down")
for _ = 1, 4 do fake.key("down") end
fake.key("ok")                    -- Guía (7)
espera("generando…")
fake.key("back")
fake.reload()
on_open()
espera("Retomar la guía del día 1")
consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "g-2", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  return true, {ok = true, state = "done", step = 3, total = 3}
end
fake.reply["viajes.guia.dia"] = function(args)
  pedidosDia[#pedidosDia + 1] = args
  return true, {ok = true, text = "Guía NUEVA del " .. args.date .. ".", at = 1789002000}
end
fake.key("ok")                    -- Retomar
espera("Armando la guía del día 1")
fake.tick()                       -- al retomar pregunta en el acto
fake.step()
assert(consultas == 1, "al retomar tenía que consultar en el acto")
espera("Bajando la guía del día 1")
fake.step()
assert(pedidosDia[#pedidosDia].date == "2026-09-14")
assert(archivo("guia-2026-09-14.txt"):find("Guía NUEVA del 2026-09-14", 1, true), "rehacer tenía que pisar la guía vieja")
assert(ultimoAbierto() == "guia-2026-09-14.txt")
espera("Guía · 2 de 13 días")     -- volvió al inicio, sin Retomar
noEspera("Retomar")
assert(not cp.load():find("job=g", 1, true))

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
espera("Italia y España")
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
espera("Día 1 · mar 3 · Cusco · libre")   -- antes del viaje: el primer día
espera("Agenda · 8 días")
espera("Papeles · 0")
espera("Lista para llevar · 0 de 0")
espera("Guía · 0 de 8 días")
-- Los papeles y las guías eran de Italia: se fueron. Los tildes pendientes quedan.
assert(cp.size("guia-2026-09-14.txt") == nil and cp.size("guia-2026-09-16.txt") == nil and
       cp.size("papel-p1.txt") == nil, "los archivos del otro viaje se borran")
assert(archivo("pendientes.json"):find('"k1"', 1, true), "el tilde pendiente de Italia sigue ahí")
assert(archivo("viaje.json"):find('"Cusco"', 1, true))
fake.key("ok")                    -- Día 1 de Cusco
espera("Cusco · Hostal Qorikancha")
espera("Guía de este día")
espera("Se arma con unas preguntas por voz")
fake.key("back")

-- ------------------------------------------------------------- 10. actualizar con pendientes
llevar = {}
for _ = 1, 6 do fake.key("down") end
espera("Actualizar · hace un momento")
fake.key("ok")
espera("Subiendo los tildes pendientes · 0 de 1")
fake.step()                       -- viajes.llevar toggle
assert(#llevar == 1 and llevar[1].action == "toggle" and llevar[1].id == "ita" and llevar[1].itemId == "k1",
       "el tilde pendiente tenía que subir como toggle del viaje de Italia")
assert(cp.read("pendientes.json") == nil, "pendientes.json se borra al subir")
espera("Bajando la lista de viajes")
fake.step()                       -- viajes.lista (Italia sigue activo en la lista falsa)
fake.step()                       -- viajes.viaje
espera("Italia y España")
espera("Cambiar de viaje (2)")
espera("Guía · 0 de 13 días")     -- las guías de la tarjeta se borraron al cambiar

-- ------------------------------------------------------------- sin reloj
-- Sin hora del aparato manda el `today` del servidor; Italia dice 16 → día 3.
local reloj = cp.time
cp.time = function() return nil end
espera("Día 3 de 13 · lun 16")
espera("Hoy · lun 16 · Roma · libre")
fake.key("ok")                    -- Hoy
espera("lunes 16 de septiembre")
espera("Roma · Hotel Artemide")
espera("Vaticano por la mañana")
espera("Bajar del servidor")      -- la guía del día 3 está en el servidor, no en la tarjeta
fake.key("back")
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

-- Un trabajo que falla vuelve a la pantalla desde la que se pidió, con Reintentar.
fake.key("ok")                    -- Hoy (día 1 de Cusco)
fake.key("down")                  -- — libre — → Guía de este día
espera("Guía de este día")
fake.reply["viajes.guia.generar"] = function(args) return true, {ok = true, jobId = "g-3"} end
fake.key("ok")
fake.step()                       -- preguntas (ninguna)
fake.step()                       -- generar
fake.reply["job.status"] = function(args) return true, {ok = true, state = "failed", error = "sin resultados"} end
avanzar(5000)
fake.tick()
fake.step()
espera("No se pudo armar la guía")
espera("sin resultados")
espera("Reintentar")
assert(not cp.load():find("job=g", 1, true), "un trabajo fallido se olvida")
fake.key("down")
fake.key("ok")                    -- Volver → el Día
espera("martes 3 de noviembre")
noEspera("Retomar")
fake.key("back")

-- ------------------------------------------------------------- un viaje.json INCOMPLETO
-- Lo que llegó a la tarjeta no es lo que el código supone: días sin lugar,
-- hotel, guía ni ítems; un ítem sin título ni hora; un día sin fecha; un
-- viaje sin `today`; papeles y lista con entradas vacías. Nada de eso puede
-- tirar la app (el bug de la línea 763 de la v1 era exactamente esto).
assert(cp.write("viaje.json", '{"id":"raro","name":"Raro","days":[' ..
  '{"date":"2026-09-14"},' ..
  '{"n":2,"items":[{"kind":"visit"},{},{"at":"10:00"}]},' ..
  '{"date":"2026-09-16","guide":{"ready":true},"items":null,"place":null},' ..
  '{"date":"no-es-fecha","n":"x","label":7}' ..
  '],"packing":[{},{"id":"k1"}],"papers":[{},{"id":"p9"}]}'))
-- Y un trabajo guardado de otro viaje, que no tiene que resucitar acá.
cp.save("trip=ita\nviajes=2\nupdE=0\nupdM=0\njob=g-viejo\njobDate=2026-09-14\njobTrip=ita\n")
fake.reload()
on_open()
espera("Raro")
espera("Hoy · 2026-09-14 · libre")   -- sin `short` ni `label`, el día dice su fecha
noEspera("Retomar")
assert(not cp.load():find("job=g", 1, true), "el trabajo de otro viaje se olvida")
fake.key("ok")                    -- Hoy (día 1: sin lugar, sin hotel, sin ítems)
espera("Día 1 de 4")
espera("— libre —")
espera("Guía de este día")
noEspera("Hotel")
fake.key("back")
fake.key("down")
fake.key("ok")                    -- Agenda
espera("Día 1 · 2026-09-14")
espera("Día 2 · Día 2")           -- sin fecha ni label: "Día 2"
espera("Sin título")
espera("10:00  Sin título")
espera("Día 3 · 2026-09-16")
espera("Día 4 · 7")               -- un label numérico se muestra como texto
fake.key("down")
fake.key("ok")                    -- Día 2: sin fecha → sin filas de guía
espera("Día 2 de 4")
espera("10:00  Sin título")
noEspera("Guía de este día")
espera("Preguntar sobre este día")
fake.key("ok")                    -- el ítem sin título ni hora
espera("Sin título")
espera("Día 2 · Día 2")
espera("Recordar 2 h antes")
espera("Preguntar sobre esto")
fake.key("back")
fake.key("back")
fake.key("back")
espera("Raro")
for _ = 1, 4 do fake.key("down") end
fake.key("ok")                    -- Guía (7)
espera("Guía · Raro")
espera("Día 1 · 2026-09-14")
espera("Día 2 · 2")
espera("Día 3 · 2026-09-16")
espera("en el servidor")
espera("Día 4 · no-es-fecha")     -- la lista usa `short`, que sin nada cae en la fecha
for _ = 1, 3 do fake.key("down") end
fake.key("ok")                    -- Día 4: sin fecha válida, no puede tener guía
assert(not cp.busy(), "un día sin fecha no pide nada")
espera("Guía · Raro")
fake.key("back")
fake.key("down")
fake.key("down")
fake.key("ok")                    -- Papeles: dos papeles sin nada adentro
espera("Papeles · 2")
espera("Papel")
fake.reply["viajes.papel"] = function(args) return true, {ok = true, title = "", text = "Texto."} end
fake.key("ok")                    -- un papel sin id
fake.step()
assert(ultimoAbierto() == "papel-x.txt" and archivo("papel-x.txt"):find("^Papel\n\nTexto%."), "el papel sin id")
fake.key("back")
for _ = 1, 3 do fake.key("down") end
fake.key("ok")                    -- Lista: un ítem sin texto ni id
espera("Para llevar · 0 de 2")
fake.key("ok")                    -- tildar el ítem sin id no rompe
espera("Para llevar · 1 de 2")
fake.key("back")

-- ------------------------------------------------------------- salir
assert(fake.key("back") == false, "Atrás en el inicio sale de la app")
