-- Escenario de examples/Apps/ahorcado.lua: partidas enteras de escritorio, una
-- ganada y otra perdida. Corre DESPUÉS de cargar la app; `fake` y `cp` están a
-- mano. Falla con error() si algo no está donde tiene que estar.
--
-- La palabra se lee de lo que la app guarda con cp.save: es la misma vía por la
-- que se acuerda de la partida, así que probarla acá es probar el guardado.

local ABC = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "Ñ",
              "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" }
local SIN_TILDE = { ["Á"] = "A", ["É"] = "E", ["Í"] = "I", ["Ó"] = "O", ["Ú"] = "U", ["Ü"] = "U" }

local function dibujado(sub)
  for _, t in ipairs(fake.draw()) do
    if t:find(sub, 1, true) then return true end
  end
  return false
end

local function campos()
  local texto = cp.load()
  assert(type(texto) == "string" and texto ~= "", "hay algo guardado")
  local c = {}
  for linea in texto:gmatch("[^\n]+") do
    local k, v = linea:match("^(%a+)%s+(.-)%s*$")
    if k then c[k] = v end
  end
  return c
end

-- Las letras que hay que teclear para una palabra: el glifo sin tilde, sin
-- repetir. Es la misma cuenta que hace la app, escrita aparte a propósito.
local function claves(pal)
  local orden, visto = {}, {}
  for _, code in utf8.codes(pal) do
    local ch = utf8.char(code)
    local base = SIN_TILDE[ch] or ch
    if not visto[base] then
      visto[base] = true
      orden[#orden + 1] = base
    end
  end
  return orden, visto
end

-- La letra sobre la que está el cursor, leída del pie de la pantalla.
local function letraCursor()
  for _, t in ipairs(fake.draw()) do
    local l = t:match("OK: probar la (%S+)")
    if l then return l end
  end
  return nil
end

local function probar(letra)
  for _ = 1, #ABC + 1 do
    if letraCursor() == letra then
      assert(fake.key("ok") == true, "OK prueba la letra")
      return
    end
    assert(fake.key("down") == true, "la palanca mueve")
  end
  error("no se llegó a la letra " .. letra)
end

-- ---------------------------------------------------------------------------
-- 1. Sin nada guardado se entra directo a jugar.

on_open()
assert(cp.busy() == false, "la app no abre micrófono ni red: nunca")
assert(dibujado("Ahorcado"), "el cabezal")
assert(dibujado("Sin fallos todavía"), "arranca sin fallos")
assert(dibujado("Quedan 6"), "seis vidas")
assert(letraCursor() == "A", "el cursor arranca en la A")

local c = campos()
assert(c.ahorcado == "1" and c.marcador == "0 0", "marcador en cero")
assert(utf8.len(c.palabra) >= 4 and utf8.len(c.palabra) <= 12, "la palabra mide de 4 a 12 letras")
assert(c.probadas == "", "sin letras probadas")
assert(dibujado(c.categoria .. " · " .. utf8.len(c.palabra) .. " letras"), "la pista dice categoría y largo")

-- Las 27 teclas están en pantalla, la Ñ incluida.
do
  local vistas = {}
  for _, t in ipairs(fake.draw()) do vistas[t] = true end
  for _, l in ipairs(ABC) do assert(vistas[l], "falta la letra " .. l .. " en el abecedario") end
end

-- ---------------------------------------------------------------------------
-- 2. La lista entera: todas las palabras se pueden teclear con esas 27 letras,
-- miden de 4 a 12 y no se repiten. Si alguna trajera un signo raro, habría una
-- palabra imposible de ganar.

do
  local puede, vistas, cuantas = {}, {}, 0
  for _, l in ipairs(ABC) do puede[l] = true end
  for _, ch in pairs(SIN_TILDE) do assert(puede[ch], "la tilde cae en una letra del abecedario") end
  for k = 1, 4000 do
    fake.advance(1)
    local pal = campos().palabra
    if not vistas[pal] then
      vistas[pal] = true
      cuantas = cuantas + 1
      local largo = utf8.len(pal)
      assert(largo ~= nil, pal .. ": no es UTF-8 válido")
      assert(largo >= 4 and largo <= 12, pal .. ": mide " .. tostring(largo) .. " letras")
      for _, code in utf8.codes(pal) do
        local ch = utf8.char(code)
        assert(puede[SIN_TILDE[ch] or ch], pal .. ": la letra " .. ch .. " no está en el abecedario")
      end
    end
    -- Palabra nueva sin tocar el marcador: Atrás sale, se vuelve a abrir y se
    -- pide otra desde la pantalla de inicio.
    assert(fake.key("back") == false, "Atrás sale dejando la partida a medias")
    fake.reload()
    on_open()
    assert(dibujado("Hay una palabra a medias"), "la partida a medias se ofrece")
    fake.key("down")
    assert(fake.key("ok") == true, "Palabra nueva")
  end
  assert(cuantas >= 150, "la lista tiene variedad de verdad (" .. cuantas .. " palabras distintas)")
end

-- ---------------------------------------------------------------------------
-- 3. Una partida GANADA, con acentos y con Ñ: se carga a mano para que el caso
-- no dependa del azar.

cp.save("ahorcado 1\nmarcador 2 3\ncategoria Animales\npalabra CIGÜEÑA\nprobadas AC")
fake.reload()
on_open()
assert(dibujado("Hay una palabra a medias"), "con partida guardada, pantalla de inicio")
assert(dibujado("2 - 3"), "el marcador sobrevive")
assert(dibujado("Animales · 7 letras · 0 de 6 fallos"), "y la pista también")
assert(fake.key("ok") == true, "Continuar")
assert(letraCursor() ~= "A" and letraCursor() ~= "C", "el cursor no cae en una letra ya probada")

-- La U destapa la Ü: los acentos se adivinan con la letra pelada.
do
  local antes = fake.draw()
  local habia = false
  for _, t in ipairs(antes) do if t == "Ü" then habia = true end end
  assert(not habia, "la Ü todavía no está a la vista")
end
probar("U")
assert(dibujado("Ü"), "la U destapa la Ü")
assert(dibujado("Quedan 6"), "y no cuenta como fallo")

-- Un fallo: la B no está en CIGÜEÑA.
probar("B")
assert(dibujado("Erradas: B"), "la errada se lista aparte")
assert(dibujado("Quedan 5"), "y se descuenta")
assert(campos().probadas == "ABCU", "lo probado se guarda en orden")

-- El resto hasta ganar. La Ñ es una tecla más.
for _, l in ipairs({ "I", "G", "E", "Ñ" }) do probar(l) end
assert(dibujado("¡Ganaste!"), "se ganó")
assert(dibujado("3 - 3"), "y se anota en el marcador")
assert(cp.load() == "ahorcado 1\nmarcador 3 3", "la partida ganada no queda guardada")
assert(fake.key("down") == false, "ganado, la palanca no hace nada")
assert(fake.key("up") == false)

-- ---------------------------------------------------------------------------
-- 4. Una partida PERDIDA: seis letras que no están.

assert(fake.key("ok") == true, "OK: otra palabra")
local pal = campos().palabra
local _, enPalabra = claves(pal)
local erradas = 0
for _, l in ipairs(ABC) do
  if erradas < 6 and not enPalabra[l] then
    probar(l)
    erradas = erradas + 1
  end
end
assert(erradas == 6, "siempre hay seis letras de sobra fuera de la palabra")
assert(dibujado("Se acabó"), "se perdió")
assert(dibujado("3 - 4"), "y se anota")
-- Perder sin ver cuál era la palabra es lo único que no se perdona.
for _, code in utf8.codes(pal) do
  assert(dibujado(utf8.char(code)), "al perder se muestra la palabra entera")
end
assert(dibujado("Era " .. pal), "y escrita de corrido")
assert(cp.load() == "ahorcado 1\nmarcador 3 4", "la partida perdida no queda guardada")

-- ---------------------------------------------------------------------------
-- 5. Continuar de verdad: una partida a medias sobrevive a salir y volver.

assert(fake.key("ok") == true, "otra palabra")
local antes = campos()
probar(claves(antes.palabra)[1])
local medias = campos()
assert(fake.key("back") == false, "Atrás sale")
fake.reload()
on_open()
assert(dibujado("Hay una palabra a medias"))
assert(fake.key("ok") == true, "Continuar")
local despues = campos()
assert(despues.palabra == medias.palabra and despues.probadas == medias.probadas
       and despues.marcador == medias.marcador, "se continúa la misma partida")

-- Desde la pantalla de inicio también se sale.
assert(fake.key("back") == false, "Atrás sale del juego")
fake.reload()
on_open()
assert(fake.key("up") == true, "la palanca da la vuelta en el inicio")
assert(dibujado("Salir"), "arriba desde Continuar cae en Salir")
assert(fake.key("ok") == false, "la fila Salir cierra la app")

-- ---------------------------------------------------------------------------
-- 6. Un guardado roto, o de una partida ya terminada, es partida nueva.

for _, roto in ipairs({
  "basura",
  "ahorcado 2\nmarcador 1 1\npalabra CASA\nprobadas ",
  "ahorcado 1\nmarcador 1 1\npalabra CA\nprobadas ",                      -- muy corta
  "ahorcado 1\nmarcador 1 1\npalabra CA7SA\nprobadas ",                   -- un dígito
  "ahorcado 1\nmarcador 1 1\npalabra CASA\nprobadas AB\xff",              -- UTF-8 roto
  "ahorcado 1\nmarcador 1 1\npalabra CASA\nprobadas A7",                  -- probada rara
  "ahorcado 1\nmarcador 1 1\npalabra CASA\nprobadas BDFGHJ",              -- ya perdida
  "ahorcado 1\nmarcador 1 1\npalabra CASA\nprobadas ACS",                 -- ya ganada
  "ahorcado 1\nmarcador 1 1\npalabra \xc3\x28ASA\nprobadas A",            -- palabra rota
}) do
  cp.save(roto)
  fake.reload()
  on_open()
  assert(not dibujado("Hay una palabra a medias"), "guardado roto o terminado = partida nueva: " .. roto)
  assert(dibujado("Quedan 6"), "y se empieza con las seis")
  local nc = campos()
  assert(utf8.len(nc.palabra) >= 4, "con una palabra de la lista")
  assert(nc.probadas == "", "y sin letras probadas")
end

-- ---------------------------------------------------------------------------
-- 7. La palanca nunca se para sobre una letra ya probada, ni siquiera después
-- de muchas vueltas con media pantalla tachada.

probar("A")
probar("E")
probar("I")
local probs = campos().probadas
for _ = 1, #ABC * 2 do
  local l = letraCursor()
  assert(l ~= nil, "en juego el pie nombra la letra")
  assert(not probs:find(l, 1, true), "el cursor saltea la " .. l .. ", que ya se probó")
  fake.key("down")
end
assert(cp.busy() == false, "y al final sigue sin haber nada en curso")
