# Apps en Lua desde la tarjeta

Los doce juegos que trae el aparato son Activities compiladas: agregar el trece
es tocar el firmware, compilar y actualizar. Esto es para que eso no haga falta.

Una app es **un archivo** en `/Apps` de la tarjeta:

    /Apps/reloj.lua
    /Apps/cuenta.lua

Se copian con **Ajustes → Sistema → Modo memoria USB** (la tarjeta aparece como
un disco) y se abren en **Juegos → Apps de la tarjeta**. Instalar es copiar,
desinstalar es borrar.

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

Los gestos que devuelve `cp.motion()` son los mismos de todo el aparato:
`TiltLeft`, `TiltRight`, `TiltForward`, `TiltBack`, `Shake`, `Rotate`, `Level`,
`FaceDown`, `FaceUp`, `DoubleTap`.

## El cajón

Una app **no puede**: abrir archivos, salir a la red, tocar el I2C o el SPI del
panel, cargar código nativo, ni ejecutar texto que arme en el momento.

Están `math`, `string`, `table`, `utf8`, `coroutine` y la base de Lua. **No**
están `io`, `os`, `package`, `debug`, `require`, `load`, `loadstring`, `dofile`,
`loadfile` ni `string.dump`. Los `.c` de esas bibliotecas ni siquiera están en
`lib/Lua`.

Lo único que una app escribe en la tarjeta es su propio archivo de estado, con
`cp.save`. No recibe rutas: no puede elegir dónde escribir.

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
