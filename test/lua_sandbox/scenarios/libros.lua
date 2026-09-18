-- Escenario de examples/Apps/libros.lua: el flujo entero de punta a punta con
-- el cp falso del harness (docs/ws397/LIBROS_CONTRATO.md). Corre DESPUÉS de
-- cargar la app. Falla con error() si algo no está donde tiene que estar.
--
-- Orden: inicio vacío → sin Telegram → sin red (Reintentar) → buscar con la
-- lista de la captura del dueño → ficha con pdf y epub (epub primero) → Leer la
-- descripción → Bajar EPUB con job.status running (label, paso/total) → done →
-- cp.download a "books" con nombre seguro → Listo → Abrir en el lector →
-- bajados.json → recargar: Inicio lista el bajado y OK lo abre; Atrás largo lo
-- saca → ficha sin formato → sin resultados y OK vuelve a escuchar → trabajo
-- que falla → Reintentar → descarga que falla → Reintentar → un bajados.json
-- INCOMPLETO por Inicio → salir.

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

-- Posición del primer texto dibujado que contiene `sub` (para el orden).
local function posicion(sub)
  for i, s in ipairs(fake.draw()) do
    if type(s) == "string" and s:find(sub, 1, true) then return i end
  end
  error("no se dibujó «" .. sub .. "»")
end

local function avanzar(ms) fake.advance(ms) end

