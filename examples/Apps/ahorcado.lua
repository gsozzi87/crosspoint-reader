-- Ahorcado: adivina la palabra letra por letra antes de que se arme el muñeco.
--
-- Tres teclas y nada más: la palanca recorre el abecedario (salteando las
-- letras ya probadas), OK prueba la del cursor y Atrás sale dejando la palabra
-- a medias. No hay teclado, no hay micrófono y la app no levanta la red nunca.
--
-- LOS ACENTOS Y LA Ñ. En Lua un string es bytes y "á" son DOS, así que comparar
-- letras a byte suelto se rompe con cualquier palabra acentuada. La palabra se
-- parte en GLIFOS con utf8 (`separar`) y cada uno lleva su CLAVE, que es el
-- glifo sin tilde: la Á, la É y la Ü se adivinan con A, E y U, y la Ñ es una
-- letra propia del abecedario (la 15.ª). O sea que las palabras se guardan y se
-- muestran BIEN escritas —MONTAÑA, CIGÜEÑA, MURCIÉLAGO— y aun así no hay
-- ninguna letra que el jugador no pueda teclear: el abecedario de 27 teclas
-- alcanza siempre para ganar.
--
-- La partida se guarda con cp.save (la palabra, lo probado y el marcador) y al
-- volver se ofrece continuarla.

-- ---------------------------------------------------------------------------
-- Las palabras: sustantivos comunes en español neutro, de 4 a 12 letras, sin
-- nombres propios. La categoría se muestra como pista.

local CATEGORIAS = {
  { "Animales", "ARAÑA ARDILLA BALLENA BÚHO CABALLO CABRA CAMELLO CANGREJO CARACOL CEBRA " ..
    "CERDO CIGÜEÑA COCODRILO CONEJO DELFÍN ELEFANTE ESCORPIÓN GALLINA GATO GAVIOTA " ..
    "GORILA HORMIGA JIRAFA LAGARTIJA LEÓN LOBO LUCIÉRNAGA MARIPOSA MURCIÉLAGO OVEJA " ..
    "PALOMA PANTERA PERRO PINGÜINO PULPO RANA SERPIENTE TIBURÓN TORTUGA TUCÁN VENADO ZORRO" },
  { "Comida", "ACEITE ACEITUNA AGUACATE ALMENDRA ARROZ AZÚCAR CALABAZA CALAMAR CANELA " ..
    "CASTAÑA CEBOLLA CEREZA CHOCOLATE DURAZNO ENSALADA ESPINACA FRESA FRIJOL GALLETA " ..
    "GARBANZO HARINA HELADO JAMÓN LECHUGA LENTEJA LIMÓN MANDARINA MANTEQUILLA MANZANA " ..
    "MELÓN NARANJA NUEZ PASTEL PEPINO PIMIENTA PLÁTANO QUESO SANDÍA SOPA TOMATE " ..
    "TORTILLA VAINILLA YOGUR ZANAHORIA" },
  { "Casa", "ALACENA ALFOMBRA ALMOHADA ARMARIO BALCÓN BAÑERA BOMBILLA CAJÓN CAMA CAMPANA " ..
    "CANASTA CEPILLO CERRADURA COCINA COLCHÓN CORTINA CUCHARA CUCHILLO ESCALERA ESCOBA " ..
    "ESPEJO ESTANTE JABÓN JARRÓN LÁMPARA LAVADORA LLAVE MANTEL MARTILLO MESA PERCHA " ..
    "PLANCHA PUERTA RELOJ SÁBANA SARTÉN SILLA SOFÁ TENEDOR TIJERAS VENTANA VENTILADOR" },
  { "Naturaleza", "ARCOÍRIS ARENA ÁRBOL BOSQUE CABAÑA CAMINO CASCADA CIELO DESIERTO " ..
    "ESTRELLA GIRASOL GRANIZO HOJA ISLA LAGUNA LLUVIA LUNA MONTAÑA NEBLINA NIEVE NUBE " ..
    "OCÉANO OTOÑO PANTANO PLANETA PLAYA PRADERA RAÍZ RELÁMPAGO ROCÍO SELVA SEMILLA " ..
    "SOMBRA TIERRA TORMENTA VIENTO VIÑEDO VOLCÁN" },
  { "Cuerpo", "BARBILLA BRAZO CABELLO CABEZA CEJA CODO CORAZÓN DEDO DIENTE ESPALDA " ..
    "GARGANTA HOMBRO HUESO LENGUA MEJILLA MUÑECA NARIZ OREJA PESTAÑA PULMÓN RODILLA TOBILLO" },
  { "Ropa", "ABRIGO BLUSA BOTA BUFANDA CALCETÍN CAMISA CHALECO CINTURÓN CORBATA FALDA " ..
    "GORRA GUANTE PANTALÓN PAÑUELO PIJAMA SANDALIA SOMBRERO SUÉTER VESTIDO ZAPATO" },
  { "Oficios", "ALBAÑIL BOMBERO CARPINTERO CARTERO CIENTÍFICO COCINERO DENTISTA " ..
    "ELECTRICISTA ENFERMERA FOTÓGRAFO JARDINERO LEÑADOR MAESTRO MECÁNICO MÉDICO " ..
    "PANADERO PELUQUERO PESCADOR PILOTO PINTOR VETERINARIO ZAPATERO" },
  { "Viaje", "AEROPUERTO AUTOBÚS AVENIDA AVIÓN BARCO BICICLETA BOLETO BRÚJULA CAMIÓN " ..
    "CARRETERA CRUCERO EQUIPAJE ESTACIÓN FRONTERA HOTEL MALETA MERCADO MUSEO PASAPORTE " ..
    "PLAZA PUENTE SEMÁFORO SENDERO TREN" },
  { "Escuela", "BIBLIOTECA BORRADOR CALENDARIO CARPETA CUADERNO CUENTO DICCIONARIO DISEÑO " ..
    "ESCRITORIO ESTUDIANTE EXAMEN HISTORIA LÁPIZ LIBRETA LIBRO MOCHILA NÚMERO PALABRA " ..
    "PIZARRÓN PREGUNTA PROBLEMA REGLA RESPUESTA TAREA" },
  { "Música", "ACORDEÓN ARMÓNICA BATERÍA CANCIÓN CONCIERTO CORO DIBUJO ESCULTURA FLAUTA " ..
    "GUITARRA MELODÍA ORQUESTA PIANO PINCEL SINFONÍA TAMBOR TEATRO TROMPETA VIOLÍN" },
  { "Deporte", "AJEDREZ BALÓN CAMPEONATO CANCHA CARRERA CICLISMO DOMINÓ ENTRENADOR EQUIPO " ..
    "GIMNASIA MEDALLA NATACIÓN PARTIDO PATINETA PELOTA PORTERO RAQUETA TORNEO TROFEO" },
}

