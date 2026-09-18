# Plan: dos apps de Lua con modelo — Viajes y Librito (EPUB a medida)

Pedido del dueño (2026-09-18): *"Quiero dos app de LUA: una para viajes (calendario de viaje, lugar, documentos
tipo vouchers, guía con mejores lugares, restaurantes, reseñas, historia de la ciudad, todo lo que necesita una
mente curiosa) y otra que genere epubs de un tema en específico, profesional y guiada, para leer en unos 15
minutos. El token de Anthropic para esto se carga en la web, solo para las apps de Lua."*

Viajes ya existió como pantalla compilada y se sacó en 1.5.93 con la nota *"vuelve como app de Lua más
adelante"*. Éste es ese regreso, y con más cosas.

## Lo que hay hoy y lo que falta

Una app de Lua hoy es un archivo en `/Apps`, dibuja, lee botones y gestos, guarda 4 KB de estado y nada más
(`docs/ws397/APPS_LUA.md`). **No tiene red, no tiene micrófono, no tiene archivos y no abre el lector.** Las dos
apps pedidas necesitan las cuatro cosas. Así que el plan tiene una **fase 0 de firmware y servidor** que les da
a TODAS las apps de Lua esas cuatro puertas —con la misma disciplina del cajón: nada de URLs ni rutas libres—, y
después cada app es Lua puro en la tarjeta, que se puede corregir sin recompilar ni publicar firmware.

Reglas que mandan sobre todo el diseño:

- **Nunca hay teclado.** Lo que el usuario "escribe" entra por voz en el aparato, o se escribe **en la web desde
  el teléfono** (`/board`), que es donde uno tiene los vouchers y donde escribir un tema largo es cómodo.
- **Todo lo pesado en el servidor**: el modelo, la búsqueda, el PDF, armar el EPUB. El aparato baja archivos
  chicos y los muestra con los visores que ya tiene.
- **Lo que se genera se guarda en la tarjeta** y se lee sin WiFi: la guía, los vouchers, el EPUB.
- **Un cliente de Anthropic aparte para las apps**, con su clave propia y su propio tope de gasto.

## Fase 0 — Las puertas de las apps (firmware + servidor)

### En el servidor: `/api/apps/*` con clave propia

- `config.apps = { key, model, monthlyUsd }` en `server/src/config.ts`, editable en `/board` → Ajustes → Avanzado
  → **Apps de Lua** (clave que **no se devuelve nunca**, sólo `hasKey`, como las demás). Sin clave, las apps que
  la necesitan dicen en pantalla "carga la clave en la web" y no pasan de ahí.
- `server/src/appsLlm.ts`: un cliente de Anthropic **aparte** de `llm.ts` (`@anthropic-ai/sdk`, `claude-opus-5`
  por omisión para prosa larga, `claude-sonnet-5` seleccionable desde la web para gastar menos; pensamiento
  adaptativo; **streaming** siempre, porque un capítulo son minutos). Registra cada llamada en `usage.ts` bajo
  una cuenta separada (`apps`) para que el tope mensual de Hablar no se mezcle con esto.
- **Trabajos, no peticiones.** Generar una guía o un librito tarda de uno a cinco minutos y el aparato no puede
  quedarse colgado en un POST (el tope es 40 s y ya vimos lo que pasa). `POST /api/apps/job` devuelve un
  `jobId`; `GET /api/apps/job/:id` dice `estado`, `paso` ("capítulo 3 de 6"), y al terminar la lista de archivos
  para bajar. El aparato consulta cada 5 s con la pantalla viva, y si se va, la próxima vez que entre a la app
  retoma por el `jobId` guardado en su estado.
- `GET /api/apps/file/:id` entrega el archivo generado (texto, EPUB) tal cual, para bajarlo a la tarjeta.

### En el firmware: cinco funciones nuevas en `cp`

