-- Escenario de examples/Apps/mascota.lua: la vida entera de la mascota con el
-- cp falso del harness. Corre DESPUÉS de cargar la app; falla con error().
--
-- Orden: la pantalla del nombre (escucha que falla → se queda; escucha que
-- entiende → nace) → pantalla con la perra (cp.image con los bytes que
-- corresponden) → alimentar → jugar las tres rondas → dormir y despertar →
-- limpiar → pasan horas (bajan las barras) → recargar (vuelve del cp.save) →
-- sin reloj → cambiar el nombre desde Info → descuido largo → "se fue" →
-- huevo nuevo con el nombre de fábrica → recorte del nombre → gestos →
-- 50 ticks quietos.

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
  return d
end

local function noEspera(sub)
  local d = fake.draw()
  if contiene(d, sub) then error("se dibujó «" .. sub .. "» y no debía") end
end

-- El número de una barra, leído de la pantalla de Info ("Hambre" · "73 %"):
-- en Info cada dato son dos textos seguidos, la etiqueta y el valor.
local function stat(nombre)
  fake.key("down"); fake.key("down"); fake.key("down"); fake.key("down")  -- fila 5: Info
  fake.key("ok")
  local d = espera("Edad")
  local valor
  for i, s in ipairs(d) do
    if s == nombre and type(d[i + 1]) == "string" then valor = tonumber(d[i + 1]:match("^(%d+) %%")) end
  end
  fake.key("back")
  fake.key("up"); fake.key("up"); fake.key("up"); fake.key("up")           -- de vuelta a la 1
  if not valor then error("no se encontró «" .. nombre .. "» en Info: " .. table.concat(d, " | ")) end
  return valor
end

