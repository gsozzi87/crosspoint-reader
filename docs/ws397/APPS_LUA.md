# Apps en Lua desde la tarjeta

Los doce juegos que trae el aparato son Activities compiladas: agregar el trece
es tocar el firmware, compilar y actualizar. Esto es para que eso no haga falta.

Una app es **un archivo** en `/Apps` de la tarjeta:

    /Apps/sudoku.lua
    /Apps/cuenta.lua

Se copian con **Ajustes → Sistema → Modo memoria USB** (la tarjeta aparece como
un disco) y se abren en **Juegos → Apps de la tarjeta**. Instalar es copiar,
desinstalar es borrar.

## El nombre que se ve

La lista de apps y el cabezal de la app abierta muestran el **título** y la
**descripción** que salen del comentario que abre el archivo, nunca la ruta:

```lua
-- Reloj: la hora grande, la fecha debajo.
```

La regla: la primera línea del archivo es un comentario `--`, y lo que hay
antes del primer `:` o `.` (hasta 32 caracteres) es el título; el resto de esa
línea es la descripción. Sin ese comentario el título es el nombre del archivo
con mayúscula (`cuenta.lua` → "Cuenta") y no hay descripción.

## El contrato

El script define lo que quiera de esto. Nada es obligatorio.

```lua
function on_open()      -- una vez, al abrir
function on_key(k)      -- "up" "down" "ok" "back"; devolver true repinta
function on_tick()      -- cada 120 ms; devolver true repinta
function on_draw()      -- pintar; la pantalla ya viene limpia
```

Es de callbacks y no de bucle propio **a propósito**. En tinta electrónica el
refresco lo tiene que decidir el firmware (la regla del panel es un refresco
completo cada 10-15 parciales, si no queda fantasma), y una app con su propio
`while true` se comería el loop, los recordatorios y el reposo.

`on_key("back")` que devuelva `false` (o que no exista) **sale de la app**. Y
**Atrás mantenido un segundo sale siempre**, aunque la app se coma el botón: no
hay forma de quedar encerrado.

## La tabla `cp`

Es lo único que una app ve del aparato.

### Dibujar

| Llamada | Qué hace |
| --- | --- |
| `cp.clear()` | Limpia el buffer (ya viene limpio en `on_draw`) |
| `cp.text(x, y, s [, tam] [, negrita])` | Texto. `tam` es 10, 12 o 14; por omisión 12 |
| `cp.textw(s [, tam])` | Ancho en píxeles de ese texto |
| `cp.texth([tam])` | Alto de un renglón |
| `cp.rect(x, y, w, h [, lleno] [, grosor])` | Rectángulo |
| `cp.line(x1, y1, x2, y2 [, grosor])` | Línea |
| `cp.image(x, y, w, h, bits [, escala])` | Un bitmap de 1 bit: `bits` es un string con las filas empaquetadas (MSB primero, **1 = tinta**, cada fila redondeada a byte, `ceil(w/8)` bytes por fila), hasta 256 × 256. `escala` entera de 1 a 8 agranda cada píxel a un cuadrado. Es la puerta de los dibujitos: 64 × 64 son 512 bytes, se llevan bien como `"\xFF\x00…"` o decodificando un string hexa al abrir |
| `cp.selection(x, y, w, h)` | El resalte del sistema visual (ver `DISENO.md`) |
| `cp.width()`, `cp.height()` | Tamaño de la pantalla: 480 × 800 |

`cp.selection` es el resalte de todo el aparato: pestaña negra a la izquierda,
marco y franjas tramadas **sólo en los márgenes**, con el centro en blanco. Si
una app hace una lista, que use eso y no invente su propio negro macizo: la
regla número uno del sistema visual es que **nunca hay letras sobre trama**.

### Lo demás

| Llamada | Qué hace |
| --- | --- |
| `cp.motion()` | El gesto pendiente del acelerómetro, o `nil`. Lo consume |
| `cp.ms()` | Milisegundos desde que arrancó el aparato |
| `cp.beep([cuál])` | Un clic: `"nav"`, `"ok"`, `"back"`, `"error"` |
| `cp.log(...)` | Al log del aparato (se lee en `/board/log`). `print` es esto |
| `cp.save(texto)` | Guarda hasta 4 KB en `/Apps/.state/<app>.txt` |
| `cp.load()` | Lee eso, o `nil` la primera vez |
| `cp.quit()` | Cierra la app y vuelve al catálogo |
| `cp.time()` | La hora local, o `nil` si el aparato no está en hora |

Y las **puertas** —escuchar, llamar al servidor, bajar archivos, la carpeta de
la app, el visor y el lector— van en su propia sección más abajo.