-- El abecedario español, con la Ñ en su lugar: 27 teclas en tres filas de nueve.
local ABC = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "Ñ",
              "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" }
local SIN_TILDE = { ["Á"] = "A", ["É"] = "E", ["Í"] = "I", ["Ó"] = "O", ["Ú"] = "U", ["Ü"] = "U" }
local ES_LETRA = {}
for _, l in ipairs(ABC) do ES_LETRA[l] = true end

local FALLOS_MAX = 6

-- ---------------------------------------------------------------------------
-- Estado

local PALABRAS, CATS = {}, {}      -- la lista aplanada, una sola vez
local pantalla = "juego"           -- "inicio" o "juego"
local glifos, claves, esDe = {}, {}, {}
local categoria = ""
local probadas = {}                -- [clave] = true
local fallos = 0
local cursor = 1
local estado = "jugando"           -- "jugando", "ganado", "perdido"
local ganadas, perdidas = 0, 0
local hayGuardada = false
local inicioCursor = 1

-- Guardas para lo que viene de afuera (cp.load, cp.time)
local function s(v)
  if type(v) == "string" then return v end
  if type(v) == "number" then return tostring(v) end
  return ""
end

local function n(v, def)
  local x = tonumber(v)
  if x == nil then return def or 0 end
  return math.floor(x)
end

-- ---------------------------------------------------------------------------
-- La palabra

