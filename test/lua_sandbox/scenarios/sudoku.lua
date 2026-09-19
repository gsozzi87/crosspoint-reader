-- Escenario de examples/Apps/sudoku.lua: una partida entera de escritorio.
-- Corre DESPUÉS de cargar la app; `fake` y `cp` están a mano. Falla con
-- error() si algo no está donde tiene que estar.
--
-- El tablero se lee de lo que la app guarda con cp.save (dadas, solución y lo
-- puesto, 81 dígitos cada uno): es la misma vía por la que la app se acuerda
-- de la partida, así que probarla acá es probar también el guardado.

local function dibujado(sub)
  for _, t in ipairs(fake.draw()) do
    if t:find(sub, 1, true) then return true end
  end
  return false
end

local function campos()
  local texto = cp.load()
  assert(type(texto) == "string" and texto ~= "", "hay partida guardada")
  local c = {}
  for linea in texto:gmatch("[^\n]+") do
    local k, v = linea:match("^(%a+)%s+(.-)%s*$")
    if k then c[k] = v end
  end
  return c
end

local function tabla(str)
  assert(type(str) == "string" and #str == 81, "81 dígitos")
  local t = {}
  for i = 1, 81 do t[i] = str:byte(i) - 48 end
  return t
end

-- Filas, columnas y cajas son permutaciones de 1..9 y las dadas coinciden.
local function validar(d, so)
  for k = 0, 8 do
    local vf, vc, vb = {}, {}, {}
    local r0, c0 = (k // 3) * 3, (k % 3) * 3
    for j = 0, 8 do
      local f = so[k * 9 + j + 1]
      local c = so[j * 9 + k + 1]
      local b = so[(r0 + j // 3) * 9 + c0 + j % 3 + 1]
      assert(f >= 1 and f <= 9, "dígito fuera de rango")
      assert(not vf[f], "fila " .. (k + 1) .. " repite " .. f)
      assert(not vc[c], "columna " .. (k + 1) .. " repite " .. c)
      assert(not vb[b], "caja " .. (k + 1) .. " repite " .. b)
      vf[f], vc[c], vb[b] = true, true, true
    end
  end
  local cuantas = 0
  for i = 1, 81 do
    assert(d[i] == 0 or d[i] == so[i], "la dada " .. i .. " no coincide con la solución")
    if d[i] ~= 0 then cuantas = cuantas + 1 end
  end
  return cuantas
end

local function partida()
  local c = campos()
  local d, so, p = tabla(c.dadas), tabla(c.sol), tabla(c.puesto)
  validar(d, so)
  for i = 1, 81 do assert(d[i] == 0 or p[i] == 0, "lo puesto no pisa una dada") end
  return c, d, so, p
end

local function vacias(d)
  local v = {}
  for i = 1, 81 do if d[i] == 0 then v[#v + 1] = i end end
  return v
end

local function ok(n) for _ = 1, n do assert(fake.key("ok") == true, "OK repinta") end end
local function menu(opcion)
  assert(fake.key("back") == true, "Atrás abre el menú")
  assert(dibujado("Verificar"), "el menú está en pantalla")
  local orden = { Dictar = 1, Verificar = 2, Pista = 3, Nuevo = 4, Salir = 5 }
  for _ = 2, orden[opcion] do fake.key("down") end
  return fake.key("ok")
end

local PALABRA = { "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve" }
local function dictar(texto)
  menu("Dictar")
  assert(cp.busy(), "Dictar abre el micrófono")
  fake.heard = texto
  assert(fake.step(), "se entrega lo oído")
  assert(not cp.busy())
end

-- 1. Sin partida guardada arranca en el selector de nivel; OK = Fácil.
on_open()
assert(dibujado("Elige la dificultad"), "sin partida, elige nivel")
assert(fake.key("ok") == true)
assert(dibujado("Sudoku · Fácil · 0 min jugados"), "cabezal con nivel y tiempo")
assert(dibujado("Palanca: celda · OK: número · Atrás: menú"), "pie con las teclas")
local c, d, so, p = partida()
assert(c.nivel == "facil")
assert(validar(d, so) == 38, "la fácil trae 38 dadas")
local v = vacias(d)
assert(#v == 43)
assert(dibujado("Fila " .. ((v[1] - 1) // 9 + 1) .. ", columna " .. ((v[1] - 1) % 9 + 1)),
  "el cursor arranca en la primera vacía")

-- 2. La palanca salta las dadas y da la vuelta; OK va pasando el número.
assert(fake.key("up") == true)
assert(dibujado("Fila " .. ((v[#v] - 1) // 9 + 1) .. ", columna " .. ((v[#v] - 1) % 9 + 1)),
  "arriba desde la primera vacía cae en la última")
fake.key("down")
fake.key("down")
ok(1)
c, d, so, p = partida()
assert(p[v[2]] == 1, "OK pone 1 en la segunda vacía")
ok(9)
c, d, so, p = partida()
assert(p[v[2]] == 0, "diez OK dan la vuelta a vacía")

-- 3. Verificar cuenta lo que está mal.
local mal = so[v[2]] % 9 + 1          -- distinto de la solución y nunca 0
ok(mal)
c, d, so, p = partida()
assert(p[v[2]] == mal)
assert(menu("Verificar") == true)
assert(dibujado("Hay 1 celda mal"), "verificar ve la celda mal")
ok((so[v[2]] - mal) % 10)
c, d, so, p = partida()
assert(p[v[2]] == so[v[2]])
menu("Verificar")
assert(dibujado("Todo bien hasta ahora · faltan 42"), "verificar cuenta lo que falta")

-- 4. Dictado: fila, columna y número con palabras, y con dígitos sueltos.
local celda = v[3]
local fr, fc = (celda - 1) // 9 + 1, (celda - 1) % 9 + 1
dictar("fila " .. PALABRA[fr] .. " columna " .. PALABRA[fc] .. " " .. PALABRA[so[celda]])
c, d, so, p = partida()
assert(p[celda] == so[celda], "dictado con palabras")
assert(dibujado("Puesto " .. so[celda] .. " en fila " .. fr .. ", columna " .. fc))
dictar("borra fila " .. fr .. " columna " .. fc)
c, d, so, p = partida()
assert(p[celda] == 0, "borrar por voz")
dictar(fr .. " " .. fc .. " " .. so[celda])
c, d, so, p = partida()
assert(p[celda] == so[celda], "dictado con dígitos sueltos")
dictar("en la fila " .. PALABRA[fr] .. ", columna " .. PALABRA[fc] .. ", pon un " .. PALABRA[so[celda] % 9 + 1])
c, d, so, p = partida()
assert(p[celda] == so[celda] % 9 + 1, "'pon un siete' es un siete")
-- Sólo el número: va a la celda del cursor (que el dictado dejó en `celda`).
dictar(PALABRA[so[celda]])
c, d, so, p = partida()
assert(p[celda] == so[celda], "un número suelto va al cursor")

-- Una dada no se toca, y lo que no se entiende lo dice.
local dada
for i = 1, 81 do if d[i] ~= 0 then dada = i break end end
dictar("fila " .. ((dada - 1) // 9 + 1) .. " columna " .. ((dada - 1) % 9 + 1) .. " cinco")
assert(dibujado("viene dada"), "una dada no se pisa")
dictar("hola qué tal")
assert(dibujado("No entendí: hola qué tal"), "basura = no entendí, con lo oído")
dictar("fila doce columna tres cinco")
assert(dibujado("No entendí"), "'doce' no es una cifra")
menu("Dictar")
fake.heard = nil
fake.step()
assert(dibujado("No entendí"), "escucha cancelada = no entendí")

-- 5. Pista pone el dígito correcto en el cursor.
dictar("fila " .. PALABRA[(v[4] - 1) // 9 + 1] .. " columna " .. PALABRA[(v[4] - 1) % 9 + 1])
assert(dibujado("Cursor en fila"), "sólo fila y columna mueve el cursor")
c, d, so, p = partida()
assert(p[v[4]] == 0)
menu("Pista")
c, d, so, p = partida()
assert(p[v[4]] == so[v[4]], "la pista es la solución")
assert(dibujado("Pista: " .. so[v[4]]))

-- 6. La partida entera: desde la primera vacía, cada celda con OK y abajo.
dictar("fila " .. ((v[1] - 1) // 9 + 1) .. " columna " .. ((v[1] - 1) % 9 + 1))
for k, i in ipairs(v) do
  c, d, so, p = partida()
  ok((so[i] - p[i]) % 10)
  if k < #v then assert(fake.key("down") == true) end
end
assert(dibujado("¡Resuelto!"), "se resolvió")
assert(cp.load() == "", "la partida resuelta no queda guardada")
assert(fake.key("down") == false, "resuelto, la palanca no hace nada")
assert(fake.tick() == false, "resuelto, el reloj se para")

-- 7. Guardar y continuar: una partida a medias sobrevive al reload.
assert(fake.key("ok") == true, "OK: otra partida")
assert(dibujado("Elige la dificultad"))
fake.advance(1000)
fake.key("ok")
ok(3)
local antes = campos()
fake.reload()
on_open()
assert(dibujado("Continuar"), "con partida guardada ofrece continuar")
assert(dibujado("Hay una partida fácil a medias"))
assert(fake.key("ok") == true)
local despues = campos()
assert(antes.dadas == despues.dadas and antes.sol == despues.sol and antes.puesto == despues.puesto,
  "continuar restaura el tablero")
assert(antes.puesto:find("3", 1, true), "y lo puesto sigue ahí")

-- 8. El reloj: repinta sólo cuando cambia el minuto, y persiste.
assert(fake.tick() == false)
fake.advance(30000)
assert(fake.tick() == false, "medio minuto no repinta")
fake.advance(31000)
assert(fake.tick() == true, "al minuto sí")
assert(fake.tick() == false)
assert(dibujado("· 1 min jugados"))
assert(campos().tiempo == "61", "el tiempo se guarda")

-- 9. Un guardado roto no rompe nada: partida nueva.
for _, roto in ipairs({
  "basura",
  "sudoku 1\nnivel facil\ndadas 123\nsol xx\npuesto",
  "sudoku 1\nnivel facil\ndadas " .. string.rep("0", 81) .. "\nsol " .. string.rep("1", 81) .. "\npuesto " .. string.rep("0", 81),
  "sudoku 1\nnivel marciano\ndadas " .. antes.dadas .. "\nsol " .. antes.sol .. "\npuesto " .. antes.puesto,
  "sudoku 1\nnivel facil\ndadas " .. antes.sol .. "\nsol " .. antes.dadas .. "\npuesto " .. antes.puesto,
}) do
  cp.save(roto)
  fake.reload()
  on_open()
  assert(dibujado("Elige la dificultad"), "guardado roto = partida nueva")
  fake.key("ok")
  partida()
end

-- 10. Los tres niveles, veinte derivaciones cada uno, todas válidas y distintas.
-- El selector arranca en el nivel de la partida en curso, así que se cuenta
-- desde ahí.
local DADAS = { facil = 38, medio = 31, dificil = 26 }
local actual = 1
for niv, clave in ipairs({ "facil", "medio", "dificil" }) do
  local vistos = {}
  for k = 1, 20 do
    fake.advance(7)
    assert(menu("Nuevo") == true)
    assert(dibujado("Elige la dificultad"))
    for _ = 1, (niv - actual) % 3 do fake.key("down") end
    actual = niv
    fake.key("ok")
    local cc, dd, ss = partida()
    assert(cc.nivel == clave, "nivel " .. clave)
    assert(validar(dd, ss) == DADAS[clave], clave .. ": cantidad de dadas")
    vistos[cc.dadas] = true
  end
  local distintos = 0
  for _ in pairs(vistos) do distintos = distintos + 1 end
  assert(distintos >= 15, clave .. ": las derivaciones son distintas (" .. distintos .. ")")
end

-- 11. Salir desde el menú guarda y cierra.
assert(menu("Salir") == false, "Salir cierra la app")
assert(campos().nivel == "dificil", "y deja la partida guardada")
