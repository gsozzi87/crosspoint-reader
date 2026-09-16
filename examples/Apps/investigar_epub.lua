-- Portada Lua; la investigación, red y escritura quedan en el servicio
-- restringido del firmware para que una app no obtenga acceso general.
function on_key(k)
  if k == "ok" then cp.open("research_epub"); return false end
  return false
end

function on_draw()
  cp.clear()
  cp.text(32, 28, "Investigar y crear EPUB", 14, true)
  cp.line(32, 64, cp.width() - 32, 64, 1)
  cp.text(32, 115, "Convierte un tema en un libro breve", 12, true)
  cp.text(32, 153, "La investigación usa fuentes actuales", 10)
  cp.text(32, 180, "y guarda el resultado en tu biblioteca.", 10)
  cp.selection(24, 235, cp.width() - 48, 68)
  cp.text(38, 251, "Dictar un tema", 12, true)
  cp.text(38, 279, "Pulsa OK y habla", 10)
  cp.text(32, cp.height() - 42, "Atrás: salir", 10)
end