| Función | Qué hace | Por dónde pasa |
|---|---|---|
| `cp.listen(seg)` | Graba hasta `seg` segundos y devuelve el texto transcrito, o `nil` | `VoiceRecorder` + `SpeechToText`, los de siempre |
| `cp.call(servicio, tabla)` | Llama a **un servicio con nombre** del servidor (`viajes.guia`, `librito.indice`…) y devuelve una tabla | `ServerClient` con el Bearer del aparato; el nombre se valida en el servidor |
| `cp.download(id, nombre)` | Baja un archivo generado a la carpeta de la app | `HttpDownloader::downloadToFile`, el mismo que usa la OTA y las fuentes |
| `cp.files()`, `cp.read(nombre, desde, largo)`, `cp.write(nombre, texto)`, `cp.remove(nombre)` | Archivos **sólo** dentro de `/Apps/data/<app>/`, sin rutas, con tope de tamaño | `Storage` del SDK |
| `cp.view(nombre [, título])` | Abre un `.txt` de la app en el **visor paginado del sistema** (`DictionaryDefinitionActivity`) y vuelve a la app al salir | El visor que ya usan Hablar, la Biblia y las notas |
| `cp.open_book(nombre)` | Copia/abre un `.epub` de la app en el lector de CrossPoint | `ActivityManager` → `EpubReaderActivity`; al cerrar el libro se vuelve al hub, no a la app (el lector necesita el heap entero) |

Por qué así y no de otra forma:

- `cp.call` con **nombre de servicio** y no con URL: la app no elige a dónde va ni qué cabeceras manda; el
  servidor tiene una tabla de servicios y cada uno valida sus argumentos. Igual que el cajón de Lua deja
  `math` y no `os`.
- `cp.view` en vez de que la app pagine texto: el visor del sistema ya sabe partir párrafos, poner grises y
  buscar palabras; una guía de 30 KB no entra en los 192 KB de Lua con comodidad y no tiene sentido reescribir
  eso en cada app.
- WiFi: `cp.listen` y `cp.call` levantan la red con `FriendlyWifi` **la primera vez que hacen falta** y la dejan
  arriba mientras la app viva; al salir de la app, radio apagada. Las mismas guardias de Hablar (no reposar con
  red arriba, `preventAutoSleep` sólo mientras se graba o se espera).
- Tope de instrucciones: `cp.call` y `cp.download` son **síncronas y bloqueantes** desde el punto de vista de
  Lua pero no cuentan instrucciones mientras esperan (el guardián del `stepHook` se pausa en la llamada
  nativa). La espera larga (el trabajo de minutos) NO se hace adentro de una llamada: se hace con `on_tick`
  preguntando `cp.call("job", {id=…})` cada 5 s, así el loop, los recordatorios y el reposo siguen vivos.
- Todo esto se prueba **de escritorio** como el resto del cajón: `./test/lua_sandbox/run.sh` con un `cp` falso
  que devuelve respuestas grabadas, y las apps enteras corren su flujo de punta a punta sin placa.

Costo de flash estimado: 10-15 KB (todo son puentes a código que ya existe). Estamos en 87,2 %.

## Fase 1 — **Librito**: un EPUB a medida para leer en 15 minutos

Es la app más chica y valida el camino entero (voz → trabajo → archivo → lector), por eso va primera.

**Flujo en el aparato** (cada paso es una pantalla; Atrás vuelve; todo lo que se "escribe" es por voz):

1. **Tema** — "¿Sobre qué querés leer?" → `cp.listen(15)`. Muestra lo entendido; OK sigue, ARRIBA repite.
2. **Enfoque** — el servidor (`librito.enfoque`) devuelve **tres formas de encararlo** (ej. para "la peste negra":
   *cronología y causas* / *cómo se vivía* / *qué cambió después*) y una pregunta corta si el tema es ambiguo.
   La palanca elige; OK confirma; o se dicta otra cosa.
