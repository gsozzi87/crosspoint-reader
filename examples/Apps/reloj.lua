-- Reloj: la hora grande, la fecha debajo. Muestra cp.time().
--
-- Lo importante de esta app no es el reloj, es CUÁNDO repinta: on_tick corre
-- diez veces por segundo, pero devolver true diez veces por segundo en una
-- pantalla de tinta sería un refresco parcial cada 120 ms, o sea un refresco
-- completo cada segundo y medio. Se repinta cuando cambia el minuto, y nada
-- más. Cualquier app que muestre algo que cambia solo tiene que hacer esto.

local ultimoMinuto = -1
local hora = nil

local MESES = {
  "enero", "febrero", "marzo", "abril", "mayo", "junio",
  "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre",
}
local DIAS = {"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"}

-- Los siete segmentos de cada dígito, en el orden a b c d e f g.
local SEGMENTOS = {
  [0] = {1, 1, 1, 1, 1, 1, 0},
  [1] = {0, 1, 1, 0, 0, 0, 0},
  [2] = {1, 1, 0, 1, 1, 0, 1},
  [3] = {1, 1, 1, 1, 0, 0, 1},
  [4] = {0, 1, 1, 0, 0, 1, 1},
  [5] = {1, 0, 1, 1, 0, 1, 1},
  [6] = {1, 0, 1, 1, 1, 1, 1},
  [7] = {1, 1, 1, 0, 0, 0, 0},
  [8] = {1, 1, 1, 1, 1, 1, 1},
  [9] = {1, 1, 1, 1, 0, 1, 1},
}

local function digito(x, y, w, h, valor, grosor)
  local s = SEGMENTOS[valor]
  if not s then return end
  local g = grosor
  local medio = y + h // 2
  if s[1] == 1 then cp.rect(x, y, w, g, true) end                       -- a, arriba
  if s[2] == 1 then cp.rect(x + w - g, y, g, h // 2, true) end          -- b, derecha arriba
  if s[3] == 1 then cp.rect(x + w - g, medio, g, h // 2, true) end      -- c, derecha abajo
  if s[4] == 1 then cp.rect(x, y + h - g, w, g, true) end               -- d, abajo
  if s[5] == 1 then cp.rect(x, medio, g, h // 2, true) end              -- e, izquierda abajo
  if s[6] == 1 then cp.rect(x, y, g, h // 2, true) end                  -- f, izquierda arriba
  if s[7] == 1 then cp.rect(x, medio - g // 2, w, g, true) end          -- g, medio
end

function on_open()
  hora = cp.time()
  ultimoMinuto = hora and hora.min or -1
end

function on_tick()
  local ahora = cp.time()
  if not ahora then return false end
  if ahora.min == ultimoMinuto then return false end
  ultimoMinuto = ahora.min
  hora = ahora
  return true
end

function on_key(k)
  if k == "ok" then
    -- Por si alguien quiere ver la hora exacta sin esperar al minuto.
    hora = cp.time()
    cp.beep("nav")
    return true
  end
  return false
end

function on_draw()
  if not hora then
    local aviso = "El aparato todavía no está en hora"
    cp.text((cp.width() - cp.textw(aviso, 14)) // 2, 360, aviso, 14, true)
    local como = "Sincroniza con tu cuenta para ponerlo en hora"
    cp.text((cp.width() - cp.textw(como, 10)) // 2, 400, como, 10)
    return
  end

  local w, h = 74, 130
  local g = 12
  local aire, dosPuntos = 18, 30
  local total = 4 * w + 3 * aire + dosPuntos
  local x = (cp.width() - total) // 2
  local y = 250

  digito(x, y, w, h, hora.hour // 10, g)
  x = x + w + aire
  digito(x, y, w, h, hora.hour % 10, g)
  x = x + w + aire

  local cx = x + dosPuntos // 2 - g // 2
  cp.rect(cx, y + h // 3 - g // 2, g, g, true)
  cp.rect(cx, y + 2 * h // 3 - g // 2, g, g, true)
  x = x + dosPuntos

  digito(x, y, w, h, hora.min // 10, g)
  x = x + w + aire
  digito(x, y, w, h, hora.min % 10, g)

  local fecha = DIAS[hora.wday] .. " " .. hora.day .. " de " .. MESES[hora.month]
  cp.text((cp.width() - cp.textw(fecha, 14)) // 2, y + h + 60, fecha, 14)
  local anio = tostring(hora.year)
  cp.text((cp.width() - cp.textw(anio, 12)) // 2, y + h + 100, anio, 12)

  cp.text(40, cp.height() - 90, "Se repinta al cambiar el minuto · OK: ahora mismo", 10)
end
