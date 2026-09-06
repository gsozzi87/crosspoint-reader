# Funcionalidades del firmware ws397

Fusión de lo que ya teníamos planeado con lo que hacen el reTerminal Sticky (Seeed) y el ZecTrix Note 4,
adaptado a nuestra placa: 800x480 sin touch, 3 botones + BOOT (trackball y 2 botones por PCF8574 cuando
llegue), ES8311 con mic y parlante, RTC PCF85063 con INT, SHTC3, QMI8658, SD, sin NFC. Reglas fijas: sin
teclado nunca (todo ingreso por voz), lo pesado en el servidor Hono, el aparato solo guarda un token propio.

Estado: ✅ hecho · 🔧 en curso · ⬜ pendiente · ❌ descartado.

## Principios de interacción

- **Un solo botón de voz (PTT).** Como el botón AI del Sticky y el Note 4: mantener graba, soltar manda.
  Hoy es el mosaico "Hablar" del hub; cuando llegue el PCF8574, un botón físico dedicado que funciona
  desde cualquier pantalla (también desde el lector, donde pregunta sobre el libro).
- **El servidor clasifica la intención.** Una sola grabación; el Hono transcribe y decide si es tarea,
  recordatorio, lista de compras, nota, mensaje, temporizador, traducción o pregunta. El aparato no tiene
  menú de "qué tipo de cosa querés decir".
- **Respuesta escrita siempre, hablada cuando aporta.** Todo se muestra en pantalla; el parlante lee
  respuestas cortas y avisos (recordatorio, temporizador). Lectura en voz alta de libros: descartada.
- **Todo funciona sin WiFi con lo último guardado.** Los widgets muestran la caché de la SD; lo que se
  crea sin red va a la cola offline y se sube después. WiFi solo se levanta en la sincronización y en las
  acciones de red, nunca en el render.
- **Tinta electrónica.** Refresco completo cada 10-15 parciales, reloj repintado solo al cambiar el
  minuto, sincronización programada y no continua.

## Fase 0 · Hardware

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 0.1 | Boot, pantalla, botones, SD, WiFi, web UI, deep sleep, batería, RTC, OTA | ✅ | |
| 0.2 | Audio: grabar y reproducir por el ES8311 | ✅ | 1.5.9 |
| 0.3 | Control de volumen del parlante y ganancia del mic | ⬜ | DAC reg 0x32, PGA reg 0x14; ajuste en Settings |
| 0.4 | Trackball + 2 botones por PCF8574 (I²C 41/42, INT 44) | ⬜ | Un botón = PTT, el otro = Home/Back |
| 0.5 | Wake por alarma del RTC (INT GPIO45) | ⬜ | Base de recordatorios y de la sincronización programada |
| 0.6 | Driver SHTC3 (temperatura y humedad interior) | ⬜ | Para el widget de clima, como el Sticky |
| 0.7 | Porcentaje de batería real y consumo en deep sleep medidos | ⬜ | |
| 0.8 | Refresco de un solo destello (halfrefresh) verificado | ⬜ | |
| 0.9 | Auto-rotación por IMU | ❌ | El lector es vertical; no vale la complejidad |

## Fase 1 · Hub y servidor (base hecha)

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 1.1 | Cliente HTTP común: token, reintentos, cola offline, `X-Request-Id` | ✅ | `lib/ServerClient` |
| 1.2 | Prueba de servidor en Settings | ✅ | |
| 1.3 | Preguntarle al libro (texto leído → Claude, sin spoilers) | ✅ | |
| 1.4 | Pregunta por voz general desde el hub | ✅ | 1.5.14, modo general de `AskBookActivity` |
| 1.5 | Hub con mosaicos, barra de estado (hora, batería, WiFi), "Continuar leyendo" | ✅ | 1.5.14 |
| 1.6 | Sincronización con el servidor: al despertar, al volver al hub cada N horas y a mano. Trae `GET /api/hub` (clima, agenda, mensajes, recordatorios, frase) y guarda en `/.crosspoint/hub.json`; vacía la cola offline | ⬜ | Timed wake del RTC como el Sticky |
| 1.7 | Widgets del hub: clima (exterior del servidor + interior del SHTC3), próximo recordatorio, próximos eventos de la agenda, última frase leída | ⬜ | Se pintan desde la caché |
| 1.8 | Pizarra de mensajes: texto que alguien manda desde el teléfono (página web del Hono con el token, o Telegram) y aparece en el hub; se marca leído con OK | ⬜ | Equivalente al "message board" del Sticky |
| 1.9 | Mosaico "Hablar" (PTT) que reemplaza a "Preguntar": una grabación, el servidor clasifica | ⬜ | Base de la Fase 2 |
| 1.10 | Ajustes del hub: orden de mosaicos y widgets, hora de sincronización, unidad °C/°F | ⬜ | Web UI |

