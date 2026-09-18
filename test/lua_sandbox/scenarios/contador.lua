-- Escenario de examples/Apps/contador.lua: lo mínimo que prueba el harness.
-- Corre DESPUÉS de cargar la app; `fake` y `cp` están a mano. Falla con
-- error() si algo no está donde tiene que estar.

local function dibujado(sub)
  for _, s in ipairs(fake.draw()) do
    if s:find(sub, 1, true) then return true end
  end
  return false
end

on_open()
assert(dibujado("0"), "arranca en cero")

assert(fake.key("up") == true, "la palanca repinta")
fake.key("up")
fake.key("up")
assert(dibujado("3"), "tres arriba = 3")

fake.key("down")
assert(dibujado("2"), "uno abajo = 2")

assert(fake.key("ok") == true)
assert(dibujado("0"), "OK vuelve a cero")

-- Atrás guarda y sale (false); al volver a abrir se acuerda.
fake.key("up")
fake.key("up")
assert(fake.key("back") == false, "Atrás sale de la app")
assert(cp.load() == "2", "guardó el número")
on_open()
assert(dibujado("2"), "al reabrir se acuerda")
