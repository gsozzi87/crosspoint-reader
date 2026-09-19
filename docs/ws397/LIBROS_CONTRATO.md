# Libros — la app que le pide libros a un bot de Telegram (contrato)

Pedido del dueño (18-09-2026, con dos capturas de su Telegram): *"Digo un nombre, me llega esa lista, la
selecciono, luego aprieto el botón epub y se baja"*. El bot es un chat de Telegram que contesta a un texto con
una lista de títulos, cada uno con un comando (`Cien años de soledad /b0E_D`); mandarle el comando devuelve una
ficha (portada + texto + botones en línea: Información, Leer online, **Epub**, Reportar error) y apretar
**Epub** hace que el bot mande el archivo.

**Cómo está armado**: un bot no puede hablarle a otro bot, así que el servidor le escribe al bot **como la
cuenta de Telegram del dueño** (MTProto, `@mtcute/bun`), con una sesión por cuenta de `/board` guardada en el
volumen. La sesión se abre UNA vez desde la web (api id + api hash de my.telegram.org, teléfono, código, y la
contraseña de dos pasos si la hay). El aparato nunca ve nada de Telegram: habla con `/api/apps/call` como
cualquier app. El puente es genérico: sirve para cualquier bot que conteste con listas `Título /comando` y
entregue el libro con un botón; el nombre del bot se carga en la web.

## Servidor

### Sesión de Telegram (`server/src/telegram.ts`, por cuenta)

- Config y estado en el documento `telegram` de `fsjson` (por cuenta): `{apiId, apiHash, phone, bot, loggedIn,
  phoneCodeHash?, user?: {id, name, phone}}`. `apiHash` es secreto: nunca vuelve por la API (sólo `hasHash`).
- Sesión MTProto en `/data/telegram/<accountId>.session` (`TELEGRAM_DIR` para cambiarlo). Un `TelegramClient`
  por cuenta, perezoso y cacheado; si la sesión no está, todo servicio de la app devuelve
  `{ok:false, error:"Conecta Telegram en la web (Ajustes → Avanzado → Telegram)", code:"no_telegram"}`.
- Rutas web (con la sesión de `/board`, sin Bearer del aparato):
  - `GET  /api/board/telegram` → `{ok, configured, loggedIn, phone, bot, hasHash, user, awaitingCode, awaitingPassword}`
  - `POST /api/board/telegram/config` `{apiId?, apiHash?, phone?, bot?}` → guarda (hash vacío = no tocar)
  - `POST /api/board/telegram/code` → manda el código al teléfono → `{ok}` (`awaitingCode` queda en true)
  - `POST /api/board/telegram/signin` `{code?, password?}` → `{ok, loggedIn, awaitingPassword}`; con
    `SESSION_PASSWORD_NEEDED` contesta `awaitingPassword:true` y espera `password`
  - `POST /api/board/telegram/logout` → cierra la sesión y borra el archivo
  - `POST /api/board/telegram/test` `{q}` → hace `libros.buscar` y devuelve lo mismo (para probar desde la web)
- Cómo se le habla al bot (`askBot`): se manda el texto con `sendText`, se anota el id del mensaje enviado y se
  **sondea** `getHistory(bot, {limit: 6})` cada 700 ms hasta ver un mensaje **entrante** con id mayor, o hasta
  el plazo (20 s para texto, 120 s para el archivo). Nada de `on('new_message')`: el sondeo no depende del bucle
  de updates y se prueba fácil. Apretar un botón en línea = `getCallbackAnswer({chatId, message, data})` con el
  `data` del botón cuyo texto coincide (sin mayúsculas ni acentos) con el formato pedido.
- Un solo pedido por cuenta a la vez (candado): dos búsquedas encimadas confundirían las respuestas.

### Servicios (`POST /api/apps/call`, `app = "libros"`)

Siempre 200 con `{ok, …}`. Síncronos en < 25 s; bajar es un trabajo.