## Fase 2 · Voz, recordatorios y listas

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 2.1 | Clasificador de intención en el Hono (`POST /api/voice`): audio → texto → `{intent, payload}` con Claude; devuelve la acción hecha y el texto para pantalla y parlante | ⬜ | Intenciones: pregunta, tarea, recordatorio, compras, nota, mensaje, temporizador, traducción, alarma |
| 2.2 | Recordatorios: título, fecha y hora, repetición (diaria, semanal por día, mensual), prioridad; se guardan en el servidor y en la SD; el aparato programa la alarma del RTC más cercana y despierta para avisar (pantalla + pitido o voz) | ⬜ | Modelo tomado del Note 4 |
| 2.3 | Lista de tareas: agregar por voz, tildar con OK, borrar con OK largo, pendientes arriba | ⬜ | |
| 2.4 | Lista de compras: separada de las tareas, ítems sueltos, se vacía de a uno o entera | ⬜ | Como el Sticky, "comprá leche y huevos" → dos ítems |
| 2.5 | Notas y memos: texto dictado, lista paginada, borrar | ⬜ | |
| 2.6 | TTS: el servidor devuelve audio (MP3 o WAV) y el aparato lo reproduce; respuesta hablada opcional en las preguntas y obligatoria en avisos | ⬜ | `POST /api/tts`, tarea de audio en core 1 |
| 2.7 | Traductor: "traducí al inglés …" o "cómo se dice …"; muestra y lee la traducción | ⬜ | |
| 2.8 | Temporizador y Pomodoro: "poné 10 minutos", "pomodoro"; cuenta en pantalla con repintado por minuto y avisa por el parlante | ⬜ | Local, sin servidor; del Note 4 |
| 2.9 | Alarma / despertador: hora fija diaria, suena por el parlante, OK apaga, Back pospone | ⬜ | RTC |
| 2.10 | Agenda del día: eventos que el servidor saca de un calendario (ICS o Google) y muestra en el widget y en su app | ⬜ | Solo lectura |
| 2.11 | Memoria del asistente: el servidor guarda las últimas preguntas y datos que el usuario le dicte ("acordate que …") | ⬜ | Del Note 4 ("AI memory") |
| 2.12 | Sonidos del sistema por el parlante: inicio y fin de grabación, confirmación, error | ⬜ | Reemplaza al buzzer del Sticky |

## Fase 3 · Contenido

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 3.1 | Biblia: índice invertido pregenerado en la SD, búsqueda en milisegundos sin WiFi, navegación por libro, capítulo y versículo, pedido por voz ("Juan 3 16", "buscá misericordia") | ⬜ | |
| 3.2 | Versículo del día y frase del día en el hub | ⬜ | Del servidor, con caché |
| 3.3 | Reproductor MP3 desde la SD (`ESP32-audioI2S`, tarea en core 1), carpetas, play/pausa, siguiente, volumen | ⬜ | Música y audiolibros propios; sin streaming |
| 3.4 | RSS y lectura web: el servidor baja y limpia el artículo, el aparato lo muestra con el paginador del diccionario | ⬜ | Fuentes configuradas en la web UI |
| 3.5 | Álbum de imágenes: el servidor convierte a 4 grises 800x480, el aparato las guarda en la SD y las muestra como salvapantallas de sueño o en un mosaico | ⬜ | Del Sticky y el Note 4; fotos, tarjetas, texto empujado |
| 3.6 | Clima detallado: pronóstico de varios días, adentro vs. afuera | ⬜ | |
| 3.7 | Radio por streaming, dashboard Casa Cerebro, lectura en voz alta de libros, Spotify | ❌ | |

## Fase 4 · Juegos

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 4.1 | Sudoku (tablero generado en el servidor o en la SD, entrada con trackball) | ⬜ | |
| 4.2 | Cartas (solitario) | ⬜ | |
| 4.3 | Ajedrez contra el servidor | ⬜ | Opcional |

## Transversal

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| T.1 | OTA desde el servidor propio, versión estricta | ✅ | |
| T.2 | Web UI en el aparato: WiFi, servidor y token, libros | ✅ | Se le suman los ajustes del hub |
| T.3 | Página web en el Hono con el token: mandar mensajes, ver y editar recordatorios y listas, subir imágenes | ⬜ | Sustituye a la app del teléfono de Sticky y Note 4 |
| T.4 | Modo bajo consumo: deep sleep con wake por botón, RTC y sincronización programada | ⬜ | |
| T.5 | Idiomas: español e inglés en toda la UI nuestra | ✅ | |

## Contrato del servidor (a agregar bajo `/api`, Bearer del aparato)

- `GET /api/hub` → `{clock, weather, reminders[], events[], messages[], quote}` para la caché del hub.
- `POST /api/voice` (audio/wav) → `{ok, intent, text, reply, speak?, reminder?, items?}`.
- `GET/POST/PATCH/DELETE /api/reminders`, `/api/todos`, `/api/shopping`, `/api/notes`, `/api/messages`.
- `POST /api/tts` (`{text}`) → audio.
- `GET /api/bible/day`, `GET /api/rss`, `GET /api/rss/:id`, `GET /api/images`.