3. **Índice** — el servidor (`librito.indice`) propone **5 a 7 capítulos** con título y una línea cada uno,
   calibrados a **~3.000 palabras en total** (15 minutos a 200 palabras por minuto). La lista se recorre con la
   palanca; ABAJO largo sobre un capítulo lo saca; OK sobre "Cambiar…" dicta un ajuste ("agregá uno sobre los
   médicos de pico", "más corto") y el índice se rehace. OK sobre "Escribir" arranca.
4. **Escribiendo** — trabajo en el servidor; la pantalla muestra "Capítulo 3 de 6" y se puede salir; la app
   guarda el `jobId` y al volver retoma.
5. **Listo** — "Abrir en el lector" (`cp.open_book`) o "Guardar y volver". El archivo queda en
   `/Books/Libritos/<tema>.epub` para siempre; también aparece en el catálogo normal de Leer.

**Lo que hace el servidor** (`server/src/librito.ts`):

- Tres servicios chicos y síncronos (`enfoque`, `indice`, `ajustar`) con salida estructurada (JSON Schema por
  `output_config`, como el clasificador de voz).
- Un trabajo `escribir`: un capítulo por llamada, con **el índice entero y el resumen de los capítulos ya
  escritos** en el system prompt para que no se repita ni se contradiga; prosa de divulgación seria, en el idioma
  del aparato, sin listas ni markdown, con fuentes generales al final ("Para seguir leyendo"). Largo pedido por
  capítulo = 3.000 / N palabras.
- Armado del EPUB **a mano** (es un zip con `mimetype`, `container.xml`, `content.opf`, `toc.ncx`, un XHTML por
  capítulo y una portada de texto): sin dependencias nuevas, `Bun.zip`/`fflate`. Metadatos: título, autor
  "Librito · <fecha>", idioma. Se valida con el propio parser del lector antes de anunciar el archivo.
- Tamaño: 3.000 palabras son ~20 KB de texto, ~12 KB comprimido. Baja en un segundo.

**Lo mismo desde la web, opcional pero barato**: `/board` → Libritos: escribir el tema en el teléfono, ver el
índice, tocar "Escribir", y el aparato lo baja en la próxima sincronización. El diálogo es el mismo; cambia el
teclado por el micrófono. Sirve para temas largos y para hacerlo con el aparato en la mochila.

## Fase 2 — **Viajes**

Tres cosas distintas que la app junta: **la agenda del viaje**, **los papeles** y **la guía**. Las dos primeras
se cargan desde el teléfono, la tercera la escribe el modelo.

### El viaje en la web (`/board` → Viajes, vuelve como pestaña)

- Alta: destino (buscador de lugares, el mismo del clima), fechas, notas. Varios viajes; uno "activo".
- **Agenda por día**: vuelos, hoteles, trenes, reservas, visitas. Cada ítem tiene día, hora, título, lugar,
  código de reserva y **un papel adjunto opcional**.
- **Papeles**: se sube el PDF, la foto o el correo del voucher. El servidor **lo lee y lo convierte a texto
  estructurado** con el modelo (aerolínea, vuelo, terminal, hora, localizador, dirección del hotel, check-in,
  política de cancelación, teléfono) y guarda **eso**: en tinta electrónica de 480 px una foto de un voucher es
  ilegible, el texto no. El original queda en el volumen del servidor por si hay que mostrarlo en el teléfono.
  Con el texto extraído además **se completa la agenda solo**: subir el voucher del vuelo crea el ítem del vuelo
  con su hora. Lectura de PDF con `pdf-parse` (puro JS; nada de `mupdf`/`sharp`, que se sacaron en 1.5.93 por
  algo) y para fotos, el modelo con visión directamente sobre la imagen.
- Los ítems con hora **se espejan como eventos del calendario** del aparato (ya existe `calendar.ts`), así el
  hub y "Mi día" dicen "hoy 14:30 vuelo a Lima" sin abrir la app, y un recordatorio suena dos horas antes si se
  pide. Marcados con `tripId`, y esta vez con dueño: se borran con el viaje.

### La guía (trabajo en el servidor, `viajes.guia`)

Se arma **una vez por viaje** (y se puede rehacer), en secciones que son archivos de texto sueltos, uno por tema,
para que en el aparato se abran de a uno con `cp.view` y no haya que bajar 200 KB de golpe:

| Sección | Qué lleva |
|---|---|
| Para entender el lugar | Historia de la ciudad en tres o cuatro épocas, qué la hizo lo que es, cómo se llama la gente, qué idioma, qué moneda, cómo se saluda |
| Barrios | Qué barrio es qué, dónde dormir, dónde no ir de noche |
| Imperdibles | Lo que hay que ver sí o sí y **por qué**, con el dato curioso que uno cuenta después |
| Para una mente curiosa | Lo que no está en las guías: personajes, leyendas, el edificio raro, la historia que nadie cuenta |
| Comer | Platos del lugar y dónde comerlos, con **reseñas recientes**; horarios raros; qué se pide y qué no |
| Moverse | Aeropuerto → centro, transporte, tarjeta, taxis, apps, propinas |
| Ojo con | Estafas típicas, zonas, clima de esas fechas, feriados que caen en el viaje |
| Un día perfecto | Un itinerario a pie para el primer día |

- **Reseñas y "recientes" salen de la búsqueda web** (la herramienta `web_search` de Anthropic, con tope de
  usos por sección). Es la única app donde la búsqueda va **encendida sin que el usuario diga "busca"**, porque
  la guía se pide una vez y sin datos recientes los restaurantes están cerrados. Se anota como excepción a la
  regla del dueño, y con su costo a la vista en la web (unas 10-15 búsquedas por guía).
- La guía se genera en el idioma del aparato y **con las fechas del viaje** (feriados, clima esperado, qué
  estación es).
- Se guarda en el servidor y se baja al aparato en `/Apps/data/viajes/<viaje>/<seccion>.txt`.

### La app en el aparato (`viajes.lua`)

- Pantalla 1: **el viaje activo**: destino, "faltan 12 días" o "día 3 de 9", y tres entradas: Agenda · Papeles ·
  Guía. Con más de un viaje, ABAJO largo cambia.
- **Agenda**: lista por día con la hora y el título; OK abre el ítem con su papel si lo tiene (`cp.view`).
- **Papeles**: todos los vouchers del viaje, a texto, ordenados por fecha. OK abre. Es lo que se mira en el
  mostrador del aeropuerto, y funciona sin WiFi.
- **Guía**: las ocho secciones; OK abre en el visor. La primera vez, si no está bajada, la baja (barra de
  progreso, una sección por vez).
- **Preguntar por voz** (Atrás largo en cualquier pantalla): "¿a qué hora es el check-in?" o "¿qué como cerca del
  hotel el martes?" → `viajes.preguntar` con la guía y la agenda del viaje en el contexto del modelo (cacheado
  con `cache_control`, como el capítulo del libro en Preguntarle al libro). Respuesta en el visor y hablada
  con Piper, como Hablar.
