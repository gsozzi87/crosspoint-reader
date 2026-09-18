# Viajes — contrato entre servidor, web y `viajes.lua` (v1)

Decisiones del dueño (18-09-2026) sobre `VIAJES_APP.md`:

- **No hay Diario ni "libro del viaje".** Se sacan de la app y de la web.
- **La guía se guarda directo en la tarjeta**: sin pantalla de confirmación para bajar ni para rehacer.
- **Antes de generar la guía, la app PREGUNTA POR VOZ** lo que le falta al itinerario (hotel, hora de llegada,
  hora de salida, intereses…). El servidor mira lo cargado y decide qué preguntar; con esas respuestas genera.
- **Preguntar por voz sí**, con todo el viaje en el contexto del modelo, y la respuesta **hablada** además de
  escrita (puerta nueva `cp.say`).

**Cambio del 18-09 (v2), después de probar 1.5.111**: un viaje son VARIOS lugares (Roma, crucero, islas, Madrid;
trenes; un hotel distinto cada noche), así que **cada día tiene su lugar y su hotel**, y **la guía es POR DÍA y
sólo a pedido**: no hay guía general del viaje. Para un día, el servidor mira dónde se está y qué hay cargado
(Barcelona, Casa Batlló) y busca qué hay cerca, qué se está perdiendo uno, cómo moverse entre las cosas del
día, dónde comer, y lo práctico de ESA fecha. Las preguntas por voz son por día (¿dónde duermes esa noche?, ¿qué
te interesa?), sólo lo que falta.

Todo lo demás de `VIAJES_APP.md` sigue valiendo. Este archivo es la verdad sobre los nombres y las formas.

## Servicios (`POST /api/apps/call`, `app = "viajes"`)

Siempre HTTP 200 con `{ok, …}`; error = `{ok:false, error}`. `lang` viene en la query como en todas las apps.
Tiempos: síncronos en < 25 s; lo largo (la guía) es un trabajo (`job.status`).

| Servicio | args | Devuelve |
| --- | --- | --- |
| `viajes.lista` | — | `{trips:[{id, name, place, start, end, when, active, itemCount, paperCount, packDone, packTotal, guideReady}]}` |
| `viajes.activar` | `{id}` | `{id}` — lo deja como viaje activo de la cuenta |
| `viajes.viaje` | `{id?}` (sin id: el activo) | `{trip}` — la **vista compacta** de abajo |
| `viajes.papel` | `{id, paperId}` | `{title, text}` — el texto del papel, campos primero ("Localizador: ABC123") y el texto entero después |
| `viajes.llevar` | `{id, action:"toggle"\|"remove", itemId}` o `{id, action:"add", text}` | `{packing:[…]}` — `add` recibe lo DICHO ("cargador, adaptador y quita el paraguas"): el servidor lo parte con el modelo en altas y bajas |
| `viajes.sugerir` | `{id}` | `{suggestions:[texto…]}` — hasta 12, según destino, fechas, clima y agenda; **no agrega nada** |
| `viajes.sugerir.agregar` | `{id, texts:[…]}` | `{packing:[…]}` |
| `viajes.guia.preguntas` | `{id, date}` | `{questions:[{key, text}]}` — 0 a 3 preguntas POR DÍA para hacer por voz: `hotel` si el día no tiene hotel cargado, `llegada` (cómo y a qué hora se llega) si el día cambia de lugar y no hay transporte cargado, `intereses` siempre. Si el itinerario ya lo dice, no lo pregunta. |
| `viajes.guia.generar` | `{id, date, answers:{key:texto}}` | `{jobId}` — trabajo: UNA guía del día (600-1000 palabras) con búsqueda en internet; progreso con `label` ("Buscando cerca de Casa Batlló…"). Se guarda en `day.guide`. |
| `viajes.guia.dia` | `{id, date}` | `{text, at}` — la guía de ese día, texto plano para el visor (hasta 24 KB); `{ok:false, error:"sin guía"}` si no hay |
| `viajes.preguntar` | `{id, question, date?, itemId?}` | `{answer, spoken}` — `spoken` es la versión corta para el parlante (≤ 220 caracteres) |
| `viajes.recordar` | `{id, itemId, on:bool}` | `{reminderId?}` — recordatorio 2 h antes en el store (suena como cualquier otro); `on=false` lo borra |
| `job.status` | `{id}` | el de siempre |

### La vista compacta (`viajes.viaje` → `trip`)

