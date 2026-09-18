# Viajes — la app de Lua (propuesta, pantalla a pantalla)

Pedido del dueño: calendario de viaje, lugar, documentos tipo vouchers, guía (mejores lugares, restaurantes,
reseñas, historia de la ciudad, "todo lo que necesita una mente curiosa"), **lista de cosas** y **agenda de viaje
diaria del día uno al último, con fechas**. Decisiones ya tomadas (PLAN_APPS_VIAJES_EPUB.md): los vouchers se
guardan como TEXTO extraído, la guía busca en internet sola, Sonnet 5 para la guía.

## Reglas de la casa que mandan sobre el diseño

- **La app no levanta la red sola** (decisión de 1.5.105). Todo se lee de la tarjeta (`/Apps/data/viajes/`), y
  la red se levanta sólo cuando el usuario elige algo que la necesita: Actualizar, bajar la guía, preguntar,
  sugerir, dictar. Lo que se toca sin red (tildar, dictar el diario) se guarda local y sube en el próximo Actualizar.
- **Sin teclado**: todo lo que se escribe entra por voz o desde la web. La carga gruesa del viaje (destino,
  fechas, ítems, papeles) se hace en `/board` → Viajes, que vuelve como pestaña.
- **Botones de una app de Lua**: ARRIBA/ABAJO (palanca), OK y ATRÁS. No hay pulsaciones largas: Atrás mantenido
  1 s SIEMPRE sale de la app (regla del host). Por eso "Preguntar por voz" es una fila, no un gesto.
- Lo que espera (red, micrófono) lo hace el firmware con sus pantallas de "Escuchando…" / "Conectando…"; la app
  sólo dibuja sus pantallas y recibe los resultados por callback.

## Pantallas del aparato (`viajes.lua`)

Convención de todas: la palanca mueve el resalte, OK abre o elige, Atrás vuelve a la pantalla anterior (en
Inicio, sale). Cada pantalla tiene su pie con lo que hacen los botones. Listas largas se paginan solas.

### 0. Sin viajes
"No hay viajes cargados. Cárgalos en la web: /board → Viajes." Una fila: **Actualizar** (levanta la red y baja
la lista). Atrás sale.

### 1. Inicio — el viaje activo
```
 Lisboa
 14 – 22 de septiembre · faltan 12 días        ← o "Día 3 de 9 · martes 16"
 Nublado · 19° en Lisboa                       ← de la última actualización
 ─────────────────────────────────────────
 > Hoy · martes 16 · 3 cosas                   ← antes del viaje: "Día 1 · sábado 14 · vuelo 10:40"
   Agenda · 9 días
   Papeles · 6
   Lista para llevar · 7 de 12
   Guía · 10 secciones                         ← o "Guía · no bajada"
   Diario · 2 notas
   Preguntar por voz
   Actualizar · hace 3 h                       ← o "nunca"
   Cambiar de viaje (2)                        ← sólo con más de un viaje
 OK: abrir · Atrás: salir
```
OK en cada fila abre su pantalla. Atrás sale de la app. Sin reloj en el aparato no hay "faltan N días" ni "Hoy": la
fila dice "Hoy · el aparato no está en hora".

### 2. Hoy
Cabezal "Martes 16 de septiembre · día 3 de 9". Lo de hoy en orden de hora (`09:00 Desayuno Hotel Plaza`, `11:30
Castillo de San Jorge · entrada 12€`, `20:30 Cena Cervejaria Ramiro · reserva a nombre de…`), el clima del día,
y debajo dos filas: **Dictar en el diario** y **Preguntar sobre hoy**. OK sobre un ítem abre el Ítem (4). Atrás →
Inicio.

### 3. Agenda — del día 1 al último
```
 Agenda · Lisboa                    Página 1 de 3
 ─────────────────────────────────────────
 Día 1 · sábado 14 de septiembre
   10:40  Vuelo IB6251 MAD → LIS · T4     📎
   15:00  Check-in Hotel Lisboa Plaza     📎
 Día 2 · domingo 15 de septiembre
   — libre —
 Día 3 · lunes 16 de septiembre
   11:30  Castillo de San Jorge
   …
 Día 9 · domingo 22 de septiembre
   12:10  Vuelo IB6252 LIS → MAD
 OK: abrir · Atrás: volver
```
Todos los días aparecen aunque estén vacíos ("— libre —"): el índice de días es el viaje entero. La palanca
recorre días e ítems; OK sobre un día abre el Día (3b), OK sobre un ítem abre el Ítem (4). El clip marca que hay
papel. Atrás → Inicio.