-- Lo que el harness anota en fake.opened: {kind, name, title}.
local function ultimoAbierto()
  local o = fake.opened[#fake.opened]
  return o and o.name or nil, o and o.kind or nil
end

-- cp.download se envuelve para ver con qué se llamó (el harness no lo anota).
local descargas = {}
do
  local orig = cp.download
  cp.download = function(fileId, nombre, destino)
    descargas[#descargas + 1] = {id = fileId, name = nombre, dest = destino}
    return orig(fileId, nombre, destino)
  end
end

-- La lista de la captura del dueño: cada título con su comando.
local LISTA = {
  {title = "Los cien pájaros", code = "/b1a2B"},
  {title = "Cien años de soledad", code = "/b0E_D"},
  {title = "Cien sonetos de amor", code = "/bZ9k1"},
  {title = "Cien años de perdón", code = "/bQq77"},
  {title = "El coronel no tiene quien le escriba", code = "/bHh3x"},
  {title = "Crónica de una muerte anunciada", code = "/bK0ll"},
  {title = "El amor en los tiempos del cólera", code = "/bM2p"},
  {title = "Del amor y otros demonios", code = "/bN8s"},
  {title = "Doce cuentos peregrinos", code = "/bP4t"},
  {title = "La hojarasca", code = "/bR5u"},
}
local DESC = "Muchos años después, frente al pelotón de fusilamiento, el coronel Aureliano Buendía había de " ..
             "recordar aquella tarde remota en que su padre lo llevó a conocer el hielo. Macondo era entonces " ..
             "una aldea de veinte casas de barro y cañabrava construidas a la orilla de un río de aguas " ..
             "diáfanas.\nLa novela cuenta la historia de la familia Buendía a lo largo de siete generaciones."

-- ------------------------------------------------------------- inicio
on_open()
espera("Libros")
espera("Presiona OK y di el título o el autor")
espera("Buscar por voz")
noEspera("Bajados")
assert(not cp.busy(), "abrir la app no pide nada a la red")

-- ------------------------------------------------------------- sin Telegram
fake.heard = "cien años de soledad"
fake.key("ok")                    -- Buscar por voz
assert(cp.busy(), "OK tenía que pedir el micrófono")
fake.step()                       -- on_heard → cp.call buscar
local buscadas = 0
fake.reply["libros.buscar"] = function(args)
  buscadas = buscadas + 1
  assert(args.q == "cien años de soledad", "buscar: q " .. tostring(args.q))
  return false, {error = "Conecta Telegram en la web (Ajustes → Avanzado → Telegram)", code = "no_telegram"}
end
espera("Esperando al servidor")
fake.step()
espera("Sin Telegram")
espera("Conecta Telegram en la web (Ajustes → Avanzado →")   -- se parte en dos renglones
espera("Telegram)")
espera("Reintentar")
espera("Volver")
fake.key("down")
fake.key("ok")                    -- Volver → inicio
espera("Buscar por voz")

-- ------------------------------------------------------------- sin red, Reintentar
fake.reply["libros.buscar"] = function(args)
  buscadas = buscadas + 1
  return false, {error = "sin conexión con el servidor (0)"}
end
fake.heard = "cien años de soledad"
fake.key("ok")
fake.step()
fake.step()
espera("No se pudo")
espera("sin conexión con el servidor (0)")
fake.reply["libros.buscar"] = function(args)
  buscadas = buscadas + 1
  assert(args.q == "cien años de soledad", "reintentar: la misma q")
  return true, {ok = true, results = LISTA}
end
fake.key("ok")                    -- Reintentar (primera fila)
fake.step()
assert(buscadas == 3, "tres búsquedas: " .. buscadas)

-- ------------------------------------------------------------- resultados
espera("cien años de soledad")    -- lo dicho, como cabezal
espera("10 resultados")
espera("Los cien pájaros")
espera("Cien años de soledad")
espera("La hojarasca")
noEspera("/b0E_D")                -- el comando no se muestra
noEspera("Página")               -- diez filas entran en una pantalla

-- OK sobre el segundo → ficha con su código.
local fichas = 0
fake.reply["libros.ficha"] = function(args)
  fichas = fichas + 1
  assert(args.code == "/b0E_D", "ficha: code " .. tostring(args.code))
  return true, {ok = true, title = "Cien años de soledad", author = "Gabriel García Márquez", year = 1967,
                pages = 345, genre = "Novela Drama", desc = DESC, formats = {"pdf", "epub", "PDF"}}
end
fake.key("down")
fake.key("ok")
fake.step()

-- ------------------------------------------------------------- ficha
espera("Cien años de soledad")
espera("Gabriel García Márquez")
espera("1967 · 345 páginas · Novela Drama")
espera("Muchos años después")
espera("Bajar EPUB")
espera("Bajar PDF")
espera("Leer la descripción")
espera("Otra búsqueda")
assert(posicion("Bajar EPUB") < posicion("Bajar PDF"), "epub va primero aunque el bot lo mande segundo")
noEspera("Este bot no da el archivo")

-- Leer la descripción: va a ficha.txt y al visor, con el título.
fake.key("down")
fake.key("down")
fake.key("ok")
local nombre, clase = ultimoAbierto()
assert(nombre == "ficha.txt" and clase == "view", "la descripción se lee en el visor: " .. tostring(nombre))
assert(fake.opened[#fake.opened].title == "Cien años de soledad", "el visor lleva el título")
local ficha = cp.read("ficha.txt")
assert(ficha and ficha:find("siete generaciones", 1, true) and ficha:find("1967 · 345 páginas", 1, true),
       "ficha.txt trae la descripción y los datos")

-- Atrás desde la ficha vuelve a la lista, sobre el elegido.
fake.key("back")
espera("10 resultados")
fake.key("ok")                    -- otra vez la ficha: el cursor quedó sobre el elegido
fake.step()
assert(fichas == 2, "segunda ficha, con el mismo código")
espera("Bajar EPUB")

-- ------------------------------------------------------------- bajar
local bajadas = 0
fake.reply["libros.bajar"] = function(args)
  bajadas = bajadas + 1
  assert(args.code == "/b0E_D", "bajar: code " .. tostring(args.code))
  assert(args.format == "epub", "bajar: format " .. tostring(args.format))
  return true, {ok = true, jobId = "j-9"}
end
fake.key("ok")                    -- Bajar EPUB es la primera fila
fake.step()
espera("Bajando")
espera("Cien años de soledad")
espera("Pidiendo el libro al bot")

local consultas = 0
fake.reply["job.status"] = function(args)
  assert(args.id == "j-9", "job.status: id " .. tostring(args.id))
  consultas = consultas + 1
  if consultas == 1 then
    return true, {ok = true, state = "running", label = "Pidiendo la ficha…"}
  elseif consultas == 2 then
    return true, {ok = true, state = "running", step = 2, total = 3, label = "Esperando el archivo…"}
  end
  return true, {ok = true, state = "done", step = 3, total = 3,
                files = {{id = "f-1", name = "Cien años de soledad.epub", bytes = 812000}}}
end
fake.tick()                       -- recién pedido: todavía no consulta
assert(consultas == 0, "no consulta antes de 3 s")
avanzar(3000)
fake.tick()
fake.step()
assert(consultas == 1, "primera consulta")
espera("Pidiendo la ficha…")
noEspera("Paso ")
fake.tick()
assert(consultas == 1, "no vuelve a consultar antes de 3 s")
avanzar(3000)
fake.tick()
fake.step()
assert(consultas == 2, "segunda consulta")
espera("Esperando el archivo…")
espera("Paso 2 de 3")
avanzar(3000)
fake.tick()
fake.step()                       -- done → arranca la descarga
assert(consultas == 3, "tercera consulta")
assert(#descargas == 1, "una descarga pedida")
assert(descargas[1].id == "f-1", "descarga: id " .. tostring(descargas[1].id))
assert(descargas[1].dest == "books", "descarga: destino " .. tostring(descargas[1].dest))
assert(descargas[1].name == "Cien-anos-de-soledad.epub",
       "el nombre del bot se vuelve seguro: " .. tostring(descargas[1].name))
espera("Bajando el archivo a la tarjeta")
fake.download["f-1"] = "EPUB de mentira"
fake.step()                       -- la descarga termina

-- ------------------------------------------------------------- listo
espera("Listo")
espera("Cien años de soledad")
espera("Guardado en /Books/libros/Cien-anos-de-soledad.epub")
espera("Abrir en el lector")
espera("Buscar otro")
local anotado = cp.read("bajados.json")
assert(anotado and anotado:find("Cien-anos-de-soledad.epub", 1, true) and anotado:find("Gabriel", 1, true),
       "bajados.json anota el libro: " .. tostring(anotado))

fake.key("ok")                    -- Abrir en el lector
nombre, clase = ultimoAbierto()
assert(nombre == "Cien-anos-de-soledad.epub" and clase == "book", "no se abrió el EPUB en el lector")

-- ------------------------------------------------------------- recargar: Bajados
fake.reload()
on_open()
espera("Bajados")
espera("Cien años de soledad")
espera("Gabriel García Márquez")
fake.key("down")                  -- la palanca saltea el renglón "Bajados"
fake.key("ok")
nombre, clase = ultimoAbierto()
assert(nombre == "Cien-anos-de-soledad.epub" and clase == "book", "OK sobre un bajado lo abre")
assert(#fake.opened == 3, "tres aperturas hasta acá")

-- Atrás largo sobre el bajado lo saca de la lista (el archivo queda).
assert(on_key("backlong") == true, "Atrás largo sobre un bajado repinta")
noEspera("Bajados")
noEspera("Gabriel García Márquez")
assert(cp.read("bajados.json") == "[]", "bajados.json quedó vacío: " .. tostring(cp.read("bajados.json")))
assert(on_key("backlong") == false, "Atrás largo sin bajado no hace nada")

-- ------------------------------------------------------------- ficha sin formato
fake.reply["libros.buscar"] = function(args) return true, {ok = true, results = LISTA} end
fake.reply["libros.ficha"] = function(args)
  assert(args.code == "/b1a2B", "ficha: code " .. tostring(args.code))
  return true, {ok = true, title = "Los cien pájaros", author = "", formats = {}, desc = ""}
end
fake.heard = "los cien pájaros"
fake.key("ok")
fake.step()
fake.step()
espera("los cien pájaros")
fake.key("ok")                    -- el primero
fake.step()
espera("Los cien pájaros")
espera("Este bot no da el archivo")
noEspera("Bajar ")
noEspera("Leer la descripción")   -- sin descripción no hay qué leer
noEspera("páginas")
espera("Otra búsqueda")
assert(on_key("ok") == false, "OK sobre «no da el archivo» no hace nada")
assert(not cp.busy(), "y no pide nada")

-- ------------------------------------------------------------- sin resultados
fake.reply["libros.buscar"] = function(args)
  assert(args.q == "un libro que no existe", "buscar: q " .. tostring(args.q))
  return true, {ok = true, results = {}}
end
fake.key("down")
fake.key("ok")                    -- Otra búsqueda
fake.heard = "un libro que no existe"
fake.step()
fake.step()
espera("No encontré nada con «un libro que no existe»")
espera("Buscar de nuevo")
fake.reply["libros.buscar"] = function(args)
  assert(args.q == "cien años de soledad", "buscar de nuevo: q " .. tostring(args.q))
  return true, {ok = true, results = LISTA}
end
fake.key("ok")                    -- OK vuelve a escuchar
assert(cp.busy(), "OK sin resultados tenía que pedir el micrófono")
fake.heard = "cien años de soledad"
fake.step()
fake.step()
espera("10 resultados")
-- Y Atrás desde los resultados vuelve a Inicio.
fake.key("back")
espera("Buscar por voz")

-- ------------------------------------------------------------- trabajo que falla
fake.reply["libros.ficha"] = function(args)
  return true, {ok = true, title = "Cien años de soledad", author = "Gabriel García Márquez", year = 1967,
                pages = 345, genre = "Novela Drama", desc = DESC, formats = {"epub"}}
end
fake.heard = "cien años de soledad"
fake.key("ok")
fake.step()
fake.step()
fake.key("down")
fake.key("ok")                    -- Cien años de soledad
fake.step()
espera("Bajar EPUB")
fake.reply["libros.bajar"] = function(args)
  bajadas = bajadas + 1
  return true, {ok = true, jobId = 44}   -- un número también vale
end
fake.key("ok")                    -- Bajar EPUB
fake.step()
fake.reply["job.status"] = function(args)
  assert(args.id == "44", "job.status: id " .. tostring(args.id))
  return true, {ok = true, state = "failed", error = "El bot no contestó en 120 s"}
end
avanzar(3000)
fake.tick()
fake.step()
espera("No se pudo bajar")
espera("El bot no contestó en 120 s")
espera("Reintentar")
local antes = bajadas
fake.reply["libros.bajar"] = function(args)
  bajadas = bajadas + 1
  assert(args.format == "epub", "reintentar: mismo formato")
  return true, {ok = true, jobId = "j-10"}
end
fake.key("ok")                    -- Reintentar → nuevo trabajo
fake.step()
assert(bajadas == antes + 1, "Reintentar pide otro trabajo")
espera("Bajando")

-- ------------------------------------------------------------- descarga que falla
fake.reply["job.status"] = function(args)
  assert(args.id == "j-10", "job.status: id " .. tostring(args.id))
  return true, {ok = true, state = "done", files = {{id = "f-2", name = "", bytes = 10}}}
end
avanzar(3000)
fake.tick()
fake.step()
assert(#descargas == 2 and descargas[2].name == "Cien-anos-de-soledad.epub",
       "sin nombre del bot, el título con el formato: " .. tostring(descargas[2] and descargas[2].name))
fake.downloadFails = true
fake.step()
espera("No se pudo")
espera("descarga fallida (1)")
fake.downloadFails = false
fake.key("ok")                    -- Reintentar → otra descarga
assert(#descargas == 3 and descargas[3].id == "f-2", "Reintentar vuelve a bajar el mismo archivo")
fake.step()
espera("Guardado en /Books/libros/Cien-anos-de-soledad.epub")
anotado = cp.read("bajados.json")
assert(anotado and select(2, anotado:gsub("Cien%-anos", "")) == 1, "el mismo archivo no se anota dos veces")
fake.key("back")                  -- Listo → inicio
espera("Bajados")
espera("Cien años de soledad")

-- ------------------------------------------------------------- bajados.json INCOMPLETO
-- Lo que llegó a la tarjeta no es lo que el código supone: entradas sin
-- nombre, sin título, con author null, con `at` numérico; números y textos
-- sueltos en la lista. Nada de eso puede tirar la app.
assert(cp.write("bajados.json", '[{"title":"Sin nombre"},{"name":"solo-nombre.epub"},7,"texto",null,' ..
  '{"name":"bueno.epub","title":"Bueno","author":null,"at":5},{"name":"","title":"Vacío"}]'))
fake.reload()
on_open()
espera("Bajados")
espera("solo-nombre.epub")        -- sin título, el nombre
espera("Bueno")
espera("5")                       -- el `at` numérico se muestra como texto
noEspera("Sin nombre")
noEspera("Vacío")
fake.key("down")
fake.key("down")
fake.key("down")                  -- da la vuelta hasta Buscar por voz
espera("Buscar por voz")

-- Un JSON que no es una lista, y uno roto: Inicio sin bajados y sin error.
assert(cp.write("bajados.json", '"hola"'))
on_open()
noEspera("Bajados")
assert(cp.write("bajados.json", '[{"name":"x.epub",'))
on_open()
noEspera("Bajados")
espera("Buscar por voz")

-- Un bajado que ya no está en la tarjeta: se saca de la lista y se dice.
assert(cp.write("bajados.json", '[{"name":"se-fue.epub","title":"Se fue"}]'))
on_open()
espera("Se fue")
fake.key("down")
fake.key("ok")
espera("No está el libro")
espera("se-fue.epub")
assert(cp.read("bajados.json") == "[]", "el que no está se saca de la lista")
fake.key("ok")                    -- Volver
noEspera("Bajados")

-- ------------------------------------------------------------- salir
assert(on_key("back") == false, "Atrás en Inicio sale de la app")