```json
{
  "id": "a1b2c3", "name": "Italia y España", "place": "Roma · crucero · Madrid", "start": "2026-09-14",
  "end": "2026-09-26", "when": "14 – 26 de septiembre", "today": "2026-09-16",
  "days": [
    {"date": "2026-09-14", "n": 1, "label": "sábado 14 de septiembre", "short": "sáb 14",
     "place": "Roma", "hotel": "Hotel Artemide", "note": "", "guide": {"ready": false, "at": 0},
     "items": [
       {"id": "i1", "at": "10:40", "title": "Vuelo IB6251 MAD → LIS", "kind": "flight", "kindLabel": "Vuelo",
        "place": "T4", "code": "ABC123", "note": "", "paperId": "p1", "remind": false}
     ]}
  ],
  "packing": [{"id": "k1", "text": "Pasaporte", "done": true}],
  "papers": [{"id": "p1", "date": "2026-09-14", "kind": "flight", "title": "Vuelo IB6251", "line": "loc. ABC123"}]
}
```

- `days` trae **todos** los días del viaje, vacíos incluidos, en orden; `label`/`short` ya en el idioma.
- `today` es la fecha civil de hoy en la zona del viaje (o `""` si el servidor no la sabe); la app la usa si el
  aparato no está en hora.
- `place` del viaje es un resumen ("Roma · crucero · Madrid"); el lugar de verdad es el de cada día. Sin clima
  en la vista (un viaje de varios lugares no tiene UN clima).
- Tope: la vista no pasa de **40 KB** (se recortan notas largas); la app la guarda entera en `viaje.json`.

### Datos (servidor, documento `trips`, resucitado de `d13923b^:server/src/trips.ts`)

```ts
Trip { id, name, place, timezone?, start, end, notes?, active?: boolean,
       days: [{date, place?, hotel?, note?, guide?: {at, answers, text},
               items: [{id, at?, title, kind, place?, code?, note?, paperId?, reminderId?}]}],
       packing: [{id, text, done}],
       papers: [{id, title, date?, kind, text, fields?: Record<string,string>, itemId?}] }
```

`kind` de ítem y de papel: `flight | train | hotel | ticket | meal | visit | other`. Los papeles son **texto**
(pegado desde el correo en la web); sin PDF ni fotos en esta versión. Los ítems con hora se **espejan** al
calendario (`calendar.json`, eventos con `tripId`, como antes de 1.5.93; `normalizeCalendar` deja de descartarlos).

## Web (`/board` → pestaña Viajes, seis pestañas otra vez)

Alta (nombre, resumen de lugares, fechas, notas, activo), **cada día con su lugar y su hotel** (editables en la
tarjeta del día), agenda por día (hora, título, tipo, lugar, código, notas, papel), papeles (título, fecha, tipo,
texto pegado), lista para llevar, y en cada día su guía: leerla, "Generar la guía de este día" / "Rehacer" con
las preguntas del día como campos. Sin guía general.

## `viajes.lua` (pantallas; Diario afuera)

0 Sin viajes · 1 Inicio · 2 Hoy · 3 Agenda · 3b Día · 4 Ítem · 5 Papeles · 6 Lista (+ 6b Sugerencias) · 7 Guía
· 9 Preguntar · 10 Actualizar · 11 Cambiar de viaje. Como en `VIAJES_APP.md`, con estos cambios:

- **Guía POR DÍA, sólo a pedido.** En **Día (3b)** hay una fila **Guía de este día**: si está en la tarjeta
  (`guia-<date>.txt`) se abre con `cp.view`; si no, arranca el flujo: `viajes.guia.preguntas {id, date}` → cada
  pregunta con `cp.listen(15, texto)` (Atrás salta la pregunta) → `viajes.guia.generar` → progreso con
  `job.status` cada 5 s mostrando `label` → al `done`, `viajes.guia.dia` → `guia-<date>.txt` → `cp.view`. Con
  guía, una segunda fila **Rehacer la guía del día** repite el flujo sin confirmación. La pantalla **Guía (7)**
  es la lista de los días ("Día 3 · lun 16 · Barcelona", con marca si ya tiene guía) y OK hace lo mismo que la
  fila del Día. En Inicio la fila dice "Guía · 3 de 13 días". Hoy y Día muestran el lugar y el hotel del día.
  **Nunca una guía general.**
- **Preguntar (9)**: `cp.listen` → `viajes.preguntar` → `cp.view` de `answer` y `cp.say(spoken)`.
- **Archivos en `/Apps/data/viajes/`**: `viaje.json` (la vista compacta), `guia-<date>.txt`, `papel-<id>.txt`,
  `pendientes.json` (tildes hechos sin red, se reproducen en Actualizar). `cp.save` guarda `{tripId, updatedMs}`.
- **La app no levanta la red sola**: todo se lee de la tarjeta; la red la levanta Actualizar, la guía, Sugerir,
  Preguntar, Agregar por voz y Recordar (y **Sin viajes → Actualizar**).

## Puerta nueva: `cp.say(texto)`

`cp.say(texto)` → `id` o `nil`. El host pide `GET /api/tts?lang=&text=&max=45` y lo reproduce con `SpeechOut`
mientras la app sigue (la pantalla del visor puede estar abierta). Llega `on_reply(id, ok, {})` cuando el
audio empezó (o falló). Atrás corta la voz. En el harness: `fake.said` (lista de textos) y `on_reply` con `ok=true`.