- Sincronización: al entrar a la app con WiFi, `viajes.estado` devuelve qué cambió (ítems, papeles, guía) y se
  baja sólo lo nuevo, por sha, como el paquete de noticias.

## Orden, tamaño y qué se puede probar sin aparato

| Paso | Qué | Dónde se prueba |
|---|---|---|
| 0a | Clave y tope de las apps en la web; `appsLlm.ts`; trabajos (`/api/apps/job`) | `bunx tsc`, servidor local con clave de prueba |
| 0b | `cp.listen`, `cp.call`, `cp.download`, archivos, `cp.view`, `cp.open_book`; doc en `APPS_LUA.md` | `./test/lua_sandbox/run.sh` con `cp` falso; lo real, en el aparato |
| 1 | `librito.ts` + `librito.lua` + pestaña Libritos en la web | EPUB validado con el parser del lector de escritorio; flujo entero en el sandbox |
| 2a | Viajes en la web: alta, agenda, papeles con extracción a texto, espejo en calendario | Playwright a 360 px como el resto de `/board`; extracción con PDFs de muestra |
| 2b | `viajes.guia` con búsqueda; `viajes.lua` | Guía real generada en Railway y leída de escritorio antes de tocar la app |
| 2c | Preguntar por voz dentro del viaje | Sandbox con respuestas grabadas |

Cada paso termina en un release y una prueba en el aparato **antes** del siguiente: la fase 0 es la que más
puede morder (red y archivos desde Lua) y conviene verla andar con el librito, que es chico, antes de meterle
los vouchers.

## Decisiones que hay que tomar antes de empezar (con mi recomendación)

1. **Los vouchers se guardan como texto extraído, no como imagen.** En el aparato se lee mejor y no hace falta
   volver a meter `sharp`/`mupdf`. El original queda en el servidor para el teléfono. **Recomiendo texto.**
2. **La guía busca en internet sola** (reseñas, horarios, cierres) aunque la regla general sea "sólo con
   busca". Cuesta unos centavos por guía. **Recomiendo sí, sólo en la guía**, con el costo a la vista en la web.