| Servicio | args | Devuelve |
| --- | --- | --- |
| `libros.estado` | — | `{connected, bot}` — para que la app diga "conecta Telegram en la web" antes de pedir el micrófono |
| `libros.buscar` | `{q, spelled?}` | `{results:[{title, code}], corrected?, spelled?}` — hasta 10 (la primera página del bot); `[]` si el bot dice que no encontró. `code` con la barra (`/b0E_D`). **`corrected`** viene sólo cuando el servidor corrigió lo dictado con el modelo y buscó con eso (ver "El corrector"): la app lo muestra como "Buscando «Ángeles Mastretta»". Con **`spelled: true`** `q` es el nombre **deletreado** ("a, ene, ge, e, ele…"): el servidor arma la palabra sin modelo (`lettersToWord`), la pasa por el corrector ANTES de buscar y devuelve la palabra armada en **`spelled`** ("angeles mastretta") |
| `libros.ficha` | `{code}` | `{title, author, year, pages, genre, desc, formats:[…]}` — `formats` son los botones en línea que parecen un formato (`epub`, `pdf`, `mobi`…), en minúsculas; el resto de los botones no viaja. `desc` ≤ 2 KB |
| `libros.bajar` | `{code, format?}` (`format` por omisión `epub`) | `{jobId}` — trabajo: manda `code`, aprieta el botón del formato, espera el documento (hasta 120 s), lo guarda con `saveFile` y termina con `files:[{id, name, bytes}]`. `name` = nombre del archivo que mandó el bot, o `<slug del título>.epub`. `label` mientras tanto ("Pidiendo la ficha…", "Esperando el archivo…", "Guardando…"). Tope 40 MB |
| `job.status` | `{id}` | el de siempre |

**El corrector** (`correctQuery` en `libros.ts`). El aparato no tiene teclado: el nombre entra por voz y el
transcriptor no conoce a los autores — el caso del dueño: dijo "ángeles mastretta", entendió "angeles mastretas"
y el bot no encontró nada. `libros.buscar` busca primero con lo dictado tal cual (barato, y casi siempre alcanza);
si el bot no devuelve nada, le pide al modelo de siempre (`chatText`, el mismo proveedor y clave que Hablar) la
consulta corregida, con este system prompt: *"The user dictated a book title and/or author name to search a
library. Fix obvious transcription errors using your knowledge of real authors and books (e.g. 'angeles
mastretas' → 'Ángeles Mastretta'). Return ONLY the corrected query, nothing else: no quotes, no explanation. If
it already looks right, return it unchanged."* Si lo que vuelve difiere de lo dictado (comparación sin
mayúsculas ni acentos), se busca otra vez con eso y se devuelve `corrected` — también cuando la segunda búsqueda
tampoco encuentra, para que la pantalla diga con qué se buscó. Sin clave, con el proveedor caído o con una
respuesta que no parece una consulta (varios renglones, tres veces más larga), el corrector devuelve `null` y la
búsqueda contesta `results: []` como siempre: **nunca falla por el corrector**. Las dos búsquedas y la llamada
al modelo van adentro de UN `withBot`, así el candado de la cuenta cubre el pedido entero.

**Deletrear** (`lettersToWord(text, lang)` en `librosParse.ts`, pura). Lo dicho se parte por comas, espacios y
guiones; cada trozo es el nombre de una letra en español (a, be, ce, de, e, efe, ge, hache, i, jota, ka, ele,
eme, ene, eñe, o, pe, cu, erre/ere, ese, te, u, uve/ve, doble uve/doble ve/uve doble, equis, i griega/ye,
zeta) o en inglés (ay, bee, see, dee, e, ef, gee, aitch, i, jay, kay, el, em, en, oh, pee, cue, ar, es, tee,
you, vee, double you, ex, why, zee/zed), "espacio"/"space" (→ espacio), "guion"/"dash" (→ `-`), una letra
suelta o un número (pasan tal cual), o —si tiene más de una letra y no es nada de eso— **una palabra entera**
dicha en el medio, que queda entera ("cien, espacio, a, eñe, o, ese" → "cien años"). "Mayúscula", "con acento"
y parecidos se tiran. `lang` decide qué tabla manda cuando un nombre vale en las dos ("de" es d en español, "el"
es l en inglés). Sale en minúsculas; acentos y mayúsculas los pone el corrector si hay modelo.

**Cómo se lee lo que dice el bot** (funciones puras en `server/src/librosParse.ts`, probadas en
`./test/libros/run.sh` con los textos de las dos capturas, y `lettersToWord` con Ángeles Mastretta deletreada en
español, Hemingway en inglés, palabras enteras en el medio y vacío):

