-- Ahorcado. La palabra sale de una lista que viene adentro del archivo: el
-- aparato no tiene teclado, así que nadie puede escribirla.
--
-- Los controles son los tres de siempre: la palanca recorre el abecedario y OK
-- prueba la letra. Muestra texto, medidas de texto y dibujo con líneas.

local PALABRAS = {
  "BIBLIOTECA", "VENTANA", "CARPINTERO", "MURCIELAGO", "ESPERANZA",
  "TORMENTA", "GIRASOL", "CUCHARA", "ELEFANTE", "SENDERO",
  "CALENDARIO", "ZAPATO", "NARANJA", "HORIZONTE", "CAMPANA",
  "PALABRA", "SEMILLA", "CASTILLO", "ESTRELLA", "PUENTE",
  "CUADERNO", "MARIPOSA", "TELEFONO", "FAROLA", "DOMINGO",
  "PIMIENTA", "BALLENA", "ESCALERA", "BOSQUE", "RELAMPAGO",
}
local ABECEDARIO = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
local FALLOS_MAX = 6

local palabra = ""
local probadas = {}
local fallos = 0
local cursor = 1
local estado = "jugando"  -- "jugando", "ganado", "perdido"
local ganadas, perdidas = 0, 0

local function nueva()
  palabra = PALABRAS[math.random(1, #PALABRAS)]
  probadas = {}
  fallos = 0
  cursor = 1
  estado = "jugando"
end

local function estaEn(letra)
  return string.find(palabra, letra, 1, true) ~= nil
end

local function completa()
  for i = 1, #palabra do
    if not probadas[string.sub(palabra, i, i)] then return false end
  end
  return true
end

local function guardar()
  cp.save(ganadas .. " " .. perdidas)
end

function on_open()
  math.randomseed(cp.ms())
  local guardado = cp.load()
  if guardado then
    local g, p = string.match(guardado, "(%d+)%s+(%d+)")
    ganadas = tonumber(g) or 0
    perdidas = tonumber(p) or 0
  end
  nueva()
end

local function probar()
  local letra = string.sub(ABECEDARIO, cursor, cursor)
  if probadas[letra] then
    cp.beep("error")
    return
  end
  probadas[letra] = true
  if estaEn(letra) then
    cp.beep("ok")
    if completa() then
      estado = "ganado"
      ganadas = ganadas + 1
      guardar()
    end
  else
    fallos = fallos + 1
    cp.beep("back")
    if fallos >= FALLOS_MAX then
      estado = "perdido"
      perdidas = perdidas + 1
      guardar()
    end
  end
end

function on_key(k)
  if estado ~= "jugando" then
    if k == "ok" then
      nueva()
      cp.beep("nav")
      return true
    end
    return false
  end
  if k == "up" then
    cursor = cursor > 1 and cursor - 1 or #ABECEDARIO
  elseif k == "down" then
    cursor = cursor < #ABECEDARIO and cursor + 1 or 1
  elseif k == "ok" then
    probar()
  else
    return false
  end
  return true
end

-- El muñeco, una parte por fallo.
local function horca(x, y)
  cp.line(x, y + 200, x + 120, y + 200, 3)      -- base
  cp.line(x + 30, y + 200, x + 30, y, 3)        -- palo
  cp.line(x + 30, y, x + 130, y, 3)             -- brazo
  cp.line(x + 130, y, x + 130, y + 30, 2)       -- soga
  local cx, cy = x + 130, y + 55
  if fallos >= 1 then cp.rect(cx - 22, cy - 22, 44, 44, false, 3) end     -- cabeza
  if fallos >= 2 then cp.line(cx, cy + 22, cx, cy + 100, 3) end           -- cuerpo
  if fallos >= 3 then cp.line(cx, cy + 45, cx - 35, cy + 75, 2) end       -- brazo izq
  if fallos >= 4 then cp.line(cx, cy + 45, cx + 35, cy + 75, 2) end       -- brazo der
  if fallos >= 5 then cp.line(cx, cy + 100, cx - 30, cy + 150, 2) end     -- pierna izq
  if fallos >= 6 then cp.line(cx, cy + 100, cx + 30, cy + 150, 2) end     -- pierna der
end

function on_draw()
  cp.text(40, 60, "Ahorcado", 14, true)
  local marcador = ganadas .. " - " .. perdidas
  cp.text(cp.width() - 40 - cp.textw(marcador, 12), 62, marcador, 12)
  cp.line(40, 100, cp.width() - 40, 100, 1)

  horca(60, 140)

  -- La palabra, con guiones bajos donde falta. Cuando se pierde se muestra
  -- entera: quedarse sin saber cuál era es lo único que no se perdona.
  local mostrada = ""
  for i = 1, #palabra do
    local letra = string.sub(palabra, i, i)
    if probadas[letra] or estado == "perdido" then
      mostrada = mostrada .. letra .. " "
    else
      mostrada = mostrada .. "_ "
    end
  end
  cp.text((cp.width() - cp.textw(mostrada, 14)) // 2, 400, mostrada, 14, true)

  if estado == "jugando" then
    -- El abecedario en dos filas, con el resalte del sistema visual sobre la
    -- letra elegida y un marco fino sobre las ya probadas.
    local porFila = 13
    local celda = (cp.width() - 80) // porFila
    for i = 1, #ABECEDARIO do
      local letra = string.sub(ABECEDARIO, i, i)
      local col = (i - 1) % porFila
      local fila = (i - 1) // porFila
      local x = 40 + col * celda
      local y = 480 + fila * (celda + 8)
      if i == cursor then
        cp.selection(x, y, celda, celda)
      elseif probadas[letra] then
        cp.rect(x, y, celda, celda, false, 1)
      end
      cp.text(x + (celda - cp.textw(letra, 12)) // 2, y + (celda - cp.texth(12)) // 2, letra, 12,
              i == cursor)
    end
    cp.text(40, cp.height() - 90, "Palanca: elegir letra · OK: probarla", 10)
  else
    local texto = estado == "ganado" and "¡Ganaste!" or "Se acabó"
    cp.text((cp.width() - cp.textw(texto, 14)) // 2, 500, texto, 14, true)
    cp.text(40, cp.height() - 90, "OK: otra palabra · Atrás: salir", 10)
  end
end