3. **Modelo para la prosa**: `claude-opus-5` escribe mejor y cuesta más; `claude-sonnet-5` alcanza para la guía.
   **Recomiendo Opus para los libritos y Sonnet para las guías**, ambos cambiables desde la web.
4. **Al abrir el EPUB desde la app, al cerrar el libro se vuelve al hub y no a la app**: el lector necesita el
   heap entero y hoy funciona así con Preguntarle al libro. Se puede vivir con eso.
5. **Largo del librito**: 15 minutos ≈ 3.000 palabras, 5-7 capítulos. Si se quiere "corto / normal / largo",
   es una pregunta más en el flujo. **Recomiendo fijo en 15 minutos** para la primera versión.

---

## Decisiones tomadas (2026-09-18)

1. Vouchers como **texto** extraído. 2. La guía **busca en internet** sola. 3. Opus 5 para libritos, Sonnet 5
para guías, cambiable desde la web. 4. Al cerrar el EPUB se vuelve al hub. 5. Largo **mínimo 15 minutos**; el
usuario **elige los capítulos** (los saca, los agrega, pide más temas) antes de escribir.

## Contrato v1 (fase 0 + Librito) — lo que firmware, servidor y app tienen que cumplir

### `cp` — lo nuevo (todo lo que espera es ASÍNCRONO: se pide y llega por callback)

| Función | Devuelve | Qué pasa |
|---|---|---|
| `cp.listen(seg [, pregunta])` | `true` si empezó | El firmware muestra su pantalla de escucha con `pregunta` arriba (pitido, "OK termina · Atrás cancela"), graba hasta `seg` s, transcribe por el servidor y llama **`on_heard(texto)`** (`nil` si canceló o falló). Después repinta. |
| `cp.call(servicio, args)` | `id` (entero) o `nil` si ya hay 4 en vuelo | `POST /api/apps/call` `{app, service, args}` con el Bearer del aparato y `?lang=`. Llama **`on_reply(id, ok, tabla)`**: `tabla` es el JSON del servidor (con `ok=false`, `tabla.error` es el texto). Después repinta. |
| `cp.download(fileId, nombre [, destino])` | `id` o `nil` | Baja `GET /api/apps/file/<fileId>` a la carpeta de la app (`destino="app"`, por omisión) o a `/Books/<App>/` (`destino="books"`). **`on_reply(id, ok, {bytes=n})`**. |
| `cp.busy()` | `true`/`false` | Hay una escucha, llamada o descarga en curso. Mientras, la app puede dibujar "esperando". |
| `cp.files()` | tabla de nombres | Sólo `/Apps/data/<app>/`. |
| `cp.read(nombre [, desde, largo])` | texto o `nil` | Entero si entra en 48 KB; con rango para archivos grandes. |
| `cp.write(nombre, texto)` | `true`/`false` | Tope 64 KB, escritura atómica (`.tmp` y renombrar). |
| `cp.remove(nombre)`, `cp.size(nombre)` | | |
| `cp.view(nombre [, titulo])` | `true` si existía | Abre el `.txt` en el visor paginado del sistema; al salir, vuelve a la app y repinta. |
| `cp.open_book(nombre)` | `true` si existía | Abre `/Books/<App>/<nombre>` en el lector. **La app se cierra**; al cerrar el libro se vuelve al hub. |

- `<App>` = nombre del archivo sin `.lua` (`librito.lua` → `/Apps/data/librito/`, `/Books/librito/`).
  Los nombres de archivo que pasa la app: `[A-Za-z0-9._-]{1,48}`, sin punto inicial, sin barras.
- WiFi: `cp.listen`, `cp.call` y `cp.download` levantan la red con `FriendlyWifi` la primera vez (pantalla de
  conexión del sistema si tarda) y la dejan arriba hasta que la app se cierra. Sin red guardada → el selector.
  Sin token del aparato → `on_reply(id, false, {error="sin vincular"})`.
- Mientras hay algo en curso (`cp.busy()`), `on_key` **no se llama** salvo para `back`, que cancela lo que está
  en curso y llama a `on_heard(nil)` / `on_reply(id, false, {error="cancelado"})`. `on_tick` sigue.
- Las llamadas son **de a una**: se encolan hasta 4 y salen en orden. El tope de instrucciones de Lua no corre
  mientras el firmware espera a la red (la espera es del host, no del script).