`cp.time()` es la **única** forma que tiene una app de saber la hora: `os` no
está en el cajón (`os.execute` y `os.remove` vienen en la misma biblioteca).
Devuelve una tabla con `year`, `month`, `day`, `hour`, `min`, `sec`, `wday`
(1 = lunes, 7 = domingo) y `epoch` (UTC, para medir diferencias sin pelearse con
el huso). **Devuelve `nil` cuando el aparato todavía no está en hora**, que es un
estado real y frecuente: sin WiFi y sin haber sincronizado nunca, el RTC no sabe
nada. Una app que no contemple ese caso se rompe justo cuando alguien la abre
recién sacada de la caja.

Y una advertencia que no es de la API sino del vidrio: si lo que mostrás cambia
con el tiempo, repintá **cuando cambia el dato**, no en cada `on_tick`. Devolver
`true` diez veces por segundo es un refresco parcial cada 120 ms, o sea un
refresco completo cada segundo y medio, para siempre. `mascota.lua` lo hace
bien: el cuadro de la animación cambia cada cuatro segundos y el resto de los
`on_tick` devuelven `false`.

Los gestos que devuelve `cp.motion()` son los mismos de todo el aparato:
`TiltLeft`, `TiltRight`, `TiltForward`, `TiltBack`, `Shake`, `Rotate`, `Level`,
`FaceDown`, `FaceUp`, `DoubleTap`.

### Las puertas: micrófono, servidor, archivos, visor y lector

Una app también puede escuchar, hablar con el servidor de la cuenta, bajar
archivos, guardar lo suyo en la tarjeta y abrir lo que bajó en los visores del
aparato. Es lo que hacen el Librito (`examples/Apps/librito.lua`) y la
Biblioteca (`examples/Apps/libros.lua`). La regla es la misma que la del cajón:
**nada de URLs ni de rutas**; la app nombra un servicio o un archivo, y a dónde va eso lo decide el
firmware.

| Llamada | Devuelve | Qué hace |
| --- | --- | --- |
| `cp.listen(seg [, pregunta])` | `true` si quedó pedido | Escucha hasta `seg` segundos (tope 30) con la pantalla de escucha del sistema (`pregunta` arriba, "Escuchando…", OK termina, Atrás cancela). Lo entendido llega por **`on_heard(texto)`**; `nil` si canceló, no se entendió o falló |
| `cp.call(servicio [, args])` | `id` (entero) o `nil` | `POST /api/apps/call` con `{app, service, args}` y el Bearer del aparato. La respuesta llega por **`on_reply(id, ok, tabla)`** |
| `cp.download(fileId, nombre [, destino])` | `id` o `nil` | Baja `GET /api/apps/file/<fileId>` a la carpeta de la app (`destino = "app"`, por omisión) o a `/Books/<app>/` (`"books"`). Llega **`on_reply(id, ok, {bytes = n})`** |
| `cp.busy()` | `true`/`false` | Hay una escucha, llamada, descarga o visor en curso, o pedidos encolados |
| `cp.files()` | tabla de nombres | Los archivos de `/Apps/data/<app>/` |
| `cp.read(nombre [, desde, largo])` | texto o `nil` | Entero si entra en **48 KB**; más grande, con rango (`desde` es 0-based) |
| `cp.write(nombre, texto)` | `true`/`false` | Hasta **64 KB**, atómico (`.tmp` y renombrar) |
| `cp.remove(nombre)` | `true`/`false` | |
| `cp.size(nombre)` | bytes o `nil` | |
| `cp.view(nombre [, titulo])` | `true` si existía | Abre el archivo (hasta 64 KB) en el **visor paginado del sistema**; al salir vuelve a la app y la repinta |
| `cp.open_book(nombre)` | `true` si existía | Abre `/Books/<app>/<nombre>` en el lector de CrossPoint. **La app se cierra**; al cerrar el libro se vuelve al hub |
| `cp.say(texto)` | `id` o `nil` | Lee `texto` (hasta 512 bytes) por el parlante con la voz del servidor (`GET /api/tts`, 45 s de audio como mucho). Llega **`on_reply(id, true, {})`** cuando el audio **arrancó** (no cuando terminó); con `ok = false`, `tabla.error` es `"sin voz (N)"`, `"cancelado"` o `"sin vincular"`. La app **sigue mientras suena** y puede abrir `cp.view` encima; Atrás corta la voz (en la app, ese Atrás no le llega; en el visor, la corta al volver) |

