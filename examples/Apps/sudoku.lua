-- Sudoku: nueve por nueve, con la palanca o dictando.
--
-- Cuatro teclas y el micrófono, nada más. La palanca recorre las celdas
-- VACÍAS en orden de lectura (las dadas se saltan), OK va pasando el número de
-- la celda (1, 2 … 9, vacía) y Atrás abre el menú: dictar ("fila tres, columna
-- cinco, siete"), verificar, pista, partida nueva, salir.
--
-- Los tableros no se generan en el aparato: comprobar que un sudoku tiene una
-- sola solución cuesta más de lo que el cajón deja por llamada (400.000
-- instrucciones). Vienen 36 tableros base con su solución, verificados de
-- escritorio, y de cada uno salen miles distintos por simetría: se cambian los
-- nombres de los dígitos, se barajan filas dentro de cada banda y columnas
-- dentro de cada pila, se barajan las bandas y las pilas, y a veces se
-- traspone. Todo eso conserva la unicidad de la solución.
--
-- La partida se guarda con cp.save (tablero derivado, lo puesto, el tiempo) y
-- al abrir se ofrece continuarla.

local FACIL = {
  { "135002089897050034246390700500401002029000000080029003054210006000005020062000010",
    "135742689897156234246398751573481962629537148481629573754213896318965427962874315" },  -- 38 dadas
  { "493001000760000008850000907200504793549300100010289045000400800604008021008002300",
    "493871256761925438852643917286514793549367182317289645125436879634798521978152364" },  -- 38 dadas
  { "270005000304060002516902400102400785043150000795000010900600800000378600630509000",
    "279845163384761592516932478162493785843157926795286314957624831421378659638519247" },  -- 38 dadas
  { "048006359000750000520940080630002000050091200000000038209865040416007090085009027",
    "748126359963758412521943786634582971857391264192674538279865143416237895385419627" },  -- 38 dadas
  { "074021008001300002050084971290000005000207804040159006820010060000790053500400107",
    "674921538981375642352684971293846715165237894748159326827513469416798253539462187" },  -- 38 dadas
  { "305020700461000290972400801708000340000034002000000009823940600510076020040010905",
    "385129764461587293972463851758291346196734582234658179823945617519376428647812935" },  -- 38 dadas
  { "052007406700900008400056092067409100045031200001070800100793000000060000673245001",
    "952817436736924518418356792267489153845631279391572864184793625529168347673245981" },  -- 38 dadas
  { "612090078370000450008736100000500800200041760700000029800159600060020005090687010",
    "612495378379218456458736192936572841285941763741863529824159637167324985593687214" },  -- 38 dadas
  { "000705000380600100762410980009300700000001804017059003008030009041508306250007408",
    "194785632385692147762413985829346751536271894417859263678134529941528376253967418" },  -- 38 dadas
  { "005900000620078005001504960090217350270450091053006400000630200080049006060080010",
    "745961823629378145831524967496217358278453691153896472917635284582149736364782519" },  -- 38 dadas
  { "760450390000008004140000007506023000081090200009147560000304900650082000302005846",
    "768451392923678154145239687576823419481596273239147568817364925654982731392715846" },  -- 38 dadas
  { "800900004500004108000010030007000810900185020280430095690042001342800000008369502",
    "816973254539624178724518936457296813963185427281437695695742381342851769178369542" },  -- 38 dadas
}
local MEDIO = {
  { "007504000050700060048036005060045001014003500000201700070050403320000000005007009",
    "697524138153798264248136975762945381814673592539281746976852413321469857485317629" },  -- 31 dadas
  { "007600008000000036680007052300000001701902800002003904003071040000290000170050080",
    "437625198215489736689137452394768521751942863862513974923871645548296317176354289" },  -- 31 dadas
  { "060740000000356100407000000095400000723061000614900030009080050040000801000603700",
    "561749328982356174437812965895437216723561489614928537279184653346275891158693742" },  -- 31 dadas
  { "005108000302004087080070009063800000000000090000400800520090764107546000630000005",
    "975138642312964587486275139763829451841657293259413876528391764197546328634782915" },  -- 31 dadas
  { "102407000800260490000100003000008720300600004270040000500009186000310000701806000",
    "132497865857263491964185273416538729389672514275941638543729186628314957791856342" },  -- 31 dadas
  { "045879020060004070008306040350090800001003200006780000004000310009007080800060000",
    "145879623263514978798326541357192864981643257426785139674958312519237486832461795" },  -- 31 dadas
  { "000600700240790001600000450008000010006410327021063000850370090964000000010000000",
    "185624739243795681679831452438257916596418327721963845852376194964182573317549268" },  -- 31 dadas
  { "070090300360458000580000090035209418004000700790600200000502000050010000000830070",
    "472196385369458127581723694635279418124385769798641253847562931253917846916834572" },  -- 31 dadas
  { "000809200007000689008074030600008500000000701014090063026000000850407920300000010",
    "135869274247153689968274135693718542582346791714592863426981357851437926379625418" },  -- 31 dadas
  { "000510000600407100150090000000040983709081045008000017000070000506004000907200801",
    "872513496693427158154698372215746983769381245438952617321879564586134729947265831" },  -- 31 dadas
  { "000090020087000000004306001000009004519030060038002970600751400000620007070040100",
    "156894723387215649924376851762589314519437268438162975693751482841623597275948136" },  -- 31 dadas
  { "600000203024000000718302900000204307000000000000960040800700016140030090907106008",
    "695481273324697851718352964569214387481573629273968145832749516146835792957126438" },  -- 31 dadas
}
local DIFICIL = {
  { "002047309000005001000006000000060045130900020060051000090000004000002506520000080",
    "852147369946325871371896452289763145135984627467251938693578214718432596524619783" },  -- 26 dadas
  { "000200009200080000300609000010060000000043007607108040000000500002900703500024800",
    "761235489295487136384619275413762958928543617657198342139876524842951763576324891" },  -- 26 dadas
  { "004060701003400000007001003000082030000000820020003007040076000190040005700000040",
    "254369781813457962967821453679582134531794826428613597345976218196248375782135649" },  -- 26 dadas
  { "007016500009850000000000400000605190018307602000200000060000274030000050000900000",
    "287416539649853721153729468324685197518397642796241385961538274832174956475962813" },  -- 26 dadas
  { "000206800090100050006000210004007108002000000380000047009504300000000000153070000",
    "415296873297183654836745219564937128972418536381652947729564381648321795153879462" },  -- 26 dadas
  { "308600000010050080500400000400200007000000010807590620000000842080002000003004006",
    "348629571612357489579418263461283957925746318837591624796135842184962735253874196" },  -- 26 dadas
  { "100040300000000107000920000600070090010006400704010602056000700030000004000034008",
    "168745329592368147347921586625473891913286475784519632456892713839157264271634958" },  -- 26 dadas
  { "006040000720000000005002016130080090807000000904500800201000009000950300000104000",
    "316849257729615483485732916132486795857293164964571832271368549648957321593124678" },  -- 26 dadas
  { "000000000508000609390000000060304902000280010000500400000407160001020000000069034",
    "726948351518732649394615827165374982479286513283591476932457168641823795857169234" },  -- 26 dadas
  { "085400003100029070030078040070002600000864030000000009209000400000700908000000000",
    "785416293164329875932578146378952614591864732426137589219683457653741928847295361" },  -- 26 dadas
  { "813047900009300106000000003000000000390206080080705000600000020504000090030000600",
    "813647952249358176765129843152483769397216485486795231671934528524861397938572614" },  -- 26 dadas
  { "702050004008020070005070300000600007010705040000000280030000400000190030500030001",
    "792853614368421579145976328983642157216785943457319286831567492624198735579234861" },  -- 26 dadas
}

local NIVELES = {
  { clave = "facil",   nombre = "Fácil",   tabla = FACIL },
  { clave = "medio",   nombre = "Medio",   tabla = MEDIO },
  { clave = "dificil", nombre = "Difícil", tabla = DIFICIL },
}

-- ---------------------------------------------------------------------------
-- Estado

local pantalla = "nivel"   -- "inicio", "nivel", "juego", "menu"
local nivel = 1            -- índice en NIVELES
local base = 1             -- índice del tablero base dentro del nivel
local semilla = 1
local dadas = {}           -- 1..81, 0 = vacía
local sol = {}             -- 1..81, la solución
local puesto = {}          -- 1..81, lo que puso la persona (0 = vacía)
local malas = {}           -- [i] = true si Verificar la marcó
local cursor = 1
local resuelto = false
local segundos = 0         -- tiempo jugado
local ultimoMs = nil       -- para acumular con cp.ms()
local minutoVisto = 0      -- el minuto que está pintado
local aviso = nil          -- un renglón bajo el tablero, hasta la próxima tecla
local hayGuardada = false
local menuCursor = 1
local nivelCursor = 1
local inicioCursor = 1

local MENU = { "Dictar", "Verificar", "Pista", "Nuevo", "Salir" }

-- ---------------------------------------------------------------------------
-- Guardas para lo que viene de afuera (cp.load, el micrófono)

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
-- El tablero: derivación por simetría

-- Un generador propio para que la semilla reproduzca siempre el mismo
-- tablero (math.random no promete eso entre versiones). Es xorshift32 sobre
-- enteros, a propósito: el Lua del aparato es de 32 bits (LUA_32BITS) y ahí
-- cualquier cuenta que se pase de 2^31 cae a float de simple precisión y el
-- generador deja de generar. Con enteros que dan la vuelta es exacto.
local rngEstado = 1
local function rng(k)
  local x = rngEstado
  x = x ~ (x << 13)
  x = x ~ (x >> 17)
  x = x ~ (x << 5)
  rngEstado = x
  return (x >> 8) % k + 1
end

local function barajar(t)
  for i = #t, 2, -1 do
    local j = rng(i)
    t[i], t[j] = t[j], t[i]
  end
  return t
end

-- Devuelve (dadas, sol) como tablas 1..81 a partir de un tablero base y una
-- semilla. Cada simetría lleva un sudoku válido a otro sudoku válido con la
-- misma cantidad de soluciones, así que la unicidad se conserva.
local function derivar(par, sem)
  rngEstado = sem
  if rngEstado == 0 then rngEstado = 1 end
  local perm = barajar({ 1, 2, 3, 4, 5, 6, 7, 8, 9 })
  local bandas = barajar({ 0, 1, 2 })
  local pilas = barajar({ 0, 1, 2 })
  local filaMapa, colMapa = {}, {}
  for b = 0, 2 do
    local dentro = barajar({ 0, 1, 2 })
    for k = 0, 2 do filaMapa[b * 3 + k + 1] = bandas[b + 1] * 3 + dentro[k + 1] end
    dentro = barajar({ 0, 1, 2 })
    for k = 0, 2 do colMapa[b * 3 + k + 1] = pilas[b + 1] * 3 + dentro[k + 1] end
  end
  local traspone = rng(2) == 1
  local d, so = {}, {}
  local g, sg = par[1], par[2]
  for r = 1, 9 do
    for c = 1, 9 do
      local fr, fc = filaMapa[r], colMapa[c]
      local viejo = fr * 9 + fc + 1
      local nuevo
      if traspone then nuevo = (c - 1) * 9 + r else nuevo = (r - 1) * 9 + c end
      local gd = g:byte(viejo) - 48
      d[nuevo] = gd == 0 and 0 or perm[gd]
      so[nuevo] = perm[sg:byte(viejo) - 48]
    end
  end
  return d, so
end

-- Filas, columnas y cajas son permutaciones de 1..9, y las dadas coinciden.
local function tableroValido(d, so)
  if #d ~= 81 or #so ~= 81 then return false end
  for k = 0, 8 do
    local vf, vc, vb = {}, {}, {}
    local r0, c0 = (k // 3) * 3, (k % 3) * 3
    for j = 0, 8 do
      local f = so[k * 9 + j + 1]
      local c = so[j * 9 + k + 1]
      local b = so[(r0 + j // 3) * 9 + c0 + j % 3 + 1]
      if f < 1 or f > 9 or vf[f] or vc[c] or vb[b] then return false end
      vf[f], vc[c], vb[b] = true, true, true
    end
  end
  for i = 1, 81 do
    if d[i] ~= 0 and d[i] ~= so[i] then return false end
  end
  return true
end

local function primeraVacia()
  for i = 1, 81 do if dadas[i] == 0 then return i end end
  return 1
end

local function mover(paso)
  for _ = 1, 81 do
    cursor = cursor + paso
    if cursor > 81 then cursor = 1 elseif cursor < 1 then cursor = 81 end
    if dadas[cursor] == 0 then return end
  end
end

-- ---------------------------------------------------------------------------
-- Guardar y cargar

local function tiraDe(t)
  local partes = {}
  for i = 1, 81 do partes[i] = tostring(t[i] or 0) end
  return table.concat(partes)
end

local function tablaDe(str, largo)
  str = s(str)
  if #str ~= largo then return nil end
  local t = {}
  for i = 1, largo do
    local b = str:byte(i) - 48
    if b < 0 or b > 9 then return nil end
    t[i] = b
  end
  return t
end

local function guardar()
  if resuelto then
    cp.save("")
    hayGuardada = false
    return
  end
  cp.save(table.concat({
    "sudoku 1",
    "nivel " .. NIVELES[nivel].clave,
    "base " .. base,
    "semilla " .. semilla,
    "tiempo " .. segundos,
    "dadas " .. tiraDe(dadas),
    "sol " .. tiraDe(sol),
    "puesto " .. tiraDe(puesto),
  }, "\n"))
  hayGuardada = true
end

-- Lee lo guardado. Devuelve true sólo si dejó una partida coherente en pie;
-- cualquier cosa rara (archivo cortado, dígitos que no cuadran) es "no hay
-- partida", nunca un error.
local function cargar()
  local texto = cp.load()
  if type(texto) ~= "string" or texto == "" then return false end
  local campos = {}
  for linea in texto:gmatch("[^\n]+") do
    local k, v = linea:match("^(%a+)%s+(.-)%s*$")
    if k then campos[k] = v end
  end
  if campos.sudoku ~= "1" then return false end
  local niv = 0
  for i, nv in ipairs(NIVELES) do if nv.clave == campos.nivel then niv = i end end
  if niv == 0 then return false end
  local d = tablaDe(campos.dadas, 81)
  local so = tablaDe(campos.sol, 81)
  local p = tablaDe(campos.puesto, 81)
  if not (d and so and p) then return false end
  if not tableroValido(d, so) then return false end
  for i = 1, 81 do if d[i] ~= 0 then p[i] = 0 end end
  nivel, dadas, sol, puesto = niv, d, so, p
  base = n(campos.base, 1)
  semilla = n(campos.semilla, 1)
  segundos = math.max(0, n(campos.tiempo, 0))
  malas = {}
  resuelto = false
  cursor = primeraVacia()
  return true
end

-- ---------------------------------------------------------------------------
-- La partida

local function nueva(niv)
  nivel = niv
  local tabla = NIVELES[niv].tabla
  base = math.random(1, #tabla)
  semilla = math.random(1, 2147483646)
  dadas, sol = derivar(tabla[base], semilla)
  puesto = {}
  for i = 1, 81 do puesto[i] = 0 end
  malas = {}
  resuelto = false
  segundos = 0
  minutoVisto = 0
  aviso = nil
  cursor = primeraVacia()
  pantalla = "juego"
  guardar()
end

local function completo()
  for i = 1, 81 do
    if dadas[i] == 0 and puesto[i] ~= sol[i] then return false end
  end
  return true
end

local function revisarResuelto()
  if completo() then
    resuelto = true
    malas = {}
    aviso = nil
    guardar()
    cp.beep("ok")
  end
end

local function ponerEn(i, valor)
  puesto[i] = valor
  malas[i] = nil
  revisarResuelto()
  if not resuelto then guardar() end
end

local function verificar()
  local mal, faltan = 0, 0
  malas = {}
  for i = 1, 81 do
    if dadas[i] == 0 then
      if puesto[i] == 0 then
        faltan = faltan + 1
      elseif puesto[i] ~= sol[i] then
        mal = mal + 1
        malas[i] = true
      end
    end
  end
  if mal == 0 and faltan == 0 then
    resuelto = true
    guardar()
  elseif mal == 0 then
    aviso = "Todo bien hasta ahora · faltan " .. faltan
  elseif mal == 1 then
    aviso = "Hay 1 celda mal"
  else
    aviso = "Hay " .. mal .. " celdas mal"
  end
  cp.beep(mal == 0 and "ok" or "error")
end

local function pista()
  if dadas[cursor] ~= 0 then return end
  ponerEn(cursor, sol[cursor])
  if not resuelto then aviso = "Pista: " .. sol[cursor] end
  cp.beep("ok")
end

-- ---------------------------------------------------------------------------
-- Dictado: "fila tres columna cinco siete", "tres cinco siete",
-- "borra fila dos columna cuatro", "siete" (en la celda del cursor).

local NUMEROS = {
  cero = 0, uno = 1, un = 1, una = 1, dos = 2, tres = 3, cuatro = 4, cinco = 5,
  seis = 6, siete = 7, ocho = 8, nueve = 9,
}
local BORRAR = {
  borra = true, borrar = true, borrala = true, quita = true, quitar = true,
  quitala = true, limpia = true, limpiar = true, vacia = true, vaciar = true,
  saca = true, sacar = true, nada = true, vacio = true,
}
local FILA = { fila = true, filas = true, renglon = true, linea = true, horizontal = true }
local COLUMNA = { columna = true, columnas = true, col = true, vertical = true }
-- Palabras que pueden venir entre "fila" y su cifra sin que signifiquen nada.
local RELLENO = {
  la = true, el = true, lo = true, de = true, del = true, en = true, y = true, e = true,
  numero = true, num = true, pon = true, poner = true, pone = true, escribe = true,
  mete = true, celda = true, casilla = true, a = true, al = true, con = true, valor = true,
  cifra = true, digito = true, es = true, coloca = true, marca = true, que = true,
}

local ACENTOS = {
  ["á"] = "a", ["é"] = "e", ["í"] = "i", ["ó"] = "o", ["ú"] = "u", ["ü"] = "u", ["ñ"] = "n",
  ["Á"] = "a", ["É"] = "e", ["Í"] = "i", ["Ó"] = "o", ["Ú"] = "u", ["Ü"] = "u", ["Ñ"] = "n",
}

local function normalizar(texto)
  texto = s(texto):gsub("[\195][\128-\191]", function(ch) return ACENTOS[ch] or ch end)
  return texto:lower()
end

-- Convierte lo dicho en palabras y números sueltos. "357" se abre en 3, 5, 7.
local function partir(texto)
  local fichas = {}
  for palabra in normalizar(texto):gmatch("[%a%d]+") do
    if palabra:match("^%d+$") then
      for i = 1, #palabra do fichas[#fichas + 1] = palabra:byte(i) - 48 end
    else
      fichas[#fichas + 1] = palabra
    end
  end
  return fichas
end

local function esNumero(f)
  return type(f) == "number" or NUMEROS[f] ~= nil
end

local function valorDe(f)
  if type(f) == "number" then return f end
  return NUMEROS[f]
end

-- Devuelve true, fila, columna, valor (valor nil = sólo mover el cursor;
-- fila y columna nil = la celda del cursor), o false y el motivo.
local function interpretar(texto)
  local fichas = partir(texto)
  local fila, col, borrar = nil, nil, false
  local sueltos = {}
  local esperando = nil   -- "fila" o "col": la próxima cifra va ahí
  for i, f in ipairs(fichas) do
    if type(f) == "string" and FILA[f] then
      esperando = "fila"
    elseif type(f) == "string" and COLUMNA[f] then
      esperando = "col"
    elseif type(f) == "string" and BORRAR[f] then
      borrar = true
    elseif esNumero(f) then
      -- "un siete" es un siete, no un uno y un siete.
      local articulo = (f == "un" or f == "una") and esNumero(fichas[i + 1])
      if not articulo then
        local v = valorDe(f)
        if esperando == "fila" and not fila then
          fila = v
        elseif esperando == "col" and not col then
          col = v
        else
          sueltos[#sueltos + 1] = v
        end
        esperando = nil
      end
    elseif esperando and not (type(f) == "string" and RELLENO[f]) then
      -- "fila doce": después de "fila" vino algo que no es una cifra. Mejor
      -- no entender que poner un número en otra celda.
      return false, "No entendí"
    end
  end
  -- Sin "fila"/"columna": los números sueltos van en orden fila, columna, valor.
  local k = 1
  if fila == nil and col == nil then
    if #sueltos >= 3 then
      fila, col = sueltos[1], sueltos[2]
      k = 3
    elseif #sueltos == 2 then
      fila, col = sueltos[1], sueltos[2]
      k = 3
    end
  elseif fila ~= nil and col == nil and #sueltos >= 1 then
    col = sueltos[1]
    k = 2
  elseif fila == nil and col ~= nil and #sueltos >= 1 then
    fila = sueltos[1]
    k = 2
  end
  local valor = sueltos[k]
  if borrar then valor = 0 end
  if fila == nil and col == nil and valor == nil then return false, "No entendí" end
  if (fila ~= nil) ~= (col ~= nil) then return false, "No entendí" end
  if fila and (fila < 1 or fila > 9 or col < 1 or col > 9) then return false, "Fila y columna van de 1 a 9" end
  if valor and (valor < 0 or valor > 9) then return false, "El número va de 1 a 9" end
  return true, fila, col, valor
end

function on_heard(texto)
  if pantalla ~= "juego" then return end
  if texto == nil or s(texto) == "" then
    aviso = "No entendí"
    cp.beep("error")
    return
  end
  local ok, fila, col, valor = interpretar(texto)
  if not ok then
    -- `fila` trae el motivo. Se muestra lo oído: así se ve si el micrófono
    -- entendió otra cosa o si fue la frase.
    aviso = fila .. ": " .. s(texto)
    cp.beep("error")
    return
  end
  local i
  if fila then i = (fila - 1) * 9 + col else i = cursor end
  if dadas[i] ~= 0 then
    aviso = "La celda " .. ((i - 1) // 9 + 1) .. "," .. ((i - 1) % 9 + 1) .. " viene dada"
    cp.beep("error")
    return
  end
  cursor = i
  if valor ~= nil then
    ponerEn(i, valor)
    if not resuelto then
      aviso = (valor == 0 and "Borrada" or ("Puesto " .. valor)) .. " en fila " .. ((i - 1) // 9 + 1)
        .. ", columna " .. ((i - 1) % 9 + 1)
    end
  else
    aviso = "Cursor en fila " .. ((i - 1) // 9 + 1) .. ", columna " .. ((i - 1) % 9 + 1)
  end
  cp.beep("ok")
end

-- ---------------------------------------------------------------------------
-- Teclas

function on_open()
  math.randomseed(cp.ms())
  local t = cp.time()
  if t then math.randomseed(cp.ms() + n(t.epoch, 0)) end
  ultimoMs = nil
  aviso = nil
  hayGuardada = cargar()
  if hayGuardada then
    pantalla = "inicio"
    inicioCursor = 1
  else
    pantalla = "nivel"
    nivelCursor = 1
  end
end

local function teclaInicio(k)
  if k == "up" then
    inicioCursor = inicioCursor > 1 and inicioCursor - 1 or 3
  elseif k == "down" then
    inicioCursor = inicioCursor < 3 and inicioCursor + 1 or 1
  elseif k == "ok" then
    if inicioCursor == 1 then
      pantalla = "juego"
      minutoVisto = segundos // 60
      ultimoMs = nil
    elseif inicioCursor == 2 then
      pantalla = "nivel"
      nivelCursor = nivel
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

local function teclaNivel(k)
  if k == "up" then
    nivelCursor = nivelCursor > 1 and nivelCursor - 1 or #NIVELES
  elseif k == "down" then
    nivelCursor = nivelCursor < #NIVELES and nivelCursor + 1 or 1
  elseif k == "ok" then
    nueva(nivelCursor)
    ultimoMs = nil
  else
    -- Atrás: si hay partida, se vuelve a ella; si no, se sale.
    if hayGuardada then
      pantalla = "juego"
      cp.beep("back")
      return true
    end
    return false
  end
  cp.beep("nav")
  return true
end

local function teclaMenu(k)
  if k == "up" then
    menuCursor = menuCursor > 1 and menuCursor - 1 or #MENU
  elseif k == "down" then
    menuCursor = menuCursor < #MENU and menuCursor + 1 or 1
  elseif k == "ok" then
    local opcion = MENU[menuCursor]
    pantalla = "juego"
    if opcion == "Dictar" then
      if resuelto then
        aviso = "Ya está resuelto"
      elseif not cp.listen(8, "Di fila, columna y número") then
        aviso = "El micrófono está ocupado"
      end
    elseif opcion == "Verificar" then
      if resuelto then aviso = nil else verificar() end
    elseif opcion == "Pista" then
      if not resuelto then pista() end
    elseif opcion == "Nuevo" then
      pantalla = "nivel"
      nivelCursor = nivel
    elseif opcion == "Salir" then
      guardar()
      cp.quit()
      return false
    end
  else
    pantalla = "juego"
    cp.beep("back")
    return true
  end
  cp.beep("nav")
  return true
end

local function teclaJuego(k)
  if k == "back" then
    pantalla = "menu"
    menuCursor = 1
    cp.beep("nav")
    return true
  end
  aviso = nil
  if resuelto then
    if k == "ok" then
      pantalla = "nivel"
      nivelCursor = nivel
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
    if dadas[cursor] ~= 0 then
      cp.beep("error")
      return true
    end
    ponerEn(cursor, (puesto[cursor] + 1) % 10)
    if not resuelto then cp.beep("ok") end
    return true
  else
    return false
  end
  cp.beep("nav")
  return true
end

function on_key(k)
  if pantalla == "inicio" then return teclaInicio(k) end
  if pantalla == "nivel" then return teclaNivel(k) end
  if pantalla == "menu" then return teclaMenu(k) end
  return teclaJuego(k)
end

-- El tiempo se acumula con cp.ms(), que anda aunque el aparato no esté en
-- hora. Se repinta sólo cuando cambia el minuto del cabezal.
function on_tick()
  if (pantalla ~= "juego" and pantalla ~= "menu") or resuelto then
    ultimoMs = nil   -- lo que se tarda eligiendo nivel no es tiempo jugado
    return false
  end
  local ahora = cp.ms()
  if ultimoMs == nil or ahora < ultimoMs then
    ultimoMs = ahora
    return false
  end
  local enteros = (ahora - ultimoMs) // 1000
  if enteros > 0 then
    segundos = segundos + enteros
    ultimoMs = ultimoMs + enteros * 1000
  end
  local minuto = segundos // 60
  if minuto ~= minutoVisto then
    minutoVisto = minuto
    guardar()
    return true
  end
  return false
end

-- ---------------------------------------------------------------------------
-- Dibujo

local CELDA = 48
local X0 = 24
local Y0 = 92

local function centrado(texto, y, tam, negrita)
  cp.text((cp.width() - cp.textw(texto, tam)) // 2, y, texto, tam, negrita)
end

-- Acorta con "…" lo que no entra en `ancho` píxeles (lo oído puede ser largo).
local function recortar(texto, ancho, tam)
  if cp.textw(texto, tam) <= ancho then return texto end
  while #texto > 0 and cp.textw(texto .. "…", tam) > ancho do
    texto = texto:sub(1, (utf8.offset(texto, -1) or 2) - 1)
  end
  return texto .. "…"
end

local function lista(y, items, cur, alto)
  for i, item in ipairs(items) do
    local fy = y + (i - 1) * alto
    if i == cur then cp.selection(24, fy, cp.width() - 48, alto) end
    cp.text(48, fy + (alto - cp.texth(12)) // 2, item, 12, i == cur)
  end
end

local function tiempoTexto()
  local m = segundos // 60
  return m .. " min"
end

local function dibujarTablero()
  local tam = 14
  local altoTexto = cp.texth(tam)
  local fila = (cursor - 1) // 9
  local col = (cursor - 1) % 9
  -- Fila y columna del cursor, con un marco fino por dentro de la cuadrícula:
  -- se ve dónde se está sin tramar nada debajo de los números.
  if not resuelto then
    cp.rect(X0 + 2, Y0 + fila * CELDA + 2, 9 * CELDA - 4, CELDA - 4, false, 1)
    cp.rect(X0 + col * CELDA + 2, Y0 + 2, CELDA - 4, 9 * CELDA - 4, false, 1)
  end
  -- Las líneas: finas entre celdas, gruesas cada tres y en el borde.
  for k = 0, 9 do
    local grosor = (k % 3 == 0) and 3 or 1
    local p = k * CELDA
    cp.line(X0, Y0 + p, X0 + 9 * CELDA, Y0 + p, grosor)
    cp.line(X0 + p, Y0, X0 + p, Y0 + 9 * CELDA, grosor)
  end
  for i = 1, 81 do
    local r, c = (i - 1) // 9, (i - 1) % 9
    local x, y = X0 + c * CELDA, Y0 + r * CELDA
    local d = dadas[i]
    local negrita = d ~= 0
    if d == 0 then d = puesto[i] end
    if d ~= 0 then
      local texto = tostring(d)
      cp.text(x + (CELDA - cp.textw(texto, tam)) // 2, y + (CELDA - altoTexto) // 2, texto, tam, negrita)
    end
    if malas[i] then cp.rect(x + CELDA - 10, y + 4, 6, 6, true) end
    if i == cursor and not resuelto then cp.rect(x + 3, y + 3, CELDA - 6, CELDA - 6, false, 3) end
  end
end

function on_draw()
  local alto = cp.height()
  if pantalla == "inicio" then
    cp.text(24, 40, "Sudoku", 14, true)
    cp.line(24, 76, cp.width() - 24, 76, 1)
    cp.text(24, 100, "Hay una partida " .. NIVELES[nivel].nombre:lower() .. " a medias (" .. tiempoTexto() .. ")", 12)
    lista(150, { "Continuar", "Nueva partida", "Salir" }, inicioCursor, 48)
    cp.text(24, alto - 40, "Palanca: elegir · OK: entrar · Atrás: salir", 10)
    return
  end
  if pantalla == "nivel" then
    cp.text(24, 40, "Sudoku · Nivel", 14, true)
    cp.line(24, 76, cp.width() - 24, 76, 1)
    cp.text(24, 100, "Elige la dificultad", 12)
    local nombres = {}
    for i, nv in ipairs(NIVELES) do nombres[i] = nv.nombre end
    lista(150, nombres, nivelCursor, 48)
    cp.text(24, alto - 40, hayGuardada and "OK: empezar · Atrás: volver a la partida" or "OK: empezar · Atrás: salir", 10)
    return
  end
  -- Juego y menú comparten el tablero arriba.
  local titulo = "Sudoku · " .. NIVELES[nivel].nombre .. " · " .. tiempoTexto() .. " jugados"
  cp.text(24, 40, titulo, 14, true)
  cp.line(24, 76, cp.width() - 24, 76, 1)
  dibujarTablero()
  local abajo = Y0 + 9 * CELDA + 24
  if pantalla == "menu" then
    lista(abajo, MENU, menuCursor, 40)
    cp.text(24, alto - 40, "Palanca: elegir · OK: hacer · Atrás: cerrar", 10)
    return
  end
  if resuelto then
    centrado("¡Resuelto!", abajo + 16, 14, true)
    centrado("en " .. tiempoTexto(), abajo + 16 + cp.texth(14) + 8, 12)
    cp.text(24, alto - 40, "OK: otra partida · Atrás: menú", 10)
    return
  end
  if aviso then
    cp.text(24, abajo + 8, recortar(aviso, cp.width() - 48, 12), 12)
  else
    cp.text(24, abajo + 8, "Fila " .. ((cursor - 1) // 9 + 1) .. ", columna " .. ((cursor - 1) % 9 + 1), 10)
  end
  cp.text(24, alto - 40, "Palanca: celda · OK: número · Atrás: menú", 10)
end
