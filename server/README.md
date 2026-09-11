# Servidor del hub ws397 (Bun + Hono, Railway)

Todo lo que el aparato necesita del lado del servidor, en un solo lugar: OTA del firmware, preguntas al
libro, transcripción, hub (clima, agenda, recordatorios), voz con clasificador de intención y el store de
recordatorios, las dos listas (compras y tareas) y notas.

Anda de dos formas, y la que corre la decide **una sola variable**: sin `DATABASE_URL` es el servidor de un solo
usuario de siempre (archivos en `/data`, un `DEVICE_TOKEN`, sin login), y con `DATABASE_URL` es multiusuario, con
cuentas de correo y contraseña y aparatos vinculados por un código de 6 dígitos. Ver **Multiusuario** más abajo.

## Railway

- **Root Directory**: `server` (Settings → Source → Root Directory). Con eso Railway solo mira esta carpeta.
- **Build**: hay `Dockerfile` (Railway lo usa solo): Bun + Piper con las seis voces (~400 MB de imagen). Start = `bun run src/index.ts`.
- **Volumen** montado en `/data` (firmware subido, `store.json`, `calendar.json`, `hub-settings.json`, `hub-data.json`).
- Variables:

| Variable | Para qué |
|---|---|
| `OTA_TOKEN` | Bearer que usa `release.sh` / `release.ps1` para subir el `.bin` (`PUT /firmware`). |
| `DEVICE_TOKEN` | Bearer del aparato para todo `/api/*` (web UI del aparato → Servidor → token). |
| `ANTHROPIC_API_KEY` | Claude (preguntas y clasificador de voz). Nunca va al aparato. |
| `STT_API_KEY` (o `OPENAI_API_KEY`) | Transcripción por un endpoint compatible con OpenAI. |
| `STT_BASE_URL`, `STT_MODEL` | Opcionales. Groq: `https://api.groq.com/openai/v1` + `whisper-large-v3-turbo`. |
| `ASK_MODEL`, `VOICE_MODEL` | Opcionales, default `claude-haiku-4-5`. |
| `WEB_SEARCH`, `SEARCH_PROVIDER`, `SEARCH_API_KEY` | Búsqueda en internet (`0` la apaga). Ver más abajo. |
| `HUB_LAT`, `HUB_LON`, `HUB_TZ` | Respaldo del clima mientras no se elija lugar por voz desde el aparato. |
| `HUB_LANG` | Idioma cuyo Piper se precalienta al arrancar (default `es`). `TTS_ENABLED=0` apaga la voz. |
| `HUB_ICS_URL` | Calendario(s) ICS para la agenda del hub (URL secreta iCal de Google, Apple, Outlook...), separados por coma. |
| `TRIPS_FILE`, `ATTACHMENTS_DIR` | Dónde viven los viajes y los adjuntos (defaults `/data/trips.json` y `/data/attachments`). |
| `ASSETS_DIR` | Dónde se guarda el paquete de contenido (default `/data/assets`). `ASSETS_BUILD=0` no lo genera al arrancar. |
| `NOTO_EMOJI_REF` | Rama o tag de [noto-emoji](https://github.com/googlefonts/noto-emoji) de donde salen los dibujos de las tarjetas (default `main`). |
| `STT_MIN_SECONDS`, `STT_MIN_PEAK`, `STT_MIN_RMS` | Mínimos de audio para considerar que alguien habló (defaults `0.4`, `350`, `90`). Dependen de la ganancia del micrófono. |
| `DATABASE_URL` | **Enciende el modo multiusuario** (Postgres). Sin ella, todo sigue como siempre: archivos en `/data` y un solo `DEVICE_TOKEN`. |
| `ADMIN_EMAIL` | Correo de la cuenta de administrador que se crea al migrar, y la única que puede tocar la pestaña IA. |
| `SESSION_SECRET` | Clave con la que se firman las cookies de sesión. Si no está, se genera una y se guarda en la base. |
| `MONTHLY_LLM_CALLS`, `MONTHLY_STT_SECONDS` | Topes mensuales por cuenta. Sin poner = sin tope. |
| `ACCOUNTS_DIR` | Dónde viven las fotos, los adjuntos y el log de las cuentas nuevas (default `/data/accounts`). |

## Rutas

| Ruta | Quién | Qué |
|---|---|---|
| `GET /firmware/latest` | aparato (sin token) | JSON con forma de release de GitHub: `tag_name`, `assets[firmware-ws397.bin]`. |
| `GET /firmware/firmware-ws397.bin` | aparato | El binario. |
| `PUT /firmware` | `release.sh` (Bearer `OTA_TOKEN`, `X-Version`) | Sube un binario nuevo. |
| `GET /board` | teléfono | Página web: calendario, recordatorios, listas, notas, fotos, noticias, viajes y ajustes. Pide el token del aparato una vez. |
| `POST /api/board/{reminder,item,note}` | página web | Altas desde la página. |
| `GET /api/ping` | aparato | Prueba del token. |
| `POST /api/ask` | aparato | Pregunta sobre el libro (`text`) o general (sin `text`). `lang` = idioma de la UI. |
| `POST /api/transcribe?lang=xx` | aparato | WAV → texto en el idioma de la UI. |
| `GET /api/hub?lang=xx` | aparato | Clima, recordatorios, las dos listas, agenda, notas y frase, en el idioma de la UI. |
| `GET /api/hub/location/search?q=`, `POST /api/hub/location` | aparato | Lugar del clima por voz. |
| `POST /api/voice?lang=xx` | aparato | Una grabación: transcribe, clasifica la intención y ejecuta. Devuelve JSON + voz Piper (ADPCM) en un cuerpo binario. |
| `GET /api/tts?text=&lang=xx` | aparato | Voz Piper en ADPCM 16 kHz para los avisos que el aparato guarda en la SD. |
| `GET /api/bible/{books,chapter,day,find}?lang=xx` | aparato | Biblia por capítulos (cacheados en la SD), versículo del día, búsqueda por referencia o texto (sin LLM). |
| `POST /api/bible/ask` | aparato | Preguntar sobre el capítulo que se está leyendo. Ver más abajo. |
| `POST /api/notes` | aparato | Nota rápida, sin clasificador de intención ni LLM. Ver más abajo. |
| `GET /api/hub/forecast?lang=xx` | aparato | Pronóstico: ahora, por horas y seis días (pantalla de Clima). |
| `GET /api/photos`, `GET /api/photos/file?id=` | aparato | Álbum: BMP de 2 bpp (4 grises) que el navegador convierte al subirlos desde `/board`. |
| `GET /api/rss`, `GET /api/rss/article?feed=&item=` | aparato | Noticias de los feeds RSS/Atom cargados en `/board`; artículo limpiado a texto. |
| `POST /api/translate?from=xx&to=yy` | aparato | Traductor en conversación: WAV en `from` → texto, traducción y voz en `to` (cuerpo binario como `/api/voice`). |
| `POST /api/hub/reminder` | aparato / web | Alta y **edición** de un recordatorio: título, fecha, hora y repetición. |
| `GET /api/calendar?from=&to=&lang=` | aparato | Calendario local del rango con las repeticiones expandidas + resumen por día. |
| `GET /api/calendar/day?date=&lang=` | aparato | El día completo. |
| `POST /api/calendar/event`, `/event/delete` | aparato / web | Alta, edición (con `id`) y borrado de un evento. |
| `POST /api/calendar/dictate` | aparato | Cargar el día entero dictándolo por el micrófono. Ver más abajo. |
| `GET /api/calendar/repeat?...` | web | La repetición en una línea, para mostrarla mientras se edita. |
| `POST /api/hub/done` | aparato | `{kind: "reminder"\|"item", id, snooze?}` marca hecho o pospone (también desde la cola offline). |
| `POST /api/hub/edit` | aparato | Mover, poner fecha o borrar un ítem de lista; borrar una nota. |
| `GET /api/trips?lang=xx`, `GET /api/trip?id=` | aparato | Viajes: la lista, y un viaje entero con sus días, sus ítems y sus adjuntos. |
| `POST /api/trip*` | aparato / web | Crear y editar viaje, día, ítem, lista de para llevar y adjuntos colgados. |
| `GET /api/attachment?id=&page=` | aparato | Una página del adjunto ya convertida: BMP de 2 bpp de 480x800 (96 KB), igual que las fotos. |
| `GET /api/attachment/info?id=` | aparato / web | Qué se extrajo del adjunto: campos, códigos y páginas. |
| `POST /api/board/attachment?trip=` | teléfono | Sube el PDF del vuelo o del hotel tal como llegó; el servidor lo convierte y contesta qué encontró. |
| `GET /api/assets/manifest?lang=xx` | aparato | Paquete de contenido: todo lo descargable con su tamaño y su sha. |
| `GET /api/assets/file?id=` | aparato | Un archivo del paquete, con `Range` para reanudar. |
| `GET /api/assets/status`, `POST /api/assets/build` | web | Cómo va la generación del paquete y cómo forzarla. |
| `GET /api/board/costs` | web | Cuánto sale cada consulta con cada modelo (tarjeta de la pestaña IA). |
| `POST /auth/register`, `/auth/login`, `/auth/logout` | web | Cuentas de la web. Solo con `DATABASE_URL`. |
| `GET /auth/me` | web | Si el servidor tiene cuentas, quién soy y qué aparatos tengo. |
| `POST /api/pair/start` | aparato (**sin** token) | Pide el código de 6 dígitos para vincularse. |
| `GET /api/pair/status` | aparato | Si ya lo vincularon y a qué cuenta. |
| `POST /api/account/pair` | web (sesión) | Vincula el aparato del código a mi cuenta. |
| `POST /api/account/device/rename`, `/device/delete` | web (sesión) | Renombrar y desvincular un aparato. |
| `GET /api/account/devices`, `POST /api/account/password` | web (sesión) | Mis aparatos y cambiar mi contraseña. |

### Voz: silencio y alucinaciones

Whisper **nunca** devuelve vacío: con una grabación muda inventa la frase que más vio en los subtítulos
con los que lo entrenaron ("Subtítulos realizados por la comunidad de Amara.org", "Gracias por ver el
video", "Thanks for watching", "♪"). El aparato abría Hablar, el usuario no decía nada y el asistente
contestaba algo sobre amara.org. Por eso hay dos filtros en `transcribe.ts`:

1. **Antes** de llamar al STT se mide la energía del WAV por tramos de 20 ms (`hasSpeech`): se descarta
   lo que dure menos de `STT_MIN_SECONDS`, lo que no llegue a `STT_MIN_PEAK` de amplitud o lo que no
   tenga al menos 120 ms de tramos por encima del piso de ruido. Un cuarto en silencio con zumbido
   tampoco pasa: se mira el contraste contra el piso, no solo el volumen.
2. **Después** se compara el texto contra una lista de frases inventadas en los seis idiomas
   (`isHallucination`), incluidas las cadenas de varias seguidas y las repeticiones.

En los dos casos vuelve un **200** con `text: ""`, `code: "no_speech"` y `error` con el mensaje ya
traducido ("No escuché nada, inténtalo de nuevo"), para que el aparato lo muestre en vez de un error
genérico. `/api/voice` y `/api/translate` devuelven ese mismo mensaje como `reply` y lo dicen por el
parlante; nunca le preguntan nada al modelo con una frase inventada.

### Noticias: juego de caracteres y por qué falla una nota

`res.text()` decodifica **siempre** como UTF-8, y medio diario latinoamericano sirve el RSS en
iso-8859-1 (Reforma manda `<?xml encoding="iso-8859-1"?>` con un `Content-Type` sin charset). Los bytes
inválidos se descartaban y el titular llegaba al aparato **sin tildes**: "Exhibe PISA fracaso educativo
en Mxico", "Nominan a Quiones", "Baln de Oro". Ahora el cuerpo se baja como bytes y `decodeBody`
(`net.ts`) elige el juego de caracteres mirando, en orden: BOM, `charset` del `Content-Type`, la
declaración del documento (`<?xml encoding>` o `<meta charset>`) y, de desempate, si los bytes son UTF-8
legal. El `TextDecoder` de Bun no trae iso-8859-15, windows-1251 ni koi8-r, así que esos van por tabla.
Se aplica al feed y al artículo.

`GET /api/rss/article` ahora **siempre** devuelve algo legible y dice por qué, en el idioma del aparato
(`&lang=xx`): `{ ok, title, text, when, source, reason, cache }`, con
`reason` = `""` | `paywall` | `blocked` | `notfound` | `timeout` | `empty` | `down` | `stale`. Cuando
`cache` es `false`, `text` es una explicación y **no** hay que guardarla en la SD como si fuera la nota.
El pedido entero tiene un presupuesto de 14 s (el `ServerClient` del aparato corta a los 20 s): antes se
podían encadenar 12 s de feed más dos intentos de 12 s de la nota y el aparato mostraba "No se pudo
obtener respuesta" sin que nada estuviera roto.

Idiomas soportados (`lang`): `es`, `en`, `fr`, `de`, `pt`, `ru`. El aparato manda el que tiene en Settings;
la transcripción escucha en ese idioma, las respuestas salen en ese idioma y el traductor traduce desde ese
idioma al que se pida (si no se dice, al inglés; desde inglés, al español).

## Listas: son dos, y la pizarra de mensajes ya no existe

Dos cambios que sacan cosas del producto, los dos pedidos por el usuario.

**Se fueron los mensajes.** La pizarra ("dejale un mensaje al hub") se sacó entera: no está el tipo
`Message` ni el campo `messages` del store, `GET /api/hub` ya **no manda la clave** `messages` (no viaja
vacía: no viaja), `POST /api/hub/done` y `POST /api/hub/edit` ya no aceptan `kind: "message"`,
`POST /api/board/message` devuelve 404 y la Pizarra de `/board` no tiene el formulario ni el listado.
En `voice.ts` desapareció la intención `message` del esquema, del prompt y del ejecutor; si un modelo
igual la devuelve, **se guarda como nota** en vez de perderse. Un `store.json` viejo con `messages`
adentro se sigue leyendo sin problemas: esa clave se ignora al cargar y no vuelve a escribirse.

**Las listas son dos y no se pueden crear más**: la de **compras** y la de **tareas** (to-do). Las
categorías de antes (Entrada, Casa, Trabajo, Administrativo y los proyectos sueltos) desaparecieron.

- La **clave guardada** es siempre el nombre canónico en español (`"Compras"` y `"Tareas"`,
  `SHOPPING_LIST` / `TASK_LIST` en `store.ts`): así cambiar el idioma del aparato no renombra las listas
  ni deja los ítems huérfanos. El **nombre visible** sale de `LABELS` de `lang.ts` según el idioma
  (`listLabel()`), o sea Compras/Tareas, Shopping/Tasks, Courses/Tâches, Einkäufe/Aufgaben,
  Compras/Tarefas, Покупки/Задачи.
- Cada lista de `GET /api/hub` viaja con `key` (la canónica, que es lo que hay que devolver al mover un
  ítem) y `name` (la que se muestra).
- `resolveList()` ya no crea nada: cualquier nombre que llegue —del modelo, del aparato en otro idioma o
  de la web— se resuelve a una de las dos. Se va a compras solo si el nombre tiene una palabra de compra
  en alguno de los seis idiomas (compras, súper, mercado, shopping, groceries, courses, Einkauf,
  покупки…); **todo lo demás cae en tareas**.
- El prompt de `voice.ts` se lo dice al modelo con todas las letras: hay dos listas, no invente otras.
- **Migración automática al leer `store.json`**: los ítems **no hechos** de las listas viejas se vuelcan
  en orden a Tareas y las listas viejas se borran del archivo. No se pierde nada; lo ya tildado no se
  arrastra. Probado con un `store.json` del formato viejo (Entrada, Casa, Trabajo, Administrativo,
  Compras y un proyecto suelto): quedan los cinco pendientes en Tareas, la leche en Compras, y los
  recordatorios, las notas y las memorias intactos.
- En `/board` la pestaña Listas ya no crea ni borra listas: muestra las dos y deja agregar y tildar
  ítems. `POST /api/board/list` y `POST /api/board/list/delete` se fueron.

## `POST /api/notes` — nota rápida sin clasificador

Una nota no necesita que un modelo decida qué es: hasta ahora toda nota entraba por `/api/voice`, o sea
que había que esperar la llamada al LLM (el "Pensando…" que se hacía eterno para guardar un párrafo).
Ahora el aparato transcribe con `/api/transcribe` y manda el texto derecho acá; **no se llama a ningún
modelo**.

```
POST /api/notes            (Bearer del aparato)
{ "text": "...", "lang": "es" }
→ 200 { "ok": true, "id": 34 }
   400 { "ok": false, "error": "text required" }   // texto vacío
```

Las notas pueden ser **largas**: hasta 20.000 caracteres, y lo que pase de ahí se corta con puntos
suspensivos en vez de rebotar el pedido. El mismo tope vale para `POST /api/board/note` desde la web.

**Y el tope del cuerpo de `/api/transcribe` subió a 4 MB** (`MAX_BYTES` en `transcribe.ts`). Estaba en
2 MB, y como el ADPCM se topea en `MAX_BYTES/4`, una grabación de 90 s en ADPCM (~720 KB) rebotaba con
`audio too large` antes de llegar al STT: dictar una nota larga era imposible. Con 4 MB entran unos dos
minutos de ADPCM y unos dos de WAV.

## `POST /api/calendar/dictate` — cargar el día dictándolo

"A las 8 gimnasio, a las 9 reunión con Ana, a las 13 almuerzo", de un tirón por el micrófono.

```
POST /api/calendar/dictate            (Bearer del aparato)
{ "text": "a las 8 gimnasio, a las 9 reunión con Ana, a las 13 almuerzo",
  "date": "2026-09-10",               // opcional: sin él, hoy en la zona del lugar guardado
  "lang": "es" }
→ 200 { "ok": true,
        "added": [ { "id": 12, "start": "2026-09-10T08:00", "title": "Gimnasio", "allDay": false } ],
        "reply": "Cargué 3 actividades." }
   400 { "ok": false, "error": "text required" }
   5xx { "ok": false, "error": "...", "code": "no_key" | "provider_error" | ... }
```

- El parseo lo hace el modelo configurado con salida estructurada (`chatJson`): devuelve
  `{ items: [{ time: "HH:MM"|null, endTime: "HH:MM"|null, title }] }` en el idioma de `lang`.
- Sin hora → **evento de todo el día** (`start` es la fecha sola y `allDay: true`).
- `end` = `start` salvo que el modelo haya entendido una hora de fin ("de 8 a 9", "hasta las 10").
- Cada actividad se guarda como un `CalEvent` de `calendar.ts` con las mismas funciones que el resto
  (`mutate` + `nextEventId`): un solo leer-modificar-escribir para todas, porque el archivo lo escribe
  también el módulo de viajes y una escritura por renglón es una carrera por renglón.
- Tope de 40 actividades por dictado y 4.000 caracteres de texto.
- `reply` es una frase corta en el idioma pedido, para leerla por el parlante; la arma el servidor con
  una plantilla (incluidos los plurales del ruso), no el modelo: es una línea con un número y no vale
  otra llamada.
- **Editar y borrar son los de siempre**: `POST /api/calendar/event` con `id` edita el evento en su
  lugar (404 si el id no existe; probado que no duplica) y `POST /api/calendar/event/delete` lo borra.

## `POST /api/bible/ask` — preguntar sobre lo que se está leyendo

"No entendí del versículo 8 al 12", "¿qué dijo el capítulo?", "¿qué significa esa palabra?".

```
POST /api/bible/ask            (Bearer del aparato)
{ "book": "Juan", "chapter": 4,
  "text": "<el capítulo entero, versículos numerados>",
  "question": "no entendí del versículo 8 al 12",
  "lang": "es" }
→ 200 { "ok": true, "answer": "..." }
   400 { "ok": false, "error": "question is required" }
```

El capítulo va en el bloque `cached` de `llm.ts` — con Anthropic es un bloque de system con
`cache_control`, igual que en `ask.ts`—, así que preguntar tres cosas seguidas sobre el mismo capítulo
no paga tres veces la entrada. Nunca busca en internet (`search: "off"`).

El prompt: explicar el pasaje con claridad y respeto, apoyándose en lo que dice el texto y en el
contexto histórico; si preguntan por un rango de versículos, contestar sobre esos y nombrarlos; si
preguntan qué significa una palabra, explicarla en ese contexto; **no empujar la interpretación de
ninguna iglesia ni corriente** —cuando hay lecturas distintas se dice en una línea sin tomar partido— y
decir que algo no está en el capítulo en vez de inventarlo. Texto plano, entre 6 y 10 líneas, en el
idioma de `lang`.

## Recordatorios que se repiten y calendario local

El aparato tiene que poder **ver y cambiar** cuándo lo va a despertar un recordatorio ("¿mañana y
pasado, o de lunes a viernes?"). Para eso la repetición dejó de ser una cadena suelta
(`"none"|"daily"|"weekly"|"monthly"`) y pasó a ser un objeto, y el servidor manda siempre, además del
dato, **la repetición escrita en una línea** en el idioma del aparato.

### El modelo (`src/store.ts`)

```ts
repeat: {
  kind: "none" | "daily" | "weekdays" | "weekly" | "monthly" | "yearly",
  days?: number[],   // 0=domingo .. 6=sábado, solo para weekly
  interval?: number, // cada N días/semanas/meses/años (default 1)
  until?: number,    // epoch UTC en segundos, opcional (último día con ocurrencia)
}
```

`normalizeRepeat()` acepta cualquier cosa y no rompe nunca: `until` puede llegar como epoch o como
fecha `YYYY-MM-DD` (lo que manda un formulario o el clasificador de voz), los días repetidos o fuera
de rango se descartan, y los siete días marcados se guardan como `daily`.

**Migración de lo que ya estaba guardado**: la cadena vieja se sigue aceptando y se convierte al leer
el `store.json` (`normalizeStore`), sin tocar nada más: `"daily"` → `{kind:"daily",interval:1}`,
`"weekly"` → `{kind:"weekly",interval:1}` (sin `days`, o sea el mismo día de la semana del `dueAt`,
exactamente lo que hacía antes), `"monthly"` → `{kind:"monthly",interval:1}`, y cualquier basura →
`{kind:"none"}`. Probado con un `store.json` del formato viejo: no se pierde ningún recordatorio,
ninguna nota ni ningún ítem de lista.

### `repeatText()`: la repetición en una línea, en los seis idiomas

Es lo que se muestra en el aparato y en `/board` para no tener que interpretar nada:

| Repetición | es | en | ru |
|---|---|---|---|
| `{kind:"none"}` | Una sola vez | Once | Один раз |
| `{kind:"daily"}` | Todos los días | Every day | Каждый день |
| `{kind:"daily",interval:2}` | Cada 2 días | Every 2 days | Каждые 2 дня |
| `{kind:"weekdays"}` | De lunes a viernes | Monday to Friday | С понедельника по пятницу |
| `{kind:"weekly",days:[2,4]}` | Los martes y jueves | Tuesdays and Thursdays | По вторникам и четвергам |
| `{kind:"weekly",interval:2}` | Cada 2 semanas | Every 2 weeks | Каждые 2 недели |
| `{kind:"monthly"}` (día 15) | El 15 de cada mes | On the 15th of every month | 15-го числа каждого месяца |
| `{kind:"yearly"}` (3 de mayo) | Todos los años el 3 de mayo | Every year on May 3 | Каждый год 3 мая |
| `+ until` | …, hasta el 31 de diciembre de 2026 | …, until December 31, 2026 | …, до 31 декабря 2026 г. |

También están el francés, el alemán y el portugués (los nombres de los días están escritos a mano
por idioma para que la gramática cierre; la fecha del `yearly` y del `until` sale de `Intl`).
El texto viaja en `repeatText` en **cada recordatorio de `GET /api/hub`** y en **cada ocurrencia del
calendario**.

### `POST /api/hub/reminder` — alta y edición

Hasta ahora un recordatorio solo se podía crear por voz y tildar. Ahora:

```
POST /api/hub/reminder?lang=es
{ "id": 12,                          // sin id = alta
  "title": "Sacar la basura",
  "dueAt": "2026-09-08T21:00",       // o "2026-09-08" (sin hora) o null
  "repeat": { "kind": "weekly", "days": [2,5] } }
→ { ok, created, reminder: { id, title, at, when, dueAt (epoch), repeat, repeatText, done } }
   404 si el id no existe, 400 si no hay título.
```

Con una repetición semanal la fecha se **alinea al primer día que corresponde** (pedirlo un lunes
para "los martes y jueves" lo deja el martes, no el lunes). El mismo cuerpo lo acepta
`POST /api/board/reminder` desde la web.

**El formato plano del aparato.** El firmware (`AgendaActivity`, `HubStore`) no maneja el objeto:
manda y espera la repetición como un **código suelto** más `weekday` e `interval` aparte, y su día de
la semana **arranca en lunes** (`0` = lunes … `6` = domingo), al revés del `days` de adentro
(`0` = domingo). La traducción está en `repeatToWire()` / `repeatFromWire()` (`store.ts`):

| Aparato | Adentro |
|---|---|
| `"once"` | `{kind:"none"}` |
| `"daily"` | `{kind:"daily"}` |
| `"weekdays"` | `{kind:"weekdays"}` |
| `"weekly"` + `weekday:0` (lunes) | `{kind:"weekly", days:[1]}` |
| `"weeks"` + `interval:3` | `{kind:"weekly", interval:3}` |
| `"monthly"` / `"yearly"` | igual |

Lo que manda el editor del aparato es `{id, date:"YYYY-MM-DD", time:"HH:MM"|"" , repeat, weekday?, interval?}`
—sin título, que se conserva—, y cada recordatorio de `GET /api/hub` viaja con `repeat`, `weekday`,
`interval` (lo que lee `HubStore::parseReminders`), `repeatSpec` (el objeto entero, lo usa `/board`,
que sí puede con varios días) y `repeatText`. Una repetición de varios días le llega al editor con el
primero; el texto de arriba igual los dice todos.

### Voz

El clasificador (`voice.ts`) entiende las repeticiones al dictar: "los lunes y miércoles a las 8"
(`weekly` + `days:[1,3]`), "todos los días hábiles" (`weekdays`), "cada dos semanas"
(`weekly` + `interval:2`), "el 5 de cada mes" (`monthly`), cumpleaños y aniversarios (`yearly`),
"hasta fin de mes" (`until`). Cuando falta la hora, el servidor la pregunta y devuelve además
`askRepeat` (el objeto) y `askRepeatText`; el aparato puede devolverlo en el segundo turno como
`?pendingRepeat=<json>` para no perder el "todos los días" mientras se pregunta la hora.

## Calendario local (`src/calendar.ts`, `/data/calendar.json`)

**Sin Google, sin ICS y sin cuentas de nadie.** Todo vive en el volumen:

```json
{ "version": 1,
  "events": [ { "id": 12, "start": "2026-09-15T10:30", "end": "2026-09-15T11:30",
                "allDay": false, "title": "Dentista", "place": "…", "note": "…",
                "repeat": { "kind": "monthly" }, "tripId": "trip-7", "tripDay": 1 } ] }
```

Se lee y se escribe con `writeJsonAtomic`/`readJsonSafe`/`serialize` de `fsjson.ts` (nunca a mano):
cada modificación es un leer-modificar-escribir **adentro de la misma cola** que usan las escrituras
atómicas, así el módulo de viajes y este pueden escribir el archivo sin pisarse, y el archivo nunca
se cachea en memoria por lo mismo. Los campos que escriba otro módulo (vuelos, adjuntos, lo que sea)
se conservan tal cual; un evento con forma rara se descarta y los demás siguen.

Qué sale en el calendario: los eventos de `calendar.json`, los **recordatorios de `store.json`
proyectados** (se leen, no se copian: el dueño sigue siendo `store.ts` y se tildan con
`POST /api/hub/done`) y los ítems de viaje (los eventos con `tripId`).

| Ruta | Qué devuelve |
|---|---|
| `GET /api/calendar?from=&to=&lang=` | `{ ok, from, to, tz, today, count, truncated, days:[{date,count,firstTitle}], events:[…] }` con las repeticiones **ya expandidas**, tope de 500 ocurrencias. `days` es lo que pinta el mes y `events` lo que lo deja mirable sin WiFi (los nombres son los que espera `CalendarActivity`). `&summary=1` manda solo el resumen. Sin `from`/`to`: el mes de hoy; máximo 400 días. |
| `GET /api/calendar/day?date=&lang=` | `{ok, date, tz, count, items:[…]}`, el día completo. |
| `POST /api/calendar/event` | Alta, o **edición** si trae `id` (404 si no existe). Acepta `{title, date, time, endDate, endTime, allDay, place, note, repeat}` o `{start, end}` ya armados. Devuelve el evento y su `repeatText`. |
| `POST /api/calendar/event/delete` | `{id}` → `{ok, found}`. |
| `GET /api/calendar/repeat?kind=&days=&interval=&until=&date=&lang=` | `{ok, repeat, text, first}`: la repetición en una línea y el primer día que corresponde. Lo usa `/board` para mostrar el texto **mientras se edita**. |

Cada ocurrencia (el firmware lee de acá `date`, `time`, `title` y `place`) trae:
`key` (única, `ev-12@2026-09-15`), `id`, `kind` (`event`/`reminder`/`trip`),
`date`, `time`, `endTime`, `allDay`, `days` y `dayIndex` (un viaje de cuatro días sale los cuatro
días, numerado), `startAt`/`endAt` (el arranque de la **serie**, para editarla), `title`, `place`,
`note`, `repeat`, `repeatText`, `start`/`end` en epoch UTC y `tripId`/`tripDay` si es de un viaje.

### Zona horaria (dónde estaban los bugs)

La mitad de los bugs de calendario salen de mezclar **fechas** con **instantes**. Las reglas acá:

1. **La zona sale del lugar guardado** en `hub-settings.json` (el mismo que eligió el usuario para el
   clima); `HUB_TZ` es el respaldo. `refreshTimeZone()` la relee (como mucho una vez por minuto)
   antes de cada pedido del calendario y del hub: antes, un proceso que había arrancado sin lugar se
   quedaba con la zona vieja hasta el próximo deploy.
2. **Lo que se guarda es la hora local, nunca un epoch**: `"2026-09-15T10:30"`. Un evento de todo el
   día se guarda como **fecha sola** (`"2026-09-15"`), porque no es un instante: pasarlo a epoch "a
   lo bruto" lo corre un día para atrás o para adelante según la zona.
3. **La cuenta de las repeticiones es sobre fechas civiles** (`expandRepeat` / `nextOccurrence` en
   `store.ts`), sin epoch en el medio. Por eso un "todos los días a las 8" sigue siendo a las 8
   después del cambio de horario de verano, en vez de correrse a las 7 o a las 9.
4. El epoch se calcula **al final** (`localToEpoch`). Para los eventos de todo el día se usa
   `startOfLocalDay`/`endOfLocalDay`, que buscan la primera hora que **existe** ese día: hay días en
   los que las 00:00 no existen (Chile adelanta el reloj justo a la medianoche) y ahí la cuenta
   directa caía en el día anterior — el evento aparecía un día antes.
5. `end` de un evento de todo el día es el **último día incluido** (no el siguiente, como en ICS).
6. El 31 y el 29 de febrero **saltean** el mes o el año que no los tiene (igual que Google Calendar y
   el RFC 5545): "el 31 de cada mes" no se corre solo al 28 de febrero.

### `/board` → Calendario

Vista de mes (la grilla arranca el lunes, con el título del primer evento de cada día y cuántos hay),
el día elegido abajo con todo lo que cae ahí, y el formulario de alta/edición con la repetición
elegible de una lista. El texto de la repetición se ve **mientras se edita** y lo escribe el servidor
(`GET /api/calendar/repeat`), o sea que es exactamente el mismo que después muestra el aparato. Los
recordatorios de la pestaña Pizarra usan los mismos controles y se pueden editar con "Editar".

## Viajes y adjuntos (`src/trips.ts`, `src/attachments.ts`)

Lo que pidió el usuario: *"para un viaje quiero poder ver en el calendario qué voy a ir haciendo por día
y poder acceder por ejemplo a los QR de los vuelos o los datos de una reserva o los pdf del hotel"* y
*"ir viendo en mi viaje a qué hora tomar el tren, a qué hora entrar al hotel, a qué hora son las entradas
al Vaticano"*.

### El modelo (`/data/trips.json`)

```jsonc
{ "version": 1, "trips": [{
  "id": "...", "name": "Roma", "place": "Roma", "start": "2026-10-12", "end": "2026-10-16",
  "days": [{ "date": "2026-10-13", "note": "", "items": [
      { "id": "...", "at": "09:30", "title": "Tren a Termini", "kind": "train",
        "place": "Fiumicino", "note": "", "attachmentIds": ["..."] }] }],
  "packing": [{ "id": "...", "text": "Adaptador de enchufe", "done": false }],
  "docs": ["..."]                                  // adjuntos que no cuelgan de ningún ítem
}]}
```

`kind` es uno de `flight`, `train`, `hotel`, `ticket`, `meal`, `visit`, `other`; el nombre en los seis
idiomas lo devuelve el servidor en `kindLabel`. Los días se arman solos entre `start` y `end` y cambiar
las fechas **no borra** lo que ya estaba cargado (lo que queda afuera del rango se arrastra al final).
Todas las escrituras pasan por `serialize()` de `fsjson.ts` con lectura y escritura adentro de la misma
cola: dos pedidos a la vez se ordenan en vez de pisarse.

### Cómo se ve en el calendario

La fuente de verdad de un viaje es **siempre** `trips.json`. `calendar.ts` muestra como `kind: "trip"`
cualquier evento con `tripId`, así que `syncCalendar()` espeja los ítems adentro de `/data/calendar.json`
(mismo `serialize()` que usa `calendar.ts`, o sea que las dos escrituras se ordenan): el espejo se rehace
entero en cada cambio del viaje y nunca se edita a mano. Cada evento espejado lleva `tripId`, `tripDay`
(qué día del viaje es, 1 = el primero), `tripItem` y `tripKind`. `POST /api/trip/sync` lo reescribe si
alguien tocó el calendario por afuera. Quien prefiera leer los ítems sin pasar por el archivo tiene
`tripCalendarEvents(from?, to?)`, que los devuelve calculados.

### Adjuntos: del PDF del correo a algo que el aparato pinte

El aparato tiene e-ink de 480x800 en 4 grises y **no sabe leer PDF**, así que todo el trabajo es del
servidor. Un adjunto se procesa **una sola vez** al subirlo y queda guardado en
`/data/attachments/<id>/pN.bmp` (más `index.json` con lo que se extrajo).

1. **Rasterizado**: `mupdf` (wasm, sin dependencias nativas ni fuentes del sistema) abre el PDF, saca el
   texto y rasteriza cada página. La página se recorta a la caja del contenido (un pase de embarque son
   cuatro líneas arriba y medio A4 en blanco: recortando el margen el texto entra casi al doble) y se
   convierte con el mismo `toDeviceBmp` de `photos.ts`: 480x800, 4 grises, Floyd-Steinberg, BMP de 2 bpp.
2. **Códigos**: `zxing-wasm` busca códigos a 200 dpi y, si no encuentra ninguno, a 400.
3. **Los códigos se vuelven a generar, no se escalan**. Un pase de embarque casi nunca trae un QR: trae
   un **PDF417** (IATA BCBP) o un **Aztec** con módulos de menos de un milímetro, y escalar esa imagen a
   480 px de ancho deja un borrón que el lector del mostrador no engancha. Con el texto que devolvió
   zxing, `bwip-js` lo dibuja de nuevo en blanco y negro puro, sin grises ni suavizado, lo más grande que
   entre; después **se vuelve a decodificar el bitmap final** y solo si el texto coincide se marca
   `verified: true`. Se prefiere la forma y la orientación de siempre mientras el módulo quede en 3 px o
   más (0,5 mm en esta pantalla, de sobra para cualquier lector); recién si no entra se prueba girado 90°
   o con menos columnas.
4. **Si no se puede decodificar**, se recorta la región del código a máxima resolución, se baja con
   vecino más cercano (sin suavizado) y se umbraliza a blanco y negro: queda `copy: true` con
   `warn` diciendo que **puede no escanear y hay que llevar el original**. Eso se ve en `/board` al subir
   el archivo, que es cuando todavía se puede hacer algo, y no en la fila del mostrador.
5. **Datos útiles** (`extracted` y `fields`): el PDF417 de un pase es de ancho fijo (BCBP), así que
   `parseBcbp()` saca pasajero, vuelo, trayecto, fecha, asiento, reserva y secuencia sin adivinar nada
   (y si los campos no tienen la pinta que manda la norma, no dice nada en vez de escupir basura). Del
   texto salen puerta, terminal, horas de embarque, salida, llegada, check-in y check-out, habitación,
   coche y dirección. **Sin LLM**: son una docena de expresiones regulares, y tiene que andar aunque no
   haya proveedor de IA cargado.

Las páginas quedan ordenadas con **los códigos primero** (es lo que se busca corriendo en el aeropuerto)
y después las páginas del documento; `pageList` dice cuál es cuál.

### Límites

| Qué | Cuánto | Por qué |
|---|---:|---|
| Tamaño de una página servida | **96.070 bytes** (480x800 a 2 bpp) | El aparato no soporta `Range` y baja el archivo entero con tope de 512 KB. |
| Páginas por adjunto | 12 | Un itinerario de 40 páginas no sirve en e-ink y cada página ocupa 96 KB del volumen. |
| Subida | 20 MB | Un PDF de reserva con fotos entra holgado. |
| Adjuntos guardados | 200 | Se avisa y no se borra nada del usuario en silencio. |
| Píxeles al rasterizar | 14 M | Para no reventar la memoria con un A3 a 400 dpi. |

Borrar un viaje borra sus adjuntos (si no, quedan 96 KB por página tirados en el volumen) y le saca los
eventos al calendario.

### Qué librería se eligió y cuál no

- **`mupdf`** para rasterizar y sacar el texto: es wasm, anda en Bun sin nada instalado y trae las fuentes
  base adentro. `pdfjs-dist` y `unpdf` necesitan un canvas nativo (`@napi-rs/canvas`) para rasterizar, que
  es una dependencia binaria más en la imagen de Railway: descartados.
- **`zxing-wasm`** para decodificar: lee PDF417, Aztec, QR, DataMatrix y los lineales, y trae el `.wasm`
  adentro del paquete (se carga con `wasmBinary` y no se lo baja de ningún CDN en cada arranque).
  Ojo: hay que **aplanar sobre blanco** antes de decodificar, porque un PNG con transparencia le llega
  como una mancha negra y no lee nada.
- **`bwip-js`** para volver a generar: hace PDF417, Aztec, QR y Code128 con la misma API.

### `/board` → Viajes

Crear el viaje con sus fechas, ver los días, agregar cosas a cada día con hora y tipo, subir los papeles
(eligiendo si son del viaje o de un ítem) y ver ahí mismo qué se extrajo y si el código quedó verificado
o es una copia, y la lista de para llevar. Todo lo de la pestaña usa `data-act` que empiezan con `trip-`
y su propio delegador, así no se pisa con el resto de la página.

## Paquete de contenido descargable (`/api/assets/*`)

Todo lo pesado va en **un solo paquete** que el aparato se baja después de actualizar el firmware, en
vez de tener un botón distinto por cada cosa (el de "bajar la Biblia" desaparece: la Biblia son 66
archivos más del manifiesto).

### Qué trae y cuánto pesa

Medido con el catálogo de hoy (240 tarjetas, Biblia Reina-Valera en español):

| `kind` | Qué | Archivos | Tamaño |
|---|---|---:|---:|
| `bible` | La Biblia entera del idioma, un archivo de texto por libro (`#<capítulo>` y los versículos numerados) | 66 | **3,83 MB** |
| `cards` | El índice de las tarjetas (`index.json`, 43,5 KB) y el dibujo de cada una: BMP de **2 bpp (4 grises)** de 320x320 (25.670 bytes) | 241 | **6,20 MB** |
| `sounds` | La palabra de cada tarjeta dicha por Piper, en español y en inglés, en el mismo ADPCM que ya usa el aparato | 480 | **~3,7 MB** (estimado) |
| `icons` | Reservado | 0 | — |
| | **Total del paquete en español** | **787** | **~13,7 MB** |

El audio es lo único estimado: un clip son `8 + muestras/2` bytes a 16 kHz, y una palabra suelta de
Piper dura entre 0,8 y 1,1 s → 6,5 a 8,8 KB por clip. Los otros dos números están medidos. Cada dibujo
pesa 25.670 bytes: el doble que la versión de 1 bpp y **veinte veces menos** que el tope de 512 KB por
archivo que baja el aparato.

En otro idioma cambia solo la Biblia (el inglés pesa menos, el ruso más); los dibujos y los audios son
los mismos para todos (las tarjetas se dicen siempre en español y en inglés, que es de lo que se trata
el juego).

### Cómo se genera

Solo, sin ningún paso a mano:

- Los **dibujos** salen de **Noto Color Emoji** ([googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji),
  código Apache 2.0 y glifos OFL 1.1): son ilustraciones llenas, no iconos de trazo. Un bebé no reconoce
  un contorno, reconoce una figura. Se eligió Noto justamente porque **no obliga a atribuir a nadie**
  (OpenMoji es CC BY-SA y Twemoji CC BY, los dos con atribución obligatoria en el producto); igual queda
  el crédito acá. El SVG de cada emoji se baja de jsDelivr (`NOTO_EMOJI_REF`) **una sola vez** y queda
  guardado en `ASSETS_DIR/emoji/<codepoint>.svg`, así una segunda corrida no vuelve a bajar nada.
- Los **números** (1 a 10) y las **formas** no salen de los emoji: los dígitos no existen y las formas son
  cuadraditos de colores que en gris no se distinguen, así que se dibujan acá (N puntos para contar; la
  forma llena en negro).
- La **voz** la hace Piper con la voz de cada idioma (`tts.ts`), una vez por palabra.
- Todo queda en `ASSETS_DIR` (el volumen) con un índice `index-<lang>.json`, así que un redeploy no
  vuelve a generar nada. La primera corrida tarda unos minutos por los 480 clips de Piper; los 240
  dibujos salen en 7 s y los 66 libros en unos segundos.

### Los dibujos son de 4 grises, no de blanco y negro

La pantalla del aparato pinta **cuatro grises** y las fotos ya se ven así (`toDeviceBmp` en `photos.ts`);
las tarjetas se hacían en 1 bpp por costumbre, no por necesidad. Ahora salen del mismo pipeline, en tres
pasos que importan los tres:

1. el emoji se rasteriza **con transparencia**, para saber qué píxel es figura y cuál es fondo;
2. el tono de la figura se estira a `[0, 190]` (percentiles 2/98). Sin esto, un emoji amarillo o blanco
   —la luna, la estrella, la banana, el vaso de leche— queda del color del papel y **desaparece**;
3. Floyd-Steinberg a los cuatro niveles y BMP de 2 bpp, igual que las fotos.

Comparado renderizando el catálogo entero y mirándolo (tal cual / escala fija / normalizado): el
normalizado es el único que deja legibles la luna y el vaso de leche.

> **Falta del lado del firmware**: `CardsActivity::drawCard` dibuja el BMP con un `drawBitmap` común, y
> en modo `BW` el SDK pinta de negro **todo** lo que no sea blanco puro (`val < 3`), así que un BMP de 4
> grises sale como una mancha. Hay que dibujar la tarjeta con el pipeline de gris del SDK, que ya está
> escrito y probado en `PhotosActivity::drawFullScreenPhoto` (base BW + pasada `GRAYSCALE_LSB` + pasada
> `GRAYSCALE_MSB` + `displayGrayBuffer`). Hasta que eso esté, el aparato muestra la silueta del dibujo.
>
> **Y una de la web**: `GET /api/assets/status` ahora devuelve `art` (`noto/<ref>/4gray-v1`) además del
> viejo `lucide`, que se mantiene solo para que la tarjeta de `/board` no diga "undefined". Cuando esa
> página muestre `Dibujos: <art>` en vez de `Lucide <...>` (`board.ts`), el campo `lucide` se saca.

### Las palabras van en español neutro

Regla fija, escrita también arriba de `src/cards.ts` para que no se vuelva a colar un regionalismo: lo
que ve el usuario se entiende en toda Hispanoamérica y en España, y **si una palabra no tiene una
variante clara y neutra se saca y se pone otra** (son tarjetas para un bebé, sobran los sustantivos
fáciles). Ya resueltos: *remera* → **camiseta**, *frutilla* → **fresa**, *ananá* → **piña**, *palta* →
**aguacate**, *colectivo* → **autobús**, *anteojos* → **gafas**, *media* → **calcetín**, *torta* →
**pastel**, *celular* → **teléfono**, *ordenador* → **computadora**, *mamadera* → **biberón**,
*sillón* → **sofá**, *cacahuete* → **maní**, y *auto* para el carro/coche. Las que no tenían arreglo
—durazno/melocotón, papa/patata, palomitas/pochoclo, zumo/jugo, manteca/mantequilla, lavarropas/lavadora,
heladera/refrigerador— se cambiaron por otra tarjeta. El inglés va en **americano** (cookie, candy,
truck, pants).

Se dispara al arrancar el servidor (5 s después, para no pelear con el arranque) y cuando llega un
pedido de manifiesto si falta algo, con un repaso como mucho cada 10 minutos.

**No hay categoría de colores**: con cuatro grises un dibujo sigue sin poder decir "rojo". En su lugar va
**formas** (círculo, cuadrado, triángulo, estrella…), que es una categoría clásica de tarjetas para bebés
y sí se entiende. Las categorías son: Animales (46), Comida (38), Casa (31), Cuerpo (16), Formas (9),
Números (10), Vehículos (20), Naturaleza (29), Ropa (15) y Juguetes (26).

### Contrato exacto (para el firmware)

Lo que sigue es lo que ya esperan `AssetSyncActivity` y `CardsActivity`; si cambia una ruta o un nombre
de campo, hay que cambiarlo en los dos lados.

**`GET /api/assets/manifest?lang=xx`** — parámetros opcionales: `kind=bible,cards` (filtra),
`from=0&limit=400` (pagina). El manifiesto completo en español son ~95 KB de JSON con el audio incluido,
así que conviene leerlo en streaming o pedirlo por `kind`. `v=` (la versión del firmware) se acepta y se
ignora.

```json
{
  "ok": true,
  "version": "873b834b2d521160",
  "lang": "es",
  "building": false,
  "progress": { "done": 784, "total": 784 },
  "total": 306, "from": 0, "count": 306, "bytes": 7139000,
  "items": [
    { "id": "bible/es/b00",       "kind": "bible",  "path": "/.crosspoint/bible/es/b00.txt",        "bytes": 192553, "sha": "6f696e077072234b" },
    { "id": "cards/index",        "kind": "cards",  "path": "/.crosspoint/cards/index.json",        "bytes": 44667,  "sha": "8ec015a8be996c00" },
    { "id": "cards/ani-perro",    "kind": "cards",  "path": "/.crosspoint/cards/img/ani-perro.bmp", "bytes": 25670,  "sha": "e8e65c7c7ed15ea1" },
    { "id": "sounds/es/ani-perro","kind": "sounds", "path": "/.crosspoint/cards/audio/es/ani-perro.adp", "bytes": 7208, "sha": "0d2e…" }
  ]
}
```

- `version` es el resumen de **todo** el paquete de ese idioma (no del pedazo que pediste): si no cambió
  respecto de lo que guardaste, no hace falta comparar archivo por archivo.
- `sha` son los **primeros 16 hex del sha256** del archivo. Alcanzan para decidir si hay que bajarlo y
  mantienen el manifiesto chico.
- `path` es **dónde va en la SD**, tal cual. El aparato no tiene que armar rutas.
- `bytes` es el tamaño final del archivo; sirve para saber cuánto falta y para reanudar.
- `building: true` quiere decir que todavía se están generando archivos (la voz tarda la primera vez):
  bajá lo que ya está listado y volvé después por el resto.
- `total` es cuántas entradas hay con el filtro puesto; `count` cuántas vinieron en esta página.

**`GET /api/assets/file?id=<id>`** — el `id` va tal cual salió del manifiesto (urlencodeado).

- 200 con `Content-Length`, `ETag: "<sha>"`, `Accept-Ranges: bytes`,
  `Cache-Control: public, max-age=31536000, immutable`.
- Con `Range: bytes=<n>-` responde **206** con `Content-Range: bytes n-fin/total`: así se reanuda una
  descarga cortada sin empezar de cero. `Range: bytes=0-99` también anda. (Hoy el `ServerClient` del
  aparato no lo usa; está listo para cuando sepa bajar a archivo en streaming.)
- **416** con `Content-Range: bytes */<total>` si el rango no entra.
- **404** `{ok:false, code:"not_found"}` si el id no está en el manifiesto (pedí el manifiesto de nuevo).
- **410** `{ok:false, code:"gone"}` si está en el índice pero el archivo se perdió del volumen.

**El índice de las tarjetas** (`/.crosspoint/cards/index.json`, id `cards/index`) es lo que lee
`CardsActivity`; las rutas de adentro son relativas a `/.crosspoint`:

```json
{ "cards": [ { "id": "ani-perro", "es": "perro", "en": "dog", "cat": "Animales",
               "img": "cards/img/ani-perro.bmp",
               "audioEs": "cards/audio/es/ani-perro.adp",
               "audioEn": "cards/audio/en/ani-perro.adp" } ] }
```

**El dibujo** es un **BMP de 2 bpp** con paleta de cuatro grises (0, 85, 170, 255), filas de abajo hacia
arriba y padding a 4 bytes — el mismo formato que ya usan las fotos y los adjuntos, o sea lo que el
`Bitmap` de `lib/GfxRenderer` ya sabe leer, sin ningún formato propio. 320x320 = 70 bytes de cabecera +
80 por fila = 25.670 bytes.

**El audio** (`.adp`) es el mismo ADPCM que devuelve `/api/tts`: cabecera `"ADPC"` + cantidad de muestras
(uint32 LE) + los nibbles, 16 kHz mono. Lo reproduce `SpeechOut::playFile` del firmware.

Las palabras de las tarjetas (id, español, inglés, categoría e ícono) están en `src/cards.ts`.

**Qué se regenera cuando algo cambia.** Cada entrada del índice guarda un `tag` con de qué se generó: el
dibujo (`emoji:1f436`, `num:3`, `shape:circulo`) o la palabra que dice el clip. Si el `tag` no coincide
con el catálogo de hoy, esa entrada sola se cae y se rehace; los ids que ya no existen se van. Además el
índice lleva la marca `art` (`noto/<ref>/4gray-v1`): al cambiarla se descartan **todos** los dibujos y se
regeneran, sin tocar la Biblia ni el audio. Con eso, cambiar *remera* por *camiseta* regenera un dibujo y
dos clips de Piper, no los 480.

## Proveedores de IA, búsqueda y costo por consulta

Todo se elige desde `/board` → **IA** y se guarda en el volumen (`config.json`); las claves nunca vuelven
por la API.

**Catálogos revisados el 2026-09-08.** Los modelos viejos de Groq (`llama-3.3-70b-versatile`,
`llama-3.1-8b-instant`) pasaron a Enterprise / Contact Sales y con una cuenta normal de desarrollador ya
no responden; en DeepSeek, `deepseek-chat` y `deepseek-reasoner` quedaron atrás. Los presets de hoy:

| Proveedor | Modelos | Búsqueda en internet |
|---|---|---|
| Anthropic | `claude-haiku-4-5`, `claude-sonnet-4-5` | herramienta del lado del servidor de Anthropic, **USD 0,01 por búsqueda** |
| Groq | `openai/gpt-oss-120b`, `openai/gpt-oss-20b`, `groq/compound`, `groq/compound-mini` | **incorporada**, incluida en el precio de los tokens |
| DeepSeek | `deepseek-v4-flash` (1M de contexto), `deepseek-v4-pro`, `deepseek-v4-flash-vision-exp` | la hace este servidor |
| OpenAI | `gpt-4o-mini`, `gpt-4o` | la hace este servidor |

La búsqueda se resuelve en este orden (`llm.ts` → `providerSearchKind`):

1. **Anthropic** → su herramienta de servidor (`web_search_*`), el modelo decide si la usa.
2. **Groq** → la búsqueda del propio proveedor: `groq/compound` y `groq/compound-mini` buscan solos (no
   hay que declarar nada; las fuentes vuelven en `choices[0].message.executed_tools[].search_results.results[]`)
   y los `openai/gpt-oss-*` con `tools: [{type: "browser_search"}]` y `reasoning_effort: "low"` (sus citas
   vienen incrustadas en el texto como `【2†L6-L10】` y se limpian antes de mandarlas a la pantalla).
3. **Cualquier otro compatible con OpenAI** → recién ahí busca este servidor (`websearch.ts`: Google
   Noticias + DuckDuckGo gratis, o Tavily/Brave con clave) y le pasa los resultados al modelo en el prompt.

### Costo por consulta

El aparato se vende en volumen, así que el costo por consulta manda. Una consulta de voz típica (5 s de
audio, ~1500 tokens de entrada y 300 de salida), transcribiendo con `whisper-large-v3-turbo`:

| Modelo | Entrada / salida (USD por 1M) | Total por consulta |
|---|---|---:|
| `openai/gpt-oss-20b` | 0,075 / 0,30 | **US$ 0,00026** |
| `openai/gpt-oss-120b`, `groq/compound` | 0,15 / 0,60 | **US$ 0,00046** |
| `deepseek-v4-flash` (fuera de pico) | 0,22 / 0,66 | **US$ 0,00058** |
| `deepseek-v4-flash` (pico) | 0,44 / 1,32 | US$ 0,00111 |
| `deepseek-v4-pro` (fuera de pico) | 0,66 / 1,98 | US$ 0,00164 |
| `claude-haiku-4-5` | 1 / 5 | US$ 0,00306 |
| `claude-sonnet-4-5` | 3 / 15 | US$ 0,00906 |

La transcripción sale US$ 0,000056 con `whisper-large-v3-turbo` (US$ 0,04 la hora de audio) — o sea, casi
nada, y es de lejos la mejor opción del proyecto. **Una búsqueda en internet con Anthropic cuesta US$ 0,01:
más que la respuesta entera**, que es exactamente por qué no se busca en todas las preguntas.

DeepSeek cobra **el doble en hora pico** (01:00-04:00 y 06:00-10:00 UTC de lunes a viernes); la tabla de
`/board` calcula con el precio que corresponde a la hora en que se la mira. Los precios viven en
`config.ts` (`MODEL_PRICES`, `STT_PRICES`) y se muestran en la pestaña IA.

## Memoria del asistente

"Recuerda que soy vegetariano", "mi hija se llama Ana": lo que el usuario le pide que recuerde se guarda
en `store.memories` y **entra en el system prompt de toda pregunta** — la general del hub (`voice.ts`), la
que se hace leyendo un libro (`ask.ts`) y el clasificador de intención. Hasta la 1.5.x se guardaba y no lo
leía nadie.

- Tope de 40 hechos o 2 KB, lo que se cumpla primero, quedándose con los más nuevos (`memoryLines`).
- El bloque es el más estable de todos, así que va **primero y cacheado**: en Anthropic como bloque de
  system con `cache_control`, y en las compatibles con OpenAI al principio del system, que es donde pega
  la caché de prefijo del proveedor. Sin eso se pagarían esos tokens enteros en cada consulta.
- Una memoria nueva **reemplaza** a la más parecida en vez de acumularse (`rememberFact`, parecido por
  palabras): "ya no vivo en México" pisa a "vivo en México" y no quedan dos que se contradicen.
- Se ven y se borran en `/board` → Pizarra → Memoria del asistente.


## Multiusuario: cuentas, aparatos y `DATABASE_URL`

El aparato se vende en volumen, así que el servidor tiene que aguantar ~1000 aparatos de gente distinta. Eso se
enciende con **una sola variable**.

### Regla de oro: sin base de datos, nada cambia

Si `DATABASE_URL` **no** está en el entorno, el servidor se comporta **exactamente** como siempre: los JSON
sueltos en `/data`, un solo `DEVICE_TOKEN`, sin login, y `/board` pidiendo el token del aparato. Es lo que corre
hoy en Railway y no se puede caer. La bifurcación está en **un solo lugar**, `src/fsjson.ts`
(`readDoc` / `writeDoc` / `mutateDoc`), y en `src/db.ts`; el resto del servidor solo recibe un `accountId` y no
sabe de dónde salen los datos.

### El diseño: `docs`

Cada archivo JSON de hoy pasa a ser una fila `docs(account_id, name)` con **el mismo contenido**:

| Antes | Ahora |
|---|---|
| `/data/store.json` | `docs(1, "store")` |
| `/data/calendar.json` | `docs(1, "calendar")` |
| `/data/trips.json` | `docs(1, "trips")` |
| `/data/suggest.json` | `docs(1, "suggest")` |
| `/data/hub-settings.json` | `docs(1, "hub-settings")` |
| `/data/hub-data.json` | `docs(1, "hub-data")` |
| `/data/attachments/index.json` | `docs(1, "attachments")` |

Por eso `store.ts`, `calendar.ts`, `trips.ts` y compañía **no cambiaron su lógica**: cambió de dónde leen y
escriben. Las tablas son `accounts`, `devices`, `pairings`, `docs`, `usage` y `server_meta`; se crean solas al
arrancar con `CREATE TABLE IF NOT EXISTS` (no hay herramienta de migraciones).

Lo que **no** es JSON sigue siendo archivo, pero por cuenta: la cuenta 1 (la que ya venía andando) se queda en
`/data/photos`, `/data/attachments` y `/data/device.log`, y las cuentas nuevas van a
`/data/accounts/<id>/photos`, `/attachments` y `/device.log`. El `id` que llega por la URL se limpia a `[a-z0-9]`
y el de la cuenta es un número, así que ninguna ruta puede salirse de su directorio.

`writeDoc` es un `INSERT ... ON CONFLICT DO UPDATE` y `mutateDoc` es una transacción con un candado de Postgres
por `(cuenta, documento)` (`pg_advisory_xact_lock`) más `SELECT ... FOR UPDATE`: eso ordena a dos pedidos a la
vez **y a dos réplicas**, cosa que la cola en memoria de los archivos no podía. Probado con 12 altas de
calendario simultáneas sobre un documento que todavía no existía: entran las 12.

### Quién es cada pedido (`src/tenant.ts`)

1. **aparato** → `Authorization: Bearer <token>`; se busca `sha256(token)` en `devices.token_hash` (el token en
   claro **no se guarda nunca**) y sale su `account_id`. Se marca `devices.last_seen`.
2. **web** → cookie de sesión firmada (HttpOnly, `SameSite=Lax`, `Secure` sobre https). No hay tabla de sesiones:
   la cookie es `<cuenta>.<vencimiento>` firmado con `SESSION_SECRET`, y dura 30 días.
3. **el de siempre** → el `DEVICE_TOKEN` del entorno y el `config.deviceToken` valen **siempre** y son la
   **cuenta 1**. Es lo que hace que el aparato que ya está andando siga andando sin tocarle nada.

El middleware de `/api` deja la cuenta en el contexto de Hono (`c.set("accountId", ...)`) y todos los handlers la
leen de ahí con `accountOf(c)`. La **zona horaria** del pedido (la del lugar que eligió esa cuenta para el clima)
viaja en un `AsyncLocalStorage` que arma ese mismo middleware: es lo único implícito de todo esto, y es a
propósito, porque `localToEpoch()` y media docena de funciones de fechas son sincrónicas y las llama todo el
mundo.

### Vincular un aparato: el código de 6 dígitos

El aparato **no tiene teclado**, así que nunca puede escribir un correo. El flujo va al revés:

```
POST /api/pair/start          (SIN Bearer: el aparato todavía no está vinculado)
body: { "deviceId": "A1B2C3D4E5F6", "token": "<64 hex>" }
200:  { "ok": true, "code": "482913", "expiresIn": 600 }
```

El servidor guarda `pairings(code, device_id, sha256(token))` con 10 minutos de vida. El código son 6 dígitos al
azar, sin repetir uno vivo. Un pedido cada 30 s por `deviceId`: mientras tanto devuelve **el mismo código**, así
el aparato que reintenta no le cambia el número al usuario en la cara. Los vencidos se borran al crear uno nuevo
(no hay cron).

```
POST /api/account/pair        (con la cookie de sesión, desde la web)
body: { "code": "482913", "name": "El lector de la cocina" }
200:  { "ok": true, "deviceId": "A1B2C3D4E5F6" }
```

Crea (o reasigna) la fila de `devices` para esa cuenta y borra el pairing. Si el aparato ya estaba en otra
cuenta, se **mueve** a esta. Solo lo acepta una sesión de la web: un aparato con su Bearer no puede reasignarse
solo (403 `no_session`).

```
GET /api/pair/status          (con Bearer del token del aparato)
200:  { "ok": true, "paired": true|false, "account": "ana@ejemplo.com"|null }
```

El aparato lo consulta cada pocos segundos mientras muestra el código. **Sin `DATABASE_URL`**, `pair/start`
devuelve 501 `{code:"single_user"}` y `pair/status` devuelve `{paired:true, account:null, single:true}` si el
token sirve: no hay nada que vincular.

### Migración de lo que ya existe

La primera vez que arranca con `DATABASE_URL`, si la tabla `accounts` está vacía:

- crea la cuenta 1 con el correo de `ADMIN_EMAIL` (o `admin@localhost`) y una contraseña al azar que **se imprime
  una sola vez en el log del servidor**, con el aviso de cambiarla (se cambia desde /board → Aparatos);
- vuelca cada JSON de `/data` a `docs` con esa cuenta;
- registra el `DEVICE_TOKEN` del entorno como su primer aparato (guardando el hash, no el token).

Es idempotente: con la tabla `accounts` ya poblada no toca nada.

### Claves de IA y costo

Con 1000 aparatos no puede poner cada uno su clave de Anthropic: `config.json` (proveedor, modelos, claves de
LLM/STT, buscador, token del aparato) es **del operador**, uno solo para todo el servidor, y **solo lo ve y lo
toca una cuenta admin**. Para las demás, la pestaña IA de `/board` ni se muestra y `GET/POST /api/board/config` y
`POST /api/board/config/test` contestan **403**. Sin base de datos hay un solo usuario y es el admin, así que
todo sigue igual que siempre.

El consumo se cuenta por cuenta y por mes en `usage` (llamadas al LLM y segundos de audio transcritos; los
segundos salen del tamaño del cuerpo, 32 kB/s en WAV y 8 kB/s en ADPCM). **Solo se cobra lo que salió bien**: un
502 del proveedor no se le carga a nadie. Pasado `MONTHLY_LLM_CALLS` o `MONTHLY_STT_SECONDS`, estas cuatro rutas
contestan **429** con el mensaje ya traducido al idioma del pedido y `code: "quota"`:

```
/api/ask   /api/voice   /api/transcribe   /api/translate
```

**Todo lo demás sigue andando**: hub, calendario, recordatorios, listas, notas, biblia offline, música, fotos,
noticias y viajes. Pasarse de preguntas no convierte el aparato en un ladrillo.

### Dos cosas que eran del operador y ahora se aíslan

- **`HUB_ICS_URL`** es una variable del entorno, o sea del operador: si valiera para todas las cuentas, el
  calendario privado del que la puso se lo verían los 1000 aparatos. En multiusuario vale **solo para la
  cuenta 1**.
- **La caché de feeds RSS** se guardaba por **id de feed**, y los ids son de cada cuenta (el feed 5 de una casa no
  es el feed 5 de otra): una cuenta veía los titulares de la otra. Ahora la clave es la URL, que además comparte
  lo ya bajado entre cuentas.

Las cachés en memoria (el store, el lugar, el clima, el pronóstico, la zona horaria) pasaron de una variable
suelta a un `Map` **con tope** por cuenta: con 1000 aparatos, un `Map` sin límite es una fuga de memoria.

### `/board` con login

- **Sin base de datos**: igual que siempre, con el formulario del token del aparato.
- **Con base de datos**: pantalla de entrar / crear cuenta, y la sesión va en una cookie HttpOnly — **no se guarda
  ningún token en el navegador**. Aparece la pestaña **Aparatos** (lista, renombrar, desvincular, el campo para el
  código de 6 dígitos, cambiar la contraseña y salir) y la pestaña **IA** solo si la cuenta es admin.
- La misma página sirve para los dos casos: lo decide el script preguntando `GET /auth/me` **antes** de cualquier
  otra cosa (si no, un 401 de un pedido suelto mostraba la pantalla del token en un servidor con login).
- `/board/log` también entra con la sesión; si el servidor no tiene cuentas, sigue pidiendo el token.
- Los botones de las listas van por delegación con `data-act`, **nunca** con `onclick` armado con comillas, y
  adentro del template literal del script **no puede haber backticks** (ni siquiera en un comentario: se lo come
  el literal y rompe la página entera; pasó dos veces escribiendo esto).

### Encenderlo en Railway

1. En el proyecto, **New → Database → Add PostgreSQL**.
2. En el servicio del servidor, agregar la variable `DATABASE_URL` con la referencia
   `${{Postgres.DATABASE_URL}}`, más `ADMIN_EMAIL` y `SESSION_SECRET` (una cadena larga al azar).
3. Redeploy. En el log aparece la contraseña provisoria del admin **una sola vez**: entrar a `/board`, cambiarla
   desde Aparatos, y de ahí en más cada usuario se crea su cuenta y vincula su aparato con el código.
4. El volumen en `/data` **sigue haciendo falta**: ahí viven el firmware, el paquete de contenido, la caché de la
   Biblia, las fotos y los adjuntos.

Para volver atrás alcanza con sacar `DATABASE_URL`: los archivos originales de `/data` siguen ahí (la migración
los copia, no los borra), así que el servidor vuelve al modo de un solo usuario con los datos que tenía el día que
se encendió la base.

## Local

Como siempre (un solo usuario, archivos sueltos):

```
cd server && bun install && OTA_TOKEN=x DEVICE_TOKEN=y ANTHROPIC_API_KEY=... STT_API_KEY=... bun run src/index.ts
```

Multiusuario, contra un Postgres local:

```
cd server && bun install
DATABASE_URL=postgres://postgres@127.0.0.1:5432/ws397 ADMIN_EMAIL=vos@ejemplo.com SESSION_SECRET=lo-que-sea \
  OTA_TOKEN=x DEVICE_TOKEN=y bun run src/index.ts
```

Chequeo rápido antes de subir nada: `bun install && bunx tsc --noEmit`.