**3b. Día**: cabezal con la fecha, la nota del día si la hay (de la web), los ítems, y filas **Dictar en el
diario de este día** y **Preguntar sobre este día** ("¿qué hago cerca del castillo después?"). Atrás → Agenda.

### 4. Ítem
Título grande ("Vuelo IB6251 Madrid → Lisboa"), fecha y hora, lugar/terminal/puerta, código de reserva en grande
(es lo que se muestra en el mostrador), notas. Filas: **Ver el papel** (abre el texto extraído del voucher en el
visor paginado del sistema; si el papel tiene varios campos, van como tabla de "etiqueta: valor" primero y el
texto completo después), **Recordar 2 h antes** (OK alterna; crea o borra el recordatorio en el servidor, que
suena como cualquier otro, con RTC y todo), **Preguntar sobre esto**. Atrás → Agenda.

### 5. Papeles
Lista de todos los vouchers del viaje por fecha: `14 sep · Vuelo IB6251 · loc. ABC123`, `14 sep · Hotel Lisboa
Plaza · check-in 15:00`, `17 sep · Tren Lisboa – Sintra · 2 asientos`. OK abre el texto extraído en el visor. Es
la pantalla del aeropuerto: **funciona sin WiFi** porque cada papel está en la tarjeta. Atrás → Inicio.