- Lista: cada línea `^(.+?)\s+(/[A-Za-z0-9_]+)\s*$` es un resultado; las demás se ignoran. Las entidades de
  Telegram no hacen falta: el comando está en el texto.
- Ficha: primera línea `Título - Autor` (se parte por el último ` - `; sin autor si no hay); `(\d{4})` en una
  línea que empiece con "Publicado" o parecido = año; `(\d+)\s*p[aá]g` = páginas; la línea siguiente sin números
  = género; lo que sigue = descripción.

## `libros.lua` (`examples/Apps/libros.lua`, primera línea `-- LIBRARY: dime un título y te lo bajo.`)

La app se llama **LIBRARY** (decisión del dueño); el archivo sigue siendo `libros.lua` y el título sale del
comentario que abre el archivo (`LuaApp::titleFromHeader`).

Pantallas:

0. **Inicio**: "Presiona OK y di el título o el autor", y debajo **Bajados** (lo que esta app ya bajó, de
   `bajados.json` en su carpeta: `{name, title, author, at}`); OK sobre uno → `cp.open_book(name)`. Atrás largo
   sobre uno lo saca de la lista (no borra el archivo).
   Al abrir, `libros.estado` sólo si hay red pedida… NO: la app no levanta la red al abrir. El estado se pide
   junto con la primera búsqueda: si `buscar` devuelve `code == "no_telegram"`, la pantalla lo dice.
1. **Escuchando**: `cp.listen(10, "¿Qué libro buscas?")` → `libros.buscar {q}` → **Resultados** con lo dicho
   como cabezal; si la respuesta trae `corrected`, debajo del cabezal va "Buscando «…»" (por `s()`), y si trae
   `spelled`, "Deletreado: …" (y el cabezal es la palabra armada, no la ristra de letras).
   Sin resultados: "No encontré nada con «…»" y dos filas: **Deletrear el nombre** →
   `cp.listen(20, "Deletrea letra por letra")` → `libros.buscar {q = <lo dicho>, spelled = true}`, y **Buscar de
   nuevo** → dictar otra vez. Reintentar después de un fallo de red conserva el modo (deletreado sigue
   `spelled = true`); Atrás a Inicio lo apaga.
2. **Resultados**: filas con el título; OK → `libros.ficha {code}` → **Ficha**.
3. **Ficha**: título (UI_14), autor, "1967 · 345 páginas · Novela", las primeras líneas de la descripción, y
   las filas **Bajar EPUB** (una por formato conocido, epub primero; si no hay ninguno: "Este bot no da el
   archivo"), **Leer la descripción** (`cp.view`), **Otra búsqueda**.
4. **Bajando**: `libros.bajar` → `job.status` cada 3 s con `label` → `cp.download(file.id, file.name, "books")`
   → **Listo**: "Guardado en /Books/libros/<name>", filas **Abrir en el lector** (`cp.open_book`) y **Buscar
   otro**. Se anota en `bajados.json`. Error → pantalla de error con Reintentar y Volver.

Reglas de siempre: sin `pcall`; todo lo que viene de `cp.read`/`cp.load`/servidor pasa por `s()`/`n()`;
español neutro; escenario en `test/lua_sandbox/scenarios/libros.lua` (buscar con y sin resultados, ficha con
dos formatos y sin formato, bajar con `job.status` que va `running` → `done`, `cp.download`, abrir, la lista de
bajados tras `fake.reload`, `no_telegram`, sin red, resultados con `corrected`, sin resultados → Deletrear con
`spelled = true` y el cabezal "Deletreado: …" / "Buscando «…»", corrección sin resultados, y un `bajados.json`
incompleto).

## Web (`/board` → Ajustes → Avanzado → **Telegram**)

Tarjeta con el estado ("Conectado como Gonzalo · +52…" o "Sin conectar"), campos api id, api hash (no se
muestra el guardado; vacío = no tocar), teléfono, bot (`@usuario`), y los pasos: **Guardar** → **Enviar código**
→ campo código (+ contraseña si la pide) → **Entrar** → **Probar** (una búsqueda de prueba con lo que se escriba)
→ **Cerrar sesión**. Todo con `change()`-style: después de cada paso se vuelve a pedir `GET /api/board/telegram`.
La app id y el hash se sacan de https://my.telegram.org (Ajustes → API development tools); la tarjeta lo dice.