-- Parte una palabra en glifos y claves. Devuelve nil si no es UTF-8 válido, si
-- no mide entre 3 y 14 letras o si trae algo que no es una letra del
-- abecedario: lo que sale de cp.load no se toca sin revisarlo.
local function separar(pal)
  if type(pal) ~= "string" then return nil end
  local largo = utf8.len(pal)
  if largo == nil or largo < 3 or largo > 14 then return nil end
  local g, c = {}, {}
  for _, code in utf8.codes(pal) do
    local ch = utf8.char(code)
    local base = SIN_TILDE[ch] or ch
    if not ES_LETRA[base] then return nil end
    g[#g + 1] = ch
    c[#c + 1] = base
  end
  return g, c
end

local function ponerPalabra(g, c, cat)
  glifos, claves, categoria = g, c, s(cat)
  esDe = {}
  for _, k in ipairs(c) do esDe[k] = true end
end

local function completa()
  for _, k in ipairs(claves) do
    if not probadas[k] then return false end
  end
  return true
end

local function contarFallos()
  local f = 0
  for _, l in ipairs(ABC) do
    if probadas[l] and not esDe[l] then f = f + 1 end
  end
  return f
end

local function primeraLibre()
  for i = 1, #ABC do
    if not probadas[ABC[i]] then return i end
  end
  return 1
end

-- ---------------------------------------------------------------------------
-- Guardar y cargar

local function guardar()
  if estado ~= "jugando" then
    cp.save("ahorcado 1\nmarcador " .. ganadas .. " " .. perdidas)
    hayGuardada = false
    return
  end
  local probs = {}
  for _, l in ipairs(ABC) do
    if probadas[l] then probs[#probs + 1] = l end
  end
  cp.save(table.concat({
    "ahorcado 1",
    "marcador " .. ganadas .. " " .. perdidas,
    "categoria " .. categoria,
    "palabra " .. table.concat(glifos),
    "probadas " .. table.concat(probs),
  }, "\n"))
  hayGuardada = true
end

-- Lee lo guardado. El marcador se toma siempre que se entienda; devuelve true
-- sólo si además quedó una partida en pie. Cualquier cosa rara es "no hay
-- partida", nunca un error.
local function cargar()
  local texto = cp.load()
  if type(texto) ~= "string" or texto == "" then return false end
  local campos = {}
  for linea in texto:gmatch("[^\n]+") do
    local k, v = linea:match("^(%a+)%s+(.-)%s*$")
    if k then campos[k] = v end
  end
  if campos.ahorcado ~= "1" then return false end
  local g, p = s(campos.marcador):match("(%d+)%s+(%d+)")
  ganadas, perdidas = n(g, 0), n(p, 0)
  local gl, cl = separar(s(campos.palabra))
  if gl == nil then return false end
  local prueba = s(campos.probadas)
  if utf8.len(prueba) == nil then return false end
  local probs = {}
  for _, code in utf8.codes(prueba) do
    local ch = utf8.char(code)
    if not ES_LETRA[ch] then return false end
    probs[ch] = true
  end
  ponerPalabra(gl, cl, campos.categoria)
  probadas = probs
  fallos = contarFallos()
  -- Una partida ya terminada no se continúa: se empieza otra.
  if fallos >= FALLOS_MAX or completa() then return false end
  estado = "jugando"
  cursor = primeraLibre()
  return true
end

local function nueva()
  local i = math.random(1, #PALABRAS)
  local g, c = separar(PALABRAS[i])
  local vueltas = 0
  while g == nil and vueltas < #PALABRAS do
    i = i % #PALABRAS + 1
    g, c = separar(PALABRAS[i])
    vueltas = vueltas + 1
  end
  if g == nil then
    -- La lista es nuestra y está probada; esto es el suelo, por si alguna vez
    -- entra una palabra rara: mejor jugar con otra que morirse acá.
    g, c = separar("PALABRA")
    i = 0
  end
  ponerPalabra(g, c, i > 0 and CATS[i] or "Escuela")
  probadas = {}
  fallos = 0
  estado = "jugando"
  cursor = 1
  pantalla = "juego"
  guardar()
end

-- ---------------------------------------------------------------------------
-- Teclas

function on_open()
  math.randomseed(cp.ms())
  local t = cp.time()
  if t then math.randomseed(cp.ms() + n(t.epoch, 0)) end
  if #PALABRAS == 0 then
    for _, cat in ipairs(CATEGORIAS) do
      for pal in cat[2]:gmatch("%S+") do
        PALABRAS[#PALABRAS + 1] = pal
        CATS[#PALABRAS] = cat[1]
      end
    end
  end
  hayGuardada = cargar()
  if hayGuardada then
    pantalla = "inicio"
    inicioCursor = 1
  else
    nueva()
  end
end

-- La palanca saltea lo ya probado: si no, media partida se va en pasar por
-- encima de letras que ya no hacen nada.
local function mover(paso)
  for _ = 1, #ABC do
    cursor = cursor + paso
    if cursor > #ABC then cursor = 1 elseif cursor < 1 then cursor = #ABC end
    if not probadas[ABC[cursor]] then return end
  end
end

local function probar()
  local letra = ABC[cursor]
  if probadas[letra] then
    cp.beep("error")
    return
  end
  probadas[letra] = true
  if esDe[letra] then
    if completa() then
      estado = "ganado"
      ganadas = ganadas + 1
      cp.beep("ok")
    else
      cp.beep("ok")
    end
  else
    fallos = fallos + 1
    if fallos >= FALLOS_MAX then
      estado = "perdido"
      perdidas = perdidas + 1
      cp.beep("error")
    else
      cp.beep("back")
    end
  end
  if estado == "jugando" then mover(1) end
  guardar()
end

local function teclaInicio(k)
  if k == "up" then
    inicioCursor = inicioCursor > 1 and inicioCursor - 1 or 3
  elseif k == "down" then
    inicioCursor = inicioCursor < 3 and inicioCursor + 1 or 1
  elseif k == "ok" then
    if inicioCursor == 1 then
      pantalla = "juego"
    elseif inicioCursor == 2 then
      nueva()
    else
      cp.quit()
      return false
    end
  else
    return false
  end
  cp.beep("nav")
  return true
end

local function teclaJuego(k)
  if estado ~= "jugando" then
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
    probar()
    return true
  else
    guardar()
    return false
  end
  cp.beep("nav")
  return true
end

function on_key(k)
  if pantalla == "inicio" then return teclaInicio(k) end
  return teclaJuego(k)
end

-- ---------------------------------------------------------------------------
-- Dibujo. Margen de 24, grilla de 8, cabezal UI_14 con filete: el sistema
-- visual de docs/ws397/DISENO.md.

local REGLA_Y = 76
local HORCA_Y = 128        -- la viga
local SUELO_Y = 424
local PALABRA_Y = 436
local ERRADAS_Y = 496
local ABC_Y = 552
local CELDA = 48

-- La cabeza es un polígono de doce lados con los desplazamientos ya redondeados
-- a entero: un cuadrado se lee como una caja y cp.line no acepta decimales.
local CABEZA = {
  { 30, 0 }, { 26, 15 }, { 15, 26 }, { 0, 30 }, { -15, 26 }, { -26, 15 },
  { -30, 0 }, { -26, -15 }, { -15, -26 }, { 0, -30 }, { 15, -26 }, { 26, -15 },
}

local function centrado(texto, y, tam, negrita)
  cp.text((cp.width() - cp.textw(texto, tam)) // 2, y, texto, tam, negrita)
end

local function lista(y, items, cur, alto)
  for i, item in ipairs(items) do
    local fy = y + (i - 1) * alto
    if i == cur then cp.selection(24, fy, cp.width() - 48, alto) end
    cp.text(48, fy + (alto - cp.texth(12)) // 2, item, 12, i == cur)
  end
end

-- La horca y el muñeco, una parte por fallo. Todo con líneas gruesas: en tinta
-- a 480x800 una línea de 1 px al lado de un texto de 14 se pierde.
local function dibujarHorca()
  local poste = cp.width() // 2 - 90
  local soga = cp.width() // 2 + 80
  cp.line(poste - 40, SUELO_Y, soga + 30, SUELO_Y, 6)     -- piso
  cp.line(poste, SUELO_Y, poste, HORCA_Y, 6)              -- palo
  cp.line(poste, HORCA_Y, soga, HORCA_Y, 6)               -- viga
  cp.line(poste, HORCA_Y + 40, poste + 40, HORCA_Y, 4)    -- tirante
  cp.line(soga, HORCA_Y, soga, HORCA_Y + 40, 4)           -- soga
  local cy = HORCA_Y + 70                                 -- centro de la cabeza
  if fallos >= 1 then
    for i = 1, #CABEZA do
      local a, b = CABEZA[i], CABEZA[i % #CABEZA + 1]
      cp.line(soga + a[1], cy + a[2], soga + b[1], cy + b[2], 4)
    end
  end
  if fallos >= 2 then cp.line(soga, cy + 30, soga, cy + 140, 6) end
  if fallos >= 3 then cp.line(soga, cy + 54, soga - 46, cy + 102, 5) end
  if fallos >= 4 then cp.line(soga, cy + 54, soga + 46, cy + 102, 5) end
  if fallos >= 5 then cp.line(soga, cy + 140, soga - 42, cy + 200, 5) end
  if fallos >= 6 then cp.line(soga, cy + 140, soga + 42, cy + 200, 5) end
  if estado == "perdido" then
    -- Los ojos en cruz: es lo que distingue de un vistazo el muñeco terminado.
    for _, dx in ipairs({ -12, 12 }) do
      cp.line(soga + dx - 6, cy - 12, soga + dx + 6, cy, 3)
      cp.line(soga + dx - 6, cy, soga + dx + 6, cy - 12, 3)
    end
    cp.line(soga - 10, cy + 16, soga + 10, cy + 16, 3)
  end
end

-- La palabra, una letra por casilla con su raya debajo. Las casillas separadas
-- se leen mucho mejor que un "_ _ A _" corrido, y de paso la raya dice cuántas
-- letras faltan sin contar guiones.
local function dibujarPalabra(y)
  local cuantas = #glifos
  if cuantas == 0 then return end
  local paso = math.min(40, (cp.width() - 48) // cuantas)
  local x0 = (cp.width() - paso * cuantas) // 2
  for i = 1, cuantas do
    local x = x0 + (i - 1) * paso
    cp.rect(x + 4, y + 38, paso - 8, 4, true)
    if probadas[claves[i]] or estado == "perdido" then
      local g = glifos[i]
      cp.text(x + (paso - cp.textw(g, 14)) // 2, y, g, 14, true)
    end
  end
end

local function dibujarAbecedario()
  for i = 1, #ABC do
    local letra = ABC[i]
    local x = 24 + ((i - 1) % 9) * CELDA
    local y = ABC_Y + ((i - 1) // 9) * CELDA
    if i == cursor then cp.selection(x, y, CELDA, CELDA) end
    cp.text(x + (CELDA - cp.textw(letra, 12)) // 2, y + (CELDA - cp.texth(12)) // 2,
            letra, 12, i == cursor)
    if probadas[letra] then
      if esDe[letra] then
        cp.rect(x + 10, y + CELDA - 11, CELDA - 20, 3, true)   -- acertada: subrayada
      else
        cp.line(x + 8, y + CELDA // 2, x + CELDA - 8, y + CELDA // 2, 3)  -- errada: tachada
      end
    end
  end
end

function on_draw()
  local alto = cp.height()
  cp.text(24, 40, "Ahorcado", 14, true)
  local marcador = ganadas .. " - " .. perdidas
  cp.text(cp.width() - 24 - cp.textw(marcador, 12), 44, marcador, 12)
  cp.line(24, REGLA_Y, cp.width() - 24, REGLA_Y, 1)

  if pantalla == "inicio" then
    cp.text(24, 96, "Hay una palabra a medias", 12)
    cp.text(24, 128, categoria .. " · " .. #glifos .. " letras · " .. fallos .. " de "
            .. FALLOS_MAX .. " fallos", 10)
    dibujarPalabra(176)
    lista(280, { "Continuar", "Palabra nueva", "Salir" }, inicioCursor, 48)
    cp.text(24, alto - 40, "Palanca: elegir · OK: entrar · Atrás: salir", 10)
    return
  end

  cp.text(24, 88, categoria .. " · " .. #glifos .. " letras", 10)
  dibujarHorca()
  dibujarPalabra(PALABRA_Y)

  local erradas = {}
  for _, l in ipairs(ABC) do
    if probadas[l] and not esDe[l] then erradas[#erradas + 1] = l end
  end
  if #erradas > 0 then
    cp.text(24, ERRADAS_Y, "Erradas: " .. table.concat(erradas, " "), 12)
  elseif estado == "jugando" then
    cp.text(24, ERRADAS_Y, "Sin fallos todavía", 12)
  end
  if estado == "jugando" then
    local quedan = "Quedan " .. (FALLOS_MAX - fallos)
    cp.text(cp.width() - 24 - cp.textw(quedan, 12), ERRADAS_Y, quedan, 12)
    dibujarAbecedario()
    -- El pie nombra la letra del cursor: el resalte se ve, pero a un metro y
    -- con el panel lavado después de diez parciales, leerla despeja la duda.
    cp.text(24, alto - 40, "Palanca: letra · OK: probar la " .. ABC[cursor] .. " · Atrás: salir", 10)
    return
  end

  centrado(estado == "ganado" and "¡Ganaste!" or "Se acabó", ABC_Y + 16, 14, true)
  centrado(estado == "ganado" and (categoria .. " · " .. #glifos .. " letras")
           or ("Era " .. table.concat(glifos)), ABC_Y + 62, 12)
  cp.text(24, alto - 40, "OK: otra palabra · Atrás: salir", 10)
end