- Todo lo de red lo hace el **host** (`LuaAppsActivity`) desde su `loop()`, nunca desde el worker de Lua (32 KB
  de stack; TLS no entra ahí).

### Servidor — `/api/apps/*` (Bearer del aparato, como todo `/api`)

- `POST /api/apps/call?lang=xx` `{app, service, args}` → JSON `{ok:true, …}` o `{ok:false, error}` (HTTP 200
  igual; 4xx sólo si falta el Bearer o el cuerpo es ilegible). **Toda respuesta síncrona en menos de 25 s** (el
  aparato corta a los 40). Lo que tarde más es un trabajo.
- Servicio común `job.status {id}` → `{ok, state:"running"|"done"|"failed", step, total, label, files:[{id,
  name, bytes}], error}`. Trabajos en `/data/apps-jobs.json` por cuenta, se podan a las 24 h; archivos en
  `/data/apps-files/<cuenta>/<id>`.
- `GET /api/apps/file/:id` → el archivo (Content-Type por extensión; `.epub` = `application/epub+zip`).
- Config: `config.apps = { key, model }` (`model` por omisión `claude-opus-5`; se elige desde la web entre
  `claude-opus-5` y `claude-sonnet-5`). La clave **no se devuelve** (`hasKey`). Sin clave, todo servicio que la
  necesite contesta `{ok:false, error:"Carga la clave de las apps en la web (Ajustes → Apps de Lua)"}`.
  Cliente propio en `server/src/appsLlm.ts` (`@anthropic-ai/sdk`, `thinking: {type:"adaptive"}`, streaming
  para prosa larga, `output_config.format` json_schema para lo estructurado). Contabiliza en `usage.ts` bajo
  `apps`.

### Servicios del Librito (`server/src/librito.ts`)

| Servicio | args | Devuelve |
|---|---|---|
| `librito.enfoque` | `{tema}` | `{ok, tema, enfoques:[{id, titulo, linea}]}` — tres formas de encarar el tema (`tema` viene normalizado: título corto) |
| `librito.indice` | `{tema, enfoque, minutos?}` | `{ok, titulo, minutos, capitulos:[{n, titulo, linea, palabras}]}` — 5 a 8 capítulos; `minutos` ≥ 15 (200 palabras/min) |
| `librito.ajustar` | `{tema, enfoque, capitulos:[{titulo, linea, activo}], pedido, minutos?}` | Igual que `indice`, con el pedido dictado aplicado ("agregá uno sobre…", "más temas", "más corto el 3"). Los inactivos se descartan y el resto se recalibra para que el total **no baje de 15 minutos** |
| `librito.escribir` | `{titulo, tema, enfoque, capitulos:[{titulo, linea, palabras}], minutos}` | `{ok, jobId}`; el trabajo escribe capítulo por capítulo (system: índice entero + resumen de lo ya escrito, prosa de divulgación seria, sin listas ni markdown, en el idioma del aparato) y arma el EPUB a mano (`mimetype`, `container.xml`, `content.opf`, `toc.ncx`, un XHTML por capítulo, portada de texto, "Para seguir leyendo" al final). `files:[{id, name:"<slug>.epub", bytes}]` |

### Prueba de escritorio (contrato entre el harness y las apps)

`test/lua_sandbox/test_sandbox.cpp` provee un `cp` falso completo. Un escenario es un archivo
`test/lua_sandbox/scenarios/<app>.lua` que corre DESPUÉS de cargar `examples/Apps/<app>.lua` y puede:
- `fake.heard = "texto"` → la próxima `cp.listen` llama a `on_heard` con eso en el siguiente `fake.step()`;
- `fake.reply["librito.enfoque"] = function(args) return true, {…} end` → la próxima `cp.call` de ese servicio
  llama a `on_reply` con eso en el siguiente `fake.step()`; `fake.reply["job.status"]` puede devolver distinto en
  cada llamada (closure con contador);
- `fake.key("ok")`, `fake.tick()`, `fake.step()` (entrega lo pendiente), `fake.draw()` (corre `on_draw` y
  devuelve el texto dibujado como lista de strings) y `assert`.
Los archivos van a un directorio temporal. El harness corre todos los escenarios y falla si alguno lanza.