`<app>` es el nombre del archivo sin `.lua`: `librito.lua` guarda en
`/Apps/data/librito/` y sus libros en `/Books/librito/`. Los nombres de archivo
que pasa la app son `[A-Za-z0-9._-]`, de 1 a 48, sin punto inicial y sin
barras; cualquier otra cosa devuelve `false`/`nil` sin tocar la tarjeta.

**Todo lo que espera es asíncrono y llega por callback.** `cp.listen`,
`cp.call`, `cp.download`, `cp.view` y `cp.say` no hacen el trabajo: lo **encolan** y
vuelven en el acto. El firmware lo atiende desde su propio loop, con sus
pantallas (escucha, conexión al WiFi, "Esperando al servidor…", el visor), y
cuando termina llama a la app:

```lua
function on_heard(texto)          -- lo que dijo el usuario, o nil
function on_reply(id, ok, tabla)  -- la respuesta al cp.call / cp.download / cp.say con ese id
```

Ninguno de los dos es obligatorio: si la app no los define, la respuesta se
descarta. Después de cada uno la pantalla se repinta sola (`on_draw`).

Cómo se lee `on_reply`:

* `ok` es `true` sólo si el servidor contestó HTTP 200, con JSON legible y
  `ok = true` adentro. `tabla` es el JSON del servidor convertido: objetos →
  tablas con claves string, arrays → tablas `1..n`, números, booleanos y
  strings tal cual (un `jobId` que llega como string sigue siendo string), y
  `null` → `nil`.
* Con `ok = false`, `tabla.error` es el texto. Los que pone el firmware:
  `"sin vincular"` (el aparato no tiene token: no se va a la red),
  `"sin conexión con el servidor (N)"` (falló el HTTP; `N` es el estado, 0 si no
  hubo respuesta), `"descarga fallida (N)"`, `"cancelado"` (Atrás) y
  `"respuesta ilegible del servidor"`.
* En `cp.call`, `args` viaja como JSON: una tabla con claves string es un
  objeto, una tabla secuencial `1..n` es un array, una tabla vacía es un objeto
  vacío. Tope de **16 KB** y **6 niveles** de profundidad; pasarse devuelve
  `nil` y lo dice en el log. Funciones y cosas que no viajan tampoco.

Las reglas del tráfico:

* **De a uno, y hasta cuatro en cola.** Los pedidos salen en orden; el quinto
  `cp.call` devuelve `nil` y una segunda `cp.listen` con otra ya pendiente,
  `false`.
* **Mientras `cp.busy()`**, `on_key` no se llama salvo con `"back"`, que
  **cancela**: corta la escucha (`on_heard(nil)`) o descarta lo que todavía no
  salió (`on_reply(id, false, {error = "cancelado"})`), en cola incluida. Lo que
  ya está en el aire —un POST en curso— no se puede cortar: Atrás se ignora
  hasta que vuelve. `on_tick` sigue corriendo, salvo con el micrófono abierto.
* **El tope de instrucciones no corre mientras el firmware espera** a la red o
  al micrófono: la espera es del host, no del script. Lo que sí sigue vigente
  es que **la espera larga no va adentro de una llamada**: un trabajo de
  minutos se consulta desde `on_tick` con `cp.call("job.status", {id = …})`
  cada 5 s, mirando `cp.ms()`, como hace `librito.lua`.
* **`cp.say` cuenta como un pedido más en la cola** (sale en orden, ocupa
  `cp.busy()` sólo mientras se pide el clip al servidor) **y el audio no
  bloquea a la app**: en cuanto suena, `on_key` y `on_tick` vuelven a correr y
  `cp.busy()` da `false`. Un `cp.listen` que venga después corta la voz antes
  de abrir el micrófono (el I2S es uno solo). Mientras suena el aparato no se
  duerme solo.
* **WiFi**: la primera `cp.listen`, `cp.call`, `cp.download` o `cp.say` levanta la red
  con las redes guardadas (pantalla de conexión del sistema si tarda; el
  selector si ninguna sirve) y la deja arriba **hasta que la app se cierra**.
  Sin token del aparato (no está vinculado) no se va a la red: `on_reply`
  vuelve con `"sin vincular"` y `on_heard` con `nil`.
* **Escuchando o con un pedido en curso el aparato no se duerme solo**, y con
  la red arriba tampoco; la red de seguridad de la media hora sin tocar nada
  sigue mandando.
* `cp.open_book` cierra la app y la radio y abre el lector; si en la sesión
  hubo red, va con reinicio silencioso (el lector necesita el heap entero, igual
  que Preguntarle al libro). Guarda antes lo que quieras conservar con
  `cp.save` o `cp.write`.

Un esqueleto:

