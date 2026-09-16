-- Viajes vive como app Lua. La red y el caché pasan por el puente cerrado
-- cp.travel/cp.open; la interfaz, la navegación y las pantallas están aquí.
local data, screen, selected, trip = {}, "trips", 1, nil
local parent = {}

local function reload()
  data = cp.travel() or { trips = {} }
  trip = data.trip
end

local function rows()
  if screen == "trips" then return 1 + #(data.trips or {}) end
  if screen == "home" then return 4 end
  if screen == "days" then return #(trip and trip.days or {}) end
  if screen == "items" then return #(parent.items or {}) end
  if screen == "packing" then return #(trip and trip.packing or {}) end
  if screen == "guide" then return #(data.suggest and data.suggest.lines or {}) end
  return 0
end

local function clamp()
  local n = rows()
  if n < 1 then selected = 1 elseif selected > n then selected = n elseif selected < 1 then selected = 1 end
end

local function fit(text, width, size)
  text = tostring(text or "")
  if cp.textw(text, size) <= width then return text end
  while #text > 3 and cp.textw(text .. "...", size) > width do text = text:sub(1, #text - 1) end
  return text .. "..."
end

local function title(text, sub)
  cp.text(32, 28, text, 14, true)
  if sub and sub ~= "" then cp.text(32, 58, fit(sub, cp.width() - 64, 10), 10) end
  cp.line(32, sub and 86 or 64, cp.width() - 32, sub and 86 or 64, 1)
  return sub and 98 or 76
end

local function row(y, a, b, active)
  local h = b and b ~= "" and 66 or 48
  if active then cp.selection(24, y, cp.width() - 48, h) end
  cp.text(38, y + 8, fit(a, cp.width() - 76, 12), 12, true)
  if b and b ~= "" then cp.text(38, y + 36, fit(b, cp.width() - 76, 10), 10) end
  cp.line(32, y + h - 1, cp.width() - 32, y + h - 1, 1)
  return h
end

local function draw_list(header, sub, list, mapper)
  local y = title(header, sub)
  local height = 66
  local visible = math.max(1, (cp.height() - y - 45) // height)
  local first = math.max(1, math.min(selected - visible + 1, math.max(1, #list - visible + 1)))
  for i = first, math.min(#list, first + visible - 1) do
    local a, b = mapper(list[i], i)
    row(y, a, b, i == selected)
    y = y + height
  end
end

function on_open()
  reload()
  local resume = cp.load() or ""
  cp.save("")
  screen, selected = "trips", 1
  if trip and resume == "home:" .. trip.id then screen = "home" end
  if trip and resume == "guide:" .. trip.id and data.suggest and data.suggest.tripId == trip.id then
    screen = "guide"
  end
end

function on_key(k)
  if k == "up" then selected = selected - 1; clamp(); return true end
  if k == "down" then selected = selected + 1; clamp(); return true end
  if k == "back" then
    if screen == "trips" then return false end
    if screen == "home" then screen = "trips"
    elseif screen == "days" or screen == "packing" or screen == "guide" then screen = "home"
    elseif screen == "items" then screen = "days" end
    selected = 1; return true
  end
  if k ~= "ok" then return false end
  if screen == "trips" then
    if selected == 1 then cp.open("travel_refresh"); return false end
    local picked = data.trips[selected - 1]
    if trip and trip.id == picked.id then screen, selected = "home", 1
    else cp.save("home:" .. picked.id); cp.open("travel:" .. picked.id) end
    return true
  end
  if screen == "home" then
    if selected == 1 then screen = "days"
    elseif selected == 2 then
      if data.suggest and data.suggest.tripId == trip.id and #(data.suggest.lines or {}) > 0 then screen = "guide"
      else cp.save("guide:" .. trip.id); cp.open("travel_guide:" .. trip.id); return false end
    elseif selected == 3 then screen = "packing"
    else cp.save("home:" .. trip.id); cp.open("travel:" .. trip.id); return false end
    selected = 1; clamp(); return true
  end
  if screen == "days" then
    parent = (trip.days or {})[selected] or { items = {} }
    screen, selected = "items", 1; clamp(); return true
  end
  if screen == "guide" then cp.save("guide:" .. trip.id); cp.open("travel_guide:" .. trip.id); return false end
  return false
end

function on_draw()
  cp.clear()
  if screen == "trips" then
    local list = {{name = "Actualizar viajes", place = "Sincronizar con la web"}}
    for _, v in ipairs(data.trips or {}) do list[#list + 1] = v end
    if #list == 1 then
      title("Viajes", "Guías y planes disponibles sin conexión")
      cp.text(32, 145, "No hay viajes sincronizados", 12, true)
      cp.text(32, 185, "Pulsa OK para actualizar", 10)
      cp.selection(24, 220, cp.width() - 48, 56)
      cp.text(38, 237, "Actualizar viajes", 12, true)
    else
      draw_list("Viajes", "Guías y planes disponibles sin conexión", list,
        function(v) return v.name or v.title or "Viaje", v.place or v.when or v.start or "" end)
    end
  elseif screen == "home" then
    local options = {
      {"Agenda del viaje", "Días, horarios y lugares"}, {"Guía inteligente", "Qué visitar y qué reservar"},
      {"Equipaje", "Lista de cosas para llevar"}, {"Actualizar", "Descargar los últimos cambios"}
    }
    draw_list(trip.name or "Viaje", trip.place or "", options, function(v) return v[1], v[2] end)
  elseif screen == "days" then
    draw_list(trip.name or "Viaje", "Agenda por día", trip.days or {},
      function(v) return v.label ~= "" and v.label or v.date, v.date end)
  elseif screen == "items" then
    draw_list(parent.label or parent.date or "Día", parent.date or "", parent.items or {},
      function(v) return ((v.at or "") ~= "" and (v.at .. "  ") or "") .. (v.title or "Actividad"), v.place or v.note or "" end)
  elseif screen == "packing" then
    local list = trip.packing or {}
    if #list == 0 then title("Equipaje", trip.name); cp.text(32, 130, "No hay elementos anotados", 12)
    else draw_list("Equipaje", trip.name, list, function(v) return (v.done and "✓ " or "□ ") .. (v.text or ""), "" end) end
  elseif screen == "guide" then
    local list = data.suggest.lines or {}
    draw_list("Guía inteligente", trip.name, list, function(v) return v, "" end)
    cp.text(32, cp.height() - 34, "OK: actualizar guía", 10)
  end
end
