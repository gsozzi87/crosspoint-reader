-- Dados: sacude el aparato (o aprieta OK) y tira. La cantidad se elige con la
-- palanca. Muestra de qué sirve cp.motion().
local cuantos = 2
local tirada = {}

local function tirar()
  tirada = {}
  for i = 1, cuantos do tirada[i] = math.random(1, 6) end
  cp.beep("ok")
end

function on_open()
  math.randomseed(cp.ms())
  tirar()
end

function on_key(k)
  if k == "up" and cuantos < 6 then cuantos = cuantos + 1
  elseif k == "down" and cuantos > 1 then cuantos = cuantos - 1
  elseif k == "ok" then tirar(); return true
  elseif k == "back" then return false
  else return false end
  tirar()
  return true
end

function on_tick()
  -- Sacudirlo tira de nuevo: es el gesto que uno hace con un cubilete.
  if cp.motion() == "Shake" then
    tirar()
    return true
  end
  return false
end

-- Un dado dibujado: marco y los puntos donde van.
local PUNTOS = {
  {5}, {1, 9}, {1, 5, 9}, {1, 3, 7, 9}, {1, 3, 5, 7, 9}, {1, 3, 4, 6, 7, 9},
}

local function dado(x, y, lado, valor)
  cp.rect(x, y, lado, lado, false, 3)
  local paso = lado // 4
  local r = lado // 12
  for _, celda in ipairs(PUNTOS[valor]) do
    local col = (celda - 1) % 3
    local fila = (celda - 1) // 3
    local cx = x + paso + col * paso
    local cy = y + paso + fila * paso
    cp.rect(cx - r, cy - r, r * 2, r * 2, true)
  end
end

function on_draw()
  cp.text(40, 60, "Dados", 14, true)
  cp.line(40, 100, cp.width() - 40, 100, 1)

  local lado, aire = 120, 24
  local porFila = 3
  local total = 0
  for _ in ipairs(tirada) do total = total + 1 end
  local anchoFila = math.min(total, porFila) * (lado + aire) - aire
  local x0 = (cp.width() - anchoFila) // 2

  for i, valor in ipairs(tirada) do
    local col = (i - 1) % porFila
    local fila = (i - 1) // porFila
    dado(x0 + col * (lado + aire), 180 + fila * (lado + aire), lado, valor)
  end

  local suma = 0
  for _, v in ipairs(tirada) do suma = suma + v end
  local texto = "Suma: " .. suma
  cp.text((cp.width() - cp.textw(texto, 14)) // 2, 560, texto, 14, true)
  cp.text(40, 690, "Palanca: cuántos dados (" .. cuantos .. ")", 10)
  cp.text(40, 715, "OK o sacudir: tirar de nuevo", 10)
end