```lua
local estado, oido, pedido = "tema", nil, nil

function on_key(k)
  if cp.busy() then return k == "back" end
  if k == "ok" and estado == "tema" then
    cp.listen(15, "¿Sobre qué quieres leer?")   -- vuelve en el acto
    return true
  end
end

function on_heard(texto)
  if not texto then return end                  -- canceló o no se entendió
  oido = texto
  pedido = cp.call("librito.enfoque", { tema = texto })
end

function on_reply(id, ok, r)
  if id ~= pedido then return end
  if not ok then estado = "error"; oido = r.error; return end
  estado = "enfoques"; -- r.enfoques[1].titulo, ...
end

function on_draw()
  cp.text(24, 100, cp.busy() and "Esperando al servidor…" or (oido or "OK: dictar el tema"))
end
```

## El cajón

Una app **no puede**: abrir archivos, salir a la red, tocar el I2C o el SPI del
panel, cargar código nativo, ni ejecutar texto que arme en el momento.

Están `math`, `string`, `table`, `utf8`, `coroutine` y la base de Lua. **No**
están `io`, `os`, `package`, `debug`, `require`, `load`, `loadstring`, `dofile`,
`loadfile` ni `string.dump`. Los `.c` de esas bibliotecas ni siquiera están en
`lib/Lua`.

Lo único que una app escribe en la tarjeta es su propio archivo de estado
(`cp.save`) y **su propia carpeta** `/Apps/data/<app>/` (`cp.write`,
`cp.download`), más `/Books/<app>/` para lo que baja con `destino = "books"`.
No recibe rutas: no puede elegir dónde escribir ni leer fuera de ahí. Con el
servidor pasa lo mismo: nombra un servicio (`cp.call`) o un archivo generado
(`cp.download`), nunca una URL.

Tres topes más:

* **192 KB** de memoria para el intérprete (sale de PSRAM). Pasarse es un error
  de la app, no un aparato sin memoria.
* **400.000 instrucciones** por llamada. Un `while true do end` termina en un
  error de la app y no en un aparato colgado.
* **32 KB de stack**, en un worker de vida corta (`tasks::runBounded`), así el
  stack de una app no vive en el loop de Arduino, que es el que anda justo.

La política del cajón vive en `src/lua/LuaSandbox.cpp`, separada del resto para
poder probarla de escritorio: `./test/lua_sandbox/run.sh` verifica que lo que
tiene que estar está y que lo que no, no.

## Probar una app sin aparato

`./test/lua_sandbox/run.sh` carga cada `examples/Apps/<app>.lua` contra un `cp`
falso completo y, si existe `test/lua_sandbox/scenarios/<app>.lua`, corre ese
guion encima. El guion tiene `cp` y una tabla `fake`:

| | |
| --- | --- |
| `fake.heard = "texto"` | La próxima `cp.listen` llama `on_heard` con eso en el siguiente `fake.step()` |
| `fake.reply["librito.enfoque"] = function(args) return true, {…} end` | La próxima `cp.call` de ese servicio llama `on_reply` con eso. Con un contador adentro de la función, `job.status` contesta distinto cada vez |
| `fake.download["f-1"] = "…"`, `fake.downloadFails = true` | Lo que "baja" `cp.download` (o que falle) |
| `fake.key("ok")`, `fake.tick()` | `on_key` / `on_tick`, con la regla de `busy()` (sólo pasa `"back"` con algo en curso) |
| `fake.step()` | Entrega **una** cosa pendiente, la más vieja |
| `fake.draw()` | Corre `on_draw` y devuelve los textos dibujados como lista |
| `fake.advance(ms)`, `fake.ms` | El reloj de `cp.ms()`, que en la prueba no avanza solo |
| `fake.opened` | Lo que abrió `cp.view` / `cp.open_book` (`{kind, name, title}`) |
| `fake.reload()` | Vacía la cola; `on_open` lo llama el guion |

Los archivos van a un directorio temporal. `cp.save`/`cp.load` persisten dentro
del guion. El guion llama a `on_open()` él mismo y falla con `error()`;
`test/lua_sandbox/scenarios/librito.lua` es el flujo entero del Librito de
punta a punta.

## Un ejemplo entero

```lua
-- Contador: la palanca suma y resta, OK reinicia. Se acuerda al salir.
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
    return false          -- false = salir
  end
  cp.beep("nav")
  return true             -- true = repintar
end

function on_draw()
  local texto = tostring(n)
  cp.text((cp.width() - cp.textw(texto, 14)) // 2, 360, texto, 14, true)
  cp.text(40, 700, "Palanca: sumar y restar · OK: volver a cero", 10)
end
```

Hay más en `examples/Apps/` del repo.
