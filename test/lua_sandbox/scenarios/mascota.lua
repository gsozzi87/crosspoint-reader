-- Escenario de examples/Apps/mascota.lua: la vida entera de la mascota con el
-- cp falso del harness. Corre DESPUÉS de cargar la app; falla con error().
--
-- Orden: nombre por voz → pantalla con la perra (cp.image con los bytes que
-- corresponden) → alimentar → jugar las tres rondas → dormir y despertar →
-- limpiar → pasan horas (bajan las barras) → recargar (vuelve del cp.save) →
-- sin reloj → descuido largo → "se fue" → huevo nuevo → 50 ticks quietos.

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

-- El número de una barra, leído de la pantalla de Info ("Hambre: 73 %").
local function stat(nombre)
  fake.key("down"); fake.key("down"); fake.key("down"); fake.key("down")  -- botón 5: Info
  fake.key("ok")
  local d = espera("Edad:")
  local valor
  for _, s in ipairs(d) do
    local v = s:match("^" .. nombre .. ": (%d+)")
    if v then valor = tonumber(v) end
  end
  fake.key("back")
  fake.key("up"); fake.key("up"); fake.key("up"); fake.key("up")           -- de vuelta al 1
  if not valor then error("no se encontró «" .. nombre .. "» en Info: " .. table.concat(d, " | ")) end
  return valor
end

local function ultimaImagen()
  local im = fake.images and fake.images[#fake.images]
  if not im then error("no se dibujó ninguna imagen") end
  return im
end

local HORA = 3600

-- ------------------------------------------------------------- nombre por voz
on_open()
assert(cp.busy(), "al abrir sin estado tiene que pedir el nombre por voz")
espera("Un huevo nuevo")
espera("Di cómo se llama")
local im = ultimaImagen()
assert(im.w == 32 and im.h == 32 and im.bytes == 128 and im.escala == 6, "el huevo: 32x32 a escala 6")

fake.heard = "se llama pipo"
fake.step()
assert(not cp.busy(), "después de oír el nombre no queda nada pendiente")
local d = espera("Pipo")
assert(contiene(d, "¡Hola, Pipo!"), "el saludo con el nombre")
assert(contiene(d, "Día 1"), "arranca en el día 1")
assert(contiene(d, "Alimentar") and contiene(d, "Jugar") and contiene(d, "Dormir")
   and contiene(d, "Limpiar") and contiene(d, "Info"), "los cinco botones")

-- La perra: 32 x 16 a escala 7 son 4 bytes por fila → 64 bytes, 224 px de ancho.
fake.images = {}
fake.draw()
local perra
for _, i in ipairs(fake.images) do if i.w == 32 and i.h == 16 then perra = i end end
assert(perra, "no se dibujó la perra")
assert(perra.escala == 7 and perra.bytes >= 4 * 16, "la perra: 32 x 16 a escala 7, " .. perra.bytes .. " bytes")
assert(perra.x == (480 - 224) // 2, "la perra va centrada")

-- ------------------------------------------------------------- alimentar
local hambre0 = stat("Hambre")
assert(hambre0 == 80, "nace con 80 de hambre, tiene " .. hambre0)
fake.key("ok")                       -- botón 1: Alimentar
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
fake.key("down")                     -- botón 2: Jugar
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
fake.key("up")                       -- de vuelta al botón 1
local animo1 = stat("Ánimo")
assert(animo1 > animo0, "jugar sube el ánimo: " .. animo0 .. " → " .. animo1)

-- ------------------------------------------------------------- dormir y despertar
fake.key("down"); fake.key("down")   -- botón 3: Dormir
fake.key("ok")
d = espera("Duerme")
assert(contiene(d, "Despertar"), "dormida, el botón dice Despertar")
-- Dormida no come.
fake.key("up"); fake.key("up"); fake.key("ok")
espera("despiértala primero")
fake.key("down"); fake.key("down"); fake.key("ok")   -- Despertar
d = fake.draw()
assert(not contiene(d, "Despertar"), "despierta, el botón vuelve a Dormir")
fake.key("up"); fake.key("up")

-- ------------------------------------------------------------- limpiar
fake.advance(3 * HORA * 1000)        -- que se ensucie un poco antes
fake.tick()
local hig0 = stat("Higiene")
assert(hig0 < 80, "tres horas después está más sucia: " .. hig0)
fake.key("down"); fake.key("down"); fake.key("down")  -- botón 4: Limpiar
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
assert(type(guardado) == "string" and guardado:find("mascota1;Pipo;", 1, true), "el estado guardado lleva el nombre")
if type(fake.reload) == "function" then fake.reload() end
on_open()
assert(not cp.busy(), "con estado guardado no vuelve a pedir el nombre")
d = espera("Pipo")
assert(not contiene(d, "Un huevo nuevo"), "no vuelve a la pantalla del huevo")
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
espera("Pipo")
fake.key("ok")                       -- comer igual funciona sin hora
espera("Ñam, ñam")
local sinReloj0 = stat("Hambre")
assert(sinReloj0 > 20, "recién comida")
fake.advance(2 * HORA * 1000)
fake.tick()
local sinReloj1 = stat("Hambre")
assert(sinReloj1 < sinReloj0, "sin reloj el tiempo corre por cp.ms(): " .. sinReloj0 .. " → " .. sinReloj1)
cp.time = horaFija

-- ------------------------------------------------------------- descuido largo
-- Sin comer: el hambre llega a cero y desde ahí 36 h de descuido y se va.
-- Cada tramo se topa en 12 h, así que hacen falta varios.
local seFue = false
for tramo = 1, 8 do
  fake.advance(12 * HORA * 1000)
  fake.tick()
  d = fake.draw()
  if contiene(d, "Pipo se fue") then seFue = true break end
end
assert(seFue, "con días de descuido se tenía que ir")
d = fake.draw()
assert(contiene(d, "Vivió"), "la pantalla de despedida cuenta los días")
im = ultimaImagen()
assert(im.w == 32 and im.h == 16 and im.escala == 7, "el cuadro de despedida es la perra dada vuelta (32 x 16 a escala 7)")
assert(not fake.key("up"), "en la despedida la palanca no hace nada")
-- Guardado como ida: al recargar sigue ida.
fake.reload()
on_open()
espera("Pipo se fue")

-- ------------------------------------------------------------- huevo nuevo
fake.key("ok")
espera("Un huevo nuevo")
assert(cp.busy(), "el huevo nuevo pide el nombre por voz")
-- Atrás corta la escucha: on_heard(nil) → nombre por omisión.
fake.key("back")
fake.step()
d = espera("¡Hola, Pipo!")
assert(contiene(d, "Día 1"), "la nueva nace en el día 1")
assert(stat("Hambre") == 80, "la nueva nace con 80")

-- Un nombre largo se recorta a 12 letras y va con mayúscula.
fake.reload()
cp.save("")
on_open()
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