### 6. Lista para llevar
```
 Para llevar · 7 de 12
 ─────────────────────────────────────────
 ☑ Pasaporte
 ☑ Cargador
 ☐ Adaptador de enchufe (tipo F)
 ☐ Protector solar
 …
   Agregar por voz
   Sugerir con IA
 OK: tildar · Atrás: volver
```
OK tilda o destilda (se guarda en la tarjeta y sube en el próximo Actualizar o en la próxima llamada). **Agregar
por voz** pregunta "¿Qué agregamos?"; se pueden decir varias cosas de corrido ("cargador, adaptador y protector
solar") y el servidor las separa; "quita el paraguas" también vale. **Sugerir con IA** pide al servidor una
lista según destino, fechas, clima esperado y agenda (vuelo → "auriculares, almohada"; playa → "protector") y
abre **6b. Sugerencias**: filas con casilla vacía, OK agrega cada una, Atrás vuelve con las agregadas. Lo que la
IA sugiere NUNCA se agrega solo. Borrar definitivo: desde la web.

### 7. Guía
Diez secciones, cada una un archivo de texto en la tarjeta que abre el visor paginado:

1. Para entender el lugar (historia en épocas, idioma, moneda y cambio, cómo se saluda)
2. Barrios (dónde dormir, dónde no ir de noche)
3. Imperdibles (qué ver y **por qué**, con el dato para contar después)
4. Para una mente curiosa (lo que no está en las guías)
5. Comer (platos y dónde, con reseñas recientes; horarios; qué se pide y qué no)
6. Moverse (aeropuerto → centro, transporte, tarjetas, taxis, propinas)
7. Ojo con (estafas, zonas, clima de ESAS fechas, feriados que caen en el viaje)
8. Un día perfecto (itinerario a pie del primer día libre)
9. **Frases útiles** (veinte frases con pronunciación figurada, para leer en el mostrador)
10. **Por si acaso** (número de emergencias, embajada/consulado, hospital cercano al hotel, cómo bloquear la
    tarjeta, dónde está la comisaría de turistas)

Con la guía no bajada la lista tiene una sola fila, **Bajar la guía**: si todavía no existe en el servidor se
genera (trabajo con barra "Sección 3 de 10", tarda unos minutos, usa búsqueda web) y después baja una sección por
vez. Al final de la lista, **Rehacer la guía** pasa por una pantalla de confirmación que dice lo que cuesta.
Atrás → Inicio.

### 8. Diario
Notas dictadas, una fila por nota con día y primeras palabras (`mar 16 · "El castillo a la mañana, sin fila…"`).
Filas: **Dictar (hoy)**. OK sobre una nota la abre en el visor. Se guardan en la tarjeta en el acto y suben con
Actualizar; se leen también en la web. Atrás → Inicio.
(Más adelante: "Armar el libro del viaje" = un EPUB con agenda + diario + fotos del teléfono, con el motor del
Librito. No va en la primera versión.)

### 9. Preguntar por voz
"¿Qué quieres saber del viaje?" → el servidor contesta con la agenda, los papeles y la guía del viaje en el
contexto del modelo (cacheado con `cache_control`): "¿a qué hora es el check-in?", "¿qué como cerca del hotel el
martes?", "¿cómo se dice la cuenta por favor?". La respuesta se abre en el visor. **Hablada**: hace falta una
puerta nueva chica, `cp.say(texto)` (TTS del servidor → parlante, lo mismo que hace Hablar); entra en esta fase.
Desde Hoy, Día e Ítem la pregunta ya sabe de qué día o de qué cosa se habla.

### 10. Actualizar
Pantalla de progreso: "Agenda · Papeles 3 de 6 · Guía al día". Sube lo pendiente (tildes, diario) y baja sólo lo
que cambió (manifiesto con sha por archivo, como el paquete de noticias). Al terminar vuelve a Inicio. Es la
ÚNICA vez que la app levanta la red por sí misma, y porque el usuario lo pidió.

### 11. Cambiar de viaje
Lista de viajes con fechas ("Lisboa · 14–22 sep", "Cusco · 3–10 nov"); OK lo hace el activo y vuelve a Inicio.

## La web (`/board` → Viajes)

- **Alta**: destino (el buscador de lugares del clima), fechas de ida y vuelta, notas. Varios viajes, uno activo.
- **Agenda por día**: la web muestra los días del uno al último con sus fechas y se agrega en cada uno: hora,
  título, tipo (vuelo, hotel, tren, reserva, visita, comida, otro), lugar, código, notas, papel adjunto.
- **Papeles**: se sube el PDF, la foto o se pega el correo. El servidor lo lee (`pdf-parse` para PDF, el modelo
  con visión para fotos) y guarda **el texto estructurado** (aerolínea, vuelo, terminal, hora, localizador,
  dirección, check-in, cancelación, teléfono). Con eso **propone** el ítem de agenda correspondiente, que se
  confirma con un toque. El original queda en el servidor para verlo desde el teléfono.
- **Lista para llevar** y **Diario**: se editan y se leen ahí también.
- **Guía**: generar, leer, rehacer; con el costo de la última generación a la vista (búsquedas y tokens).
- Los ítems con hora se **espejan al calendario** del aparato (marcados con `tripId`, se borran con el viaje), así
  "Mi día" y el hub dicen "hoy 10:40 vuelo a Lisboa" sin abrir la app.

## Servidor (`server/src/viajes.ts`, bajo `/api/apps/call`)

`viajes.lista`, `viajes.estado` (manifiesto con sha de agenda, papeles, guía, lista, diario → ids de archivo para
`cp.download`), `viajes.llevar` (tildar / agregar por texto / quitar), `viajes.sugerir`, `viajes.guia` (trabajo),
`viajes.preguntar`, `viajes.diario`, `viajes.recordar`. Todo con la clave de las apps y contado en `apps_calls`.

## Orden de construcción

1. Web + servidor: alta, agenda, papeles con extracción, lista, diario, espejo al calendario (Playwright a 360 px;
   extracción con PDFs de muestra).
2. `viajes.lua` con Inicio, Hoy, Agenda, Día, Ítem, Papeles, Lista, Diario, Actualizar, Cambiar de viaje, con su
   escenario en `test/lua_sandbox/scenarios/viajes.lua` (viaje de nueve días de prueba).
3. Guía (`viajes.guia` con búsqueda) y Preguntar con `cp.say`.
Cada paso termina en un release y una prueba en el aparato.