local function ultimaImagen()
  local im = fake.images and fake.images[#fake.images]
  if not im then error("no se dibujó ninguna imagen") end
  return im
end

-- La perra en pantalla: la imagen más grande que se dibujó (los iconos son de 16).
local function perraEnPantalla()
  fake.images = {}
  fake.draw()
  local perra
  for _, i in ipairs(fake.images) do if i.w >= 100 then perra = i end end
  if not perra then error("no se dibujó la perra") end
  return perra
end

local HORA = 3600

-- ------------------------------------------------------------- la pantalla del nombre
on_open()
assert(not cp.busy(), "al abrir sin estado pregunta en pantalla, no abre el micrófono solo")
local d = espera("¿Cómo se llama tu perrita?")
assert(contiene(d, "Decir el nombre") and contiene(d, "Se llama Pipo"), "las dos filas del nombre")
local im = ultimaImagen()
assert(im.w == 32 and im.h == 32 and im.bytes == 128 and im.escala == 6, "el huevo: 32x32 a escala 6")

-- OK en la primera fila escucha; una escucha que no entiende NO bautiza sola.
assert(fake.key("ok"), "OK en «Decir el nombre» repinta")
assert(cp.busy(), "queda escuchando")
espera("Di el nombre")
fake.heard = nil
fake.step()
assert(not cp.busy())
d = espera("No entendí, intenta otra vez")
assert(contiene(d, "¿Cómo se llama tu perrita?"), "sigue en la pantalla del nombre")
noEspera("¡Hola")
-- Atrás sin mascota sale de la app (y la próxima vez vuelve a preguntar).
assert(fake.key("back") == false, "Atrás sin mascota sale")

-- Segunda escucha, ahora con un nombre.
fake.key("ok")
fake.heard = "se llama Luna, la perrita."
fake.step()
assert(not cp.busy(), "después de oír el nombre no queda nada pendiente")
d = espera("Luna")
assert(contiene(d, "¡Hola, Luna!"), "el saludo con el nombre limpio (sin la coma ni lo que siga)")
assert(contiene(d, "Día 1"), "arranca en el día 1")
assert(contiene(d, "Alimentar") and contiene(d, "Jugar") and contiene(d, "Dormir")
   and contiene(d, "Limpiar") and contiene(d, "Info"), "las cinco filas")
assert(contiene(d, "Hambre") and contiene(d, "Higiene"), "las barras con etiqueta")

-- La perra: la silueta parada es 150 x 56 a escala 2 → 19 bytes por fila, 300 px de ancho.
local perra = perraEnPantalla()
assert(perra.w == 150 and perra.h == 56, "la silueta parada es 150 x 56, es " .. perra.w .. "x" .. perra.h)
assert(perra.escala == 2 and perra.bytes >= 19 * 56, "a escala 2 con " .. perra.bytes .. " bytes")
assert(perra.x == (480 - 300) // 2, "la perra va centrada")
assert(perra.y >= 96 and perra.y + 112 <= 304, "la perra queda dentro de su zona")

-- ------------------------------------------------------------- alimentar
local hambre0 = stat("Hambre")
assert(hambre0 == 80, "nace con 80 de hambre, tiene " .. hambre0)
fake.key("ok")                       -- fila 1: Alimentar
espera("Ñam, ñam")
local hambre1 = stat("Hambre")
assert(hambre1 > hambre0, "alimentar sube el hambre: " .. hambre0 .. " → " .. hambre1)
-- Con la panza llena no come más.
fake.key("ok")
assert(stat("Hambre") == 100, "llena en 100")
fake.key("ok")
espera("No tiene hambre")

-- ------------------------------------------------------------- jugar
local animo0 = stat("Ánimo")
fake.key("down")                     -- fila 2: Jugar
fake.key("ok")
espera("Mayor o menor")
espera("Ronda 1 de 3")
-- Tres rondas, cada una con la palanca. El resultado de cada una da igual:
-- lo que se prueba es que el juego termina y paga.
for ronda = 1, 3 do
  fake.key(ronda % 2 == 0 and "down" or "up")
end
local fin = espera("se divirtió")
assert(contiene(fin, "de 3"), "el marcador final")
-- Terminado, la palanca no hace nada y OK vuelve a casa.
assert(not fake.key("up"), "terminado el juego la palanca no repinta")
fake.key("ok")
espera("Alimentar")
fake.key("up")                       -- de vuelta a la fila 1
local animo1 = stat("Ánimo")
assert(animo1 > animo0, "jugar sube el ánimo: " .. animo0 .. " → " .. animo1)
-- Contenta y recién jugó: el cuadro es la silueta parada, no la dormida.
perra = perraEnPantalla()
assert(perra.h == 56, "contenta se dibuja parada")

-- ------------------------------------------------------------- dormir y despertar
fake.key("down"); fake.key("down")   -- fila 3: Dormir
fake.key("ok")
d = espera("Duerme")
assert(contiene(d, "Despertar"), "dormida, la fila dice Despertar")
-- Dormida es la perra enroscada: 120 x 81 a escala 2 (240 px de ancho).
perra = perraEnPantalla()
assert(perra.w == 120 and perra.h == 81 and perra.escala == 2, "dormida: 120 x 81 a escala 2, es " .. perra.w .. "x" .. perra.h)
assert(perra.x == (480 - 240) // 2, "la dormida también va centrada")
-- Dormida no come.
fake.key("up"); fake.key("up"); fake.key("ok")
espera("despiértala primero")
fake.key("down"); fake.key("down"); fake.key("ok")   -- Despertar
d = fake.draw()
assert(not contiene(d, "Despertar"), "despierta, la fila vuelve a Dormir")
fake.key("up"); fake.key("up")

-- ------------------------------------------------------------- limpiar
fake.advance(3 * HORA * 1000)        -- que se ensucie un poco antes
fake.tick()
local hig0 = stat("Higiene")
assert(hig0 < 80, "tres horas después está más sucia: " .. hig0)
fake.key("down"); fake.key("down"); fake.key("down")  -- fila 4: Limpiar
fake.key("ok")
espera("Quedó limpia")
fake.key("up"); fake.key("up"); fake.key("up")
assert(stat("Higiene") == 100, "limpia del todo")

-- ------------------------------------------------------------- pasan las horas
local h0, a0, e0 = stat("Hambre"), stat("Ánimo"), stat("Energía")
fake.advance(6 * HORA * 1000)
assert(fake.tick(), "seis horas después hay que repintar")
local h1, a1, e1 = stat("Hambre"), stat("Ánimo"), stat("Energía")
assert(h1 < h0 and a1 < a0 and e1 < e0, "seis horas bajan las tres barras")
assert(h0 - h1 >= 40 and h0 - h1 <= 56, "el hambre baja 8 por hora: " .. h0 .. " → " .. h1)

-- ------------------------------------------------------------- recargar
local guardado = cp.load()
assert(type(guardado) == "string" and guardado:find("mascota1;Luna;", 1, true), "el estado guardado lleva el nombre")
if type(fake.reload) == "function" then fake.reload() end
on_open()
assert(not cp.busy(), "con estado guardado no vuelve a pedir el nombre")
d = espera("Luna")
assert(not contiene(d, "¿Cómo se llama tu perrita?"), "no vuelve a la pantalla del nombre")
local h2 = stat("Hambre")
assert(math.abs(h2 - h1) <= 1, "el hambre vuelve del guardado: " .. h1 .. " → " .. h2)

-- Al volver con reloj, descuenta lo que estuvo cerrada, con tope de 12 h:
-- se guarda con un epoch viejo y se vuelve a abrir.
fake.key("ok"); fake.key("ok"); fake.key("ok")   -- a 100 de hambre antes del viaje
assert(stat("Hambre") == 100, "llena antes de dejarla sola")
local horaFija = cp.time
local base = cp.time().epoch
cp.time = function() return {epoch = base + 30 * HORA} end   -- 30 h después
on_open()
d = espera("Te esperó 12 h")
local h3 = stat("Hambre")
-- 30 h sin comer la dejarían en cero; con el tope de 12 h a 8 por hora quedan 4.
assert(h3 >= 2 and h3 <= 6, "12 h de tope sobre 30 h fuera: 100 → " .. h3)
espera("Tiene hambre")
cp.time = horaFija

-- ------------------------------------------------------------- sin reloj
cp.time = function() return nil end
on_open()
espera("Luna")
fake.key("ok")                       -- comer igual funciona sin hora
espera("Ñam, ñam")
local sinReloj0 = stat("Hambre")
assert(sinReloj0 > 20, "recién comida")
fake.advance(2 * HORA * 1000)
fake.tick()
local sinReloj1 = stat("Hambre")
assert(sinReloj1 < sinReloj0, "sin reloj el tiempo corre por cp.ms(): " .. sinReloj0 .. " → " .. sinReloj1)
cp.time = horaFija

-- ------------------------------------------------------------- cambiar el nombre desde Info
fake.key("down"); fake.key("down"); fake.key("down"); fake.key("down")  -- fila 5: Info
fake.key("ok")
d = espera("Cambiar el nombre")
assert(contiene(d, "Edad"), "es la pantalla de Info")
fake.key("ok")                       -- OK en Info escucha
assert(cp.busy(), "Info: OK abre el micrófono para el nombre nuevo")
fake.heard = "   "
fake.step()
d = espera("No entendí, intenta otra vez")
assert(contiene(d, "Cambiar el nombre") and contiene(d, "Luna"), "sin entender, sigue en Info y sigue siendo Luna")
fake.key("ok")
fake.heard = "Canela"
fake.step()
d = espera("Ahora se llama Canela")
assert(contiene(d, "Canela") and not contiene(d, "Luna"), "el cabezal de Info muestra el nombre nuevo")
fake.key("back")
espera("Canela")
assert(cp.load():find("mascota1;Canela;", 1, true), "el nombre nuevo se guardó")
fake.key("up"); fake.key("up"); fake.key("up"); fake.key("up")           -- de vuelta a la fila 1

-- ------------------------------------------------------------- descuido largo
-- Sin comer: el hambre llega a cero y desde ahí 36 h de descuido y se va.
-- Cada tramo se topa en 12 h, así que hacen falta varios.
local seFue = false
for tramo = 1, 8 do
  fake.advance(12 * HORA * 1000)
  fake.tick()
  d = fake.draw()
  if contiene(d, "Canela se fue") then seFue = true break end
end
assert(seFue, "con días de descuido se tenía que ir")
d = fake.draw()
assert(contiene(d, "Vivió"), "la pantalla de despedida cuenta los días")
-- El cuadro de despedida es la silueta espejada y más chica: 100 x 38 a escala 2.
perra = perraEnPantalla()
assert(perra.w == 100 and perra.h == 38 and perra.escala == 2, "se fue: 100 x 38 a escala 2, es " .. perra.w .. "x" .. perra.h)
assert(not fake.key("up"), "en la despedida la palanca no hace nada")
-- Guardado como ida: al recargar sigue ida.
fake.reload()
on_open()
espera("Canela se fue")

-- ------------------------------------------------------------- huevo nuevo
fake.key("ok")
d = espera("¿Cómo se llama tu perrita?")
assert(not cp.busy(), "el huevo nuevo pregunta en pantalla, no escucha solo")
-- Segunda fila: el nombre de fábrica, sin hablar.
fake.key("down")
fake.key("ok")
d = espera("¡Hola, Pipo!")
assert(contiene(d, "Día 1"), "la nueva nace en el día 1")
assert(stat("Hambre") == 80, "la nueva nace con 80")

-- Atrás durante la escucha la corta: on_heard(nil) → se queda preguntando.
fake.reload()
cp.save("")
on_open()
fake.key("ok")
assert(cp.busy())
assert(fake.key("back"), "Atrás corta la escucha")
fake.heard = nil
fake.step()
espera("No entendí, intenta otra vez")

-- Un nombre largo se recorta a 12 letras y va con mayúscula.
fake.key("ok")
fake.heard = "abcdefghijklmnopqrstuvwxyz"
fake.step()
espera("Abcdefghijkl")
noEspera("Abcdefghijklm")

-- ------------------------------------------------------------- gestos
-- El harness no tiene acelerómetro: se le presta uno que entrega un gesto y
-- se calla. Boca abajo la duerme; sacudir la despierta de golpe y de mal humor.
local gestoPendiente = nil
local motionReal = cp.motion
cp.motion = function()
  local g = gestoPendiente
  gestoPendiente = nil
  return g
end
local animoAntes = stat("Ánimo")
gestoPendiente = "FaceDown"
assert(fake.tick(), "boca abajo repinta")
d = espera("Duerme")
assert(contiene(d, "Se durmió"), "boca abajo la duerme")
gestoPendiente = "Shake"
assert(fake.tick(), "la sacudida repinta")
d = espera("¡Se despertó de golpe!")
assert(not contiene(d, "Despertar"), "sacudida: despierta")
assert(stat("Ánimo") < animoAntes, "sacudirla le baja el ánimo")
cp.motion = motionReal
assert(cp.motion() == nil)

-- ------------------------------------------------------------- 50 ticks quietos
fake.draw()
for i = 1, 50 do
  if fake.tick() then error("tick " .. i .. " repintó sin que cambiara nada") end
end
-- Y a los 4 s cambia el cuadro de la animación: UNA repintada, no cincuenta.
fake.advance(4000)
assert(fake.tick(), "a los 4 s cambia el cuadro")
assert(not fake.tick(), "y el tick siguiente no repinta")

-- Salir guarda.
cp.save("")
assert(fake.key("back") == false, "Atrás sale de la app")
assert(type(cp.load()) == "string" and cp.load():find("mascota1;", 1, true), "al salir se guardó")
