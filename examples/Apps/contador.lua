-- Contador: la palanca suma y resta, OK vuelve a cero.
-- Se acuerda del número al salir.
local n = 0

function on_open()
  n = tonumber(cp.load() or "0") or 0
end

function on_key(k)
  if k == "up" then n = n + 1
  elseif k == "down" then n = n - 1
  elseif k == "ok" then n = 0
  elseif k == "back" then
    cp.save(tostring(n))
    return false
  end
  cp.beep("nav")
  return true
end

function on_draw()
  local texto = tostring(n)
  cp.text((cp.width() - cp.textw(texto, 14)) // 2, 360, texto, 14, true)
  cp.text(40, 700, "Palanca: sumar y restar · OK: volver a cero", 10)
end
