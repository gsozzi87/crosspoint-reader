-- Tres en raya contra el aparato. Tres botones alcanzan: la palanca recorre las
-- casillas libres y OK pone la ficha.
--
-- Muestra cp.selection(), que es el resalte de TODO el aparato: pestaña negra a
-- la izquierda, marco y trama sólo en los márgenes, centro en blanco. Una app
-- que invente su propio negro macizo con texto encima rompe la regla número uno
-- del sistema visual (nunca hay letras sobre trama) y además deja fantasma.

local tablero = {}     -- 1..9: "", "X" (la persona), "O" (el aparato)
local cursor = 1
local estado = "turno"  -- "turno", "ganaste", "perdiste", "empate"
local ganadas, perdidas, empates = 0, 0, 0

local LINEAS = {
  {1, 2, 3}, {4, 5, 6}, {7, 8, 9},
  {1, 4, 7}, {2, 5, 8}, {3, 6, 9},
  {1, 5, 9}, {3, 5, 7},
}

local function ganador()
  for _, l in ipairs(LINEAS) do
    local a, b, c = tablero[l[1]], tablero[l[2]], tablero[l[3]]
    if a ~= "" and a == b and b == c then return a end
  end
  return nil
end

local function lleno()
  for i = 1, 9 do if tablero[i] == "" then return false end end
  return true
end

local function primeraLibre()
  for i = 1, 9 do if tablero[i] == "" then return i end end
  return 1
end

local function guardar()
  cp.save(ganadas .. " " .. perdidas .. " " .. empates)
end

local function nueva()
  for i = 1, 9 do tablero[i] = "" end
  cursor = 1
  estado = "turno"
end

-- Busca la casilla que completa una línea para `ficha`. Sirve para ganar y,
-- con la otra ficha, para tapar.
local function completa(ficha)
  for _, l in ipairs(LINEAS) do
    local libres, propias = 0, 0
    local hueco = nil
    for _, i in ipairs(l) do
      if tablero[i] == ficha then
        propias = propias + 1
      elseif tablero[i] == "" then
        libres = libres + 1
        hueco = i
      end
    end
    if propias == 2 and libres == 1 then return hueco end
  end
  return nil
end

local function jugadaDelAparato()
  local jugada = completa("O") or completa("X")
  if jugada then return jugada end
  if tablero[5] == "" then return 5 end
  local esquinas = {}
  for _, i in ipairs({1, 3, 7, 9}) do
    if tablero[i] == "" then esquinas[#esquinas + 1] = i end
  end
  if #esquinas > 0 then return esquinas[math.random(1, #esquinas)] end
  return primeraLibre()
end

local function revisar()
  local g = ganador()
  if g == "X" then
    estado = "ganaste"
    ganadas = ganadas + 1
  elseif g == "O" then
    estado = "perdiste"
    perdidas = perdidas + 1
  elseif lleno() then
    estado = "empate"
    empates = empates + 1
  else
    return false
  end
  guardar()
  return true
end

function on_open()
  math.randomseed(cp.ms())
  local guardado = cp.load()
  if guardado then
    local g, p, e = string.match(guardado, "(%d+)%s+(%d+)%s+(%d+)")
    ganadas = tonumber(g) or 0
    perdidas = tonumber(p) or 0
    empates = tonumber(e) or 0
  end
  nueva()
end

local function mover(paso)
  -- Salta las casillas ocupadas: el cursor nunca se para donde no se puede
  -- jugar. Da la vuelta como mucho nueve veces, así que no hay bucle infinito
  -- ni con el tablero lleno.
  for _ = 1, 9 do
    cursor = cursor + paso
    if cursor > 9 then cursor = 1 elseif cursor < 1 then cursor = 9 end
    if tablero[cursor] == "" then return end
  end
end

function on_key(k)
  if estado ~= "turno" then
    if k == "ok" then
      nueva()
      cp.beep("nav")
      return true
    end
    return false
  end
  if k == "up" then
    mover(-1)
  elseif k == "down" then
    mover(1)
  elseif k == "ok" then
    if tablero[cursor] ~= "" then
      cp.beep("error")
      return true
    end
    tablero[cursor] = "X"
    cp.beep("ok")
    if not revisar() then
      tablero[jugadaDelAparato()] = "O"
      revisar()
      if estado == "turno" then cursor = primeraLibre() end
    end
  else
    return false
  end
  return true
end

local function ficha(x, y, lado, cual)
  local aire = lado // 4
  if cual == "X" then
    cp.line(x + aire, y + aire, x + lado - aire, y + lado - aire, 4)
    cp.line(x + lado - aire, y + aire, x + aire, y + lado - aire, 4)
  elseif cual == "O" then
    -- Sin círculos en el contrato: un marco grueso con las esquinas comidas se
    -- lee igual de bien a esta escala.
    local r = lado // 6
    cp.rect(x + aire, y + aire + r, lado - 2 * aire, lado - 2 * aire - 2 * r, false, 4)
    cp.rect(x + aire + r, y + aire, lado - 2 * aire - 2 * r, lado - 2 * aire, false, 4)
  end
end

function on_draw()
  local bajo = cp.height() < 600
  local tituloY = bajo and 35 or 60
  local reglaY = bajo and 70 or 100
  cp.text(40, tituloY, "Tres en raya", 14, true)
  local marcador = ganadas .. " - " .. perdidas .. " - " .. empates
  cp.text(cp.width() - 40 - cp.textw(marcador, 12), tituloY + 2, marcador, 12)
  cp.line(40, reglaY, cp.width() - 40, reglaY, 1)

  local aire = 8
  local lado = math.min(120, (cp.width() - 80 - 2 * aire) // 3,
                         (cp.height() - reglaY - (bajo and 85 or 180) - 2 * aire) // 3)
  local total = 3 * lado + 2 * aire
  local x0 = (cp.width() - total) // 2
  local y0 = reglaY + (bajo and 15 or 120)

  for i = 1, 9 do
    local col = (i - 1) % 3
    local fila = (i - 1) // 3
    local x = x0 + col * (lado + aire)
    local y = y0 + fila * (lado + aire)
    if estado == "turno" and i == cursor then
      cp.selection(x, y, lado, lado)
    else
      cp.rect(x, y, lado, lado, false, 1)
    end
    ficha(x, y, lado, tablero[i])
  end

  local pie
  if estado == "turno" then
    pie = "Palanca: mover · OK: poner tu ficha"
  else
    local texto = estado == "ganaste" and "¡Ganaste!"
      or (estado == "perdiste" and "Ganó el aparato" or "Empate")
    cp.text((cp.width() - cp.textw(texto, 14)) // 2, y0 + total + 50, texto, 14, true)
    pie = "OK: otra partida · Atrás: salir"
  end
  cp.text(40, cp.height() - 35, pie, 10)
end
