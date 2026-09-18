# Viajes — contrato entre servidor, web y `viajes.lua` (v1)

Decisiones del dueño (18-09-2026) sobre `VIAJES_APP.md`:

- **No hay Diario ni "libro del viaje".** Se sacan de la app y de la web.
- **La guía se guarda directo en la tarjeta**: sin pantalla de confirmación para bajar ni para rehacer.
- **Antes de generar la guía, la app PREGUNTA POR VOZ** lo que le falta al itinerario (hotel, hora de llegada,
  hora de salida, intereses…). El servidor mira lo cargado y decide qué preguntar; con esas respuestas genera.
- **Preguntar por voz sí**, con todo el viaje en el contexto del modelo, y la respuesta **hablada** además de
  escrita (puerta nueva `cp.say`).

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
| `viajes.guia.preguntas` | `{id}` | `{questions:[{key, text}]}` — 0 a 4 preguntas para hacer por voz (hotel, llegada, salida, intereses). Si el itinerario ya lo dice, no lo pregunta. |
| `viajes.guia.generar` | `{id, answers:{key:texto}}` | `{jobId}` — trabajo: 10 secciones con búsqueda en internet, progreso "Sección N de 10" |
| `viajes.guia.seccion` | `{id, n}` | `{n, title, text}` — texto plano listo para el visor (hasta 24 KB) |
| `viajes.preguntar` | `{id, question, date?, itemId?}` | `{answer, spoken}` — `spoken` es la versión corta para el parlante (≤ 220 caracteres) |
| `viajes.recordar` | `{id, itemId, on:bool}` | `{reminderId?}` — recordatorio 2 h antes en el store (suena como cualquier otro); `on=false` lo borra |
| `job.status` | `{id}` | el de siempre |

### La vista compacta (`viajes.viaje` → `trip`)

```json
{
  "id": "a1b2c3", "name": "Lisboa", "place": "Lisboa, Portugal", "start": "2026-09-14", "end": "2026-09-22",
  "when": "14 – 22 de septiembre", "hotel": "Hotel Lisboa Plaza", "weather": "Nublado · 19°",
  "today": "2026-09-16",
  "days": [
    {"date": "2026-09-14", "n": 1, "label": "sábado 14 de septiembre", "short": "sáb 14", "note": "",
     "items": [
       {"id": "i1", "at": "10:40", "title": "Vuelo IB6251 MAD → LIS", "kind": "flight", "kindLabel": "Vuelo",
        "place": "T4", "code": "ABC123", "note": "", "paperId": "p1", "remind": false}
     ]}
  ],
  "packing": [{"id": "k1", "text": "Pasaporte", "done": true}],
  "papers": [{"id": "p1", "date": "2026-09-14", "kind": "flight", "title": "Vuelo IB6251", "line": "loc. ABC123"}],
  "guide": {"ready": true, "at": 1758200000, "sections": [{"n": 1, "title": "Para entender el lugar"}]}
}
```

- `days` trae **todos** los días del viaje, vacíos incluidos, en orden; `label`/`short` ya en el idioma.
- `today` es la fecha civil de hoy en la zona del viaje (o `""` si el servidor no la sabe); la app la usa si el
  aparato no está en hora.
- `weather` puede venir `""`.
- Tope: la vista no pasa de **40 KB** (se recortan notas largas); la app la guarda entera en `viaje.json`.

### Datos (servidor, documento `trips`, resucitado de `d13923b^:server/src/trips.ts`)

```ts
Trip { id, name, place, lat?, lon?, timezone?, start, end, hotel?, notes?, active?: boolean,
       days: [{date, note?, items: [{id, at?, title, kind, place?, code?, note?, paperId?, reminderId?}]}],
       packing: [{id, text, done}],
       papers: [{id, title, date?, kind, text, fields?: Record<string,string>, itemId?}],
       guide?: {at, answers, sections: [{n, title, text}]} }
```

`kind` de ítem y de papel: `flight | train | hotel | ticket | meal | visit | other`. Los papeles son **texto**
(pegado desde el correo en la web); sin PDF ni fotos en esta versión. Los ítems con hora se **espejan** al
calendario (`calendar.json`, eventos con `tripId`, como antes de 1.5.93; `normalizeCalendar` deja de descartarlos).

## Web (`/board` → pestaña Viajes, seis pestañas otra vez)

Base: la sección `// ── Viajes` de `d13923b^:server/public/board/app.js` y las rutas `/api/trip*` de
`d13923b^:server/src/trips.ts`, sin adjuntos. Alta (destino con el buscador del clima, fechas, hotel, notas),
agenda por día (hora, título, tipo, lugar, código, notas, papel), papeles (título, fecha, tipo, texto pegado),
lista para llevar, guía (ver secciones, generar/rehacer con las mismas preguntas como campos), viaje activo.

## `viajes.lua` (pantallas; Diario afuera)

0 Sin viajes · 1 Inicio · 2 Hoy · 3 Agenda · 3b Día · 4 Ítem · 5 Papeles · 6 Lista (+ 6b Sugerencias) · 7 Guía
· 9 Preguntar · 10 Actualizar · 11 Cambiar de viaje. Como en `VIAJES_APP.md`, con estos cambios:

- **Guía (7)**: sin guía en la tarjeta la pantalla arranca sola el flujo: `viajes.guia.preguntas` → cada
  pregunta con `cp.listen(15, texto)` (Atrás salta la pregunta) → `viajes.guia.generar` → progreso con
  `job.status` cada 5 s → al terminar baja las 10 secciones con `viajes.guia.seccion` a `guia-N.txt` y muestra
  la lista. Última fila **Rehacer la guía**: mismo flujo, sin confirmación. Cada sección abre con `cp.view`.
- **Preguntar (9)**: `cp.listen` → `viajes.preguntar` → `cp.view` de `answer` y `cp.say(spoken)`.
- **Archivos en `/Apps/data/viajes/`**: `viaje.json` (la vista compacta), `guia-N.txt`, `papel-<id>.txt`,
  `pendientes.json` (tildes hechos sin red, se reproducen en Actualizar). `cp.save` guarda `{tripId, updatedMs}`.
- **La app no levanta la red sola**: todo se lee de la tarjeta; la red la levanta Actualizar, la guía, Sugerir,
  Preguntar, Agregar por voz y Recordar (y **Sin viajes → Actualizar**).

## Puerta nueva: `cp.say(texto)`

`cp.say(texto)` → `id` o `nil`. El host pide `GET /api/tts?lang=&text=&max=45` y lo reproduce con `SpeechOut`
mientras la app sigue (la pantalla del visor puede estar abierta). Llega `on_reply(id, ok, {})` cuando el
audio empezó (o falló). Atrás corta la voz. En el harness: `fake.said` (lista de textos) y `on_reply` con `ok=true`.
