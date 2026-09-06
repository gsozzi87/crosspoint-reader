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
| 0.5 | Wake por recordatorio: el INT del PCF85063 (GPIO45) no es RTC GPIO y no puede despertar del deep sleep, así que se usa el timer de deep sleep del ESP32 armado al próximo recordatorio de la caché (`armReminderWake`). La alarma del RTC queda para cuando haya un pin RTC libre | ✅ | 1.5.19 |
| 0.6 | Driver SHTC3 (temperatura y humedad interior) | ⬜ | Para el widget de clima, como el Sticky |
| 0.7 | Porcentaje de batería real y consumo en deep sleep medidos | ⬜ | |
| 0.8 | Refresco de un solo destello (halfrefresh) verificado | ⬜ | |
| 0.9 | Driver QMI8658 por polling (INT1 está compartido con el amp) y gestos: **boca abajo** = silenciar alarma o temporizador y posponer; **doble golpe** = PTT alternativo; **sacudir** = cancelar la grabación o deshacer el último ítem | ⬜ | Se usa en 2.2, 2.8, 2.9 |
| 0.10 | Modo atril: el hub, los widgets y el álbum en horizontal cuando el aparato está apoyado de costado (IMU). El lector siempre vertical | ⬜ | Opcional, se activa en Settings |
| 0.11 | Auto-rotación del lector por IMU | ❌ | |

## Fase 1 · Hub y servidor (base hecha)

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 1.1 | Cliente HTTP común: token, reintentos, cola offline, `X-Request-Id` | ✅ | `lib/ServerClient` |
| 1.2 | Prueba de servidor en Settings | ✅ | |
| 1.3 | Preguntarle al libro (texto leído → Claude, sin spoilers) | ✅ | |
| 1.4 | Pregunta por voz general desde el hub | ✅ | 1.5.14, modo general de `AskBookActivity` |
| 1.5 | Hub con mosaicos, barra de estado (hora, batería, WiFi), "Continuar leyendo" | ✅ | 1.5.14 |
| 1.6 | Sincronización con el servidor: al entrar al hub con la caché de más de 6 h (reintento a la hora si falló), manteniendo Atrás en el hub, y desde Settings → Sincronizar hub. Trae `GET /api/hub` (clima, agenda, mensajes, recordatorios, frase) a `/.crosspoint/hub.json`, pone en hora el RTC con el reloj del servidor y vacía la cola offline | ✅ | 1.5.15. Falta el timed wake del RTC (0.5) para sincronizar dormido |
| 1.7 | Widgets del hub: clima exterior, próximo recordatorio, agenda de hoy (o frase del día si no hay eventos), contador de mensajes en la barra | ✅ | 1.5.15. Falta la temperatura interior del SHTC3 (0.6) |
| 1.8 | Pizarra y "app del teléfono": página web del Hono (`/board`, pide el token una vez) para dejar mensajes en el hub, crear recordatorios con fecha y repetición, agregar ítems a las listas, escribir notas, y tildar o borrar. En el aparato los mensajes aparecen en Recordatorios → Mensajes y se marcan leídos con OK; el contador va en la barra del hub | ✅ | 1.5.23 |
| 1.9 | Mosaico "Hablar" (PTT) que reemplaza a "Preguntar": una grabación, `POST /api/voice`, el servidor clasifica y ejecuta; la respuesta o la confirmación se muestra paginada y el hub se resincroniza | ✅ | 1.5.17 (`VoiceActivity`) |
| 1.10 | Ajustes del hub: orden de mosaicos y widgets, hora de sincronización, unidad °C/°F | ⬜ | Web UI |
| 1.11 | Lugar del clima por voz: Settings → Lugar del clima, decís la ciudad, el servidor transcribe y geocodifica (Open-Meteo), elegís de la lista y queda guardado en el servidor; el hub se resincroniza | ✅ | 1.5.16 |

## Fase 2 · Voz, recordatorios y listas

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 2.1 | Clasificador de intención en el Hono (`POST /api/voice`): audio → texto → JSON con esquema (Claude, salida estructurada) → guarda en `store.json` y devuelve `{intent, reply, saved[]}` | ✅ | 1.5.17, `voice.ts` + `store.ts`. Temporizador y alarma se reconocen pero avisan que todavía no se ejecutan |
| 2.2 | Recordatorios: título, fecha y hora, repetición (diaria, semanal, mensual); se crean por voz, se ven y tildan en el mosaico Recordatorios; **suenan**: `ReminderAlertActivity` con pitidos por el parlante, OK = hecho, Atrás = 10 min más; despierta del deep sleep por timer y también salta si el hub está en pantalla; los repetidos pasan al próximo ciclo al tildarlos | ✅ | 1.5.19. Falta: que salte también dentro del lector y de otras pantallas |
| 2.3 | **Varias listas de tareas.** Entrada, Casa, Trabajo, Administrativo y Compras de fábrica; las demás se crean por voz. Mosaico Recordatorios → lista de listas con pendientes; adentro OK tilda y Atrás largo abre el menú del ítem: mover a otra lista, poner fecha (hoy, mañana, semana que viene, sin fecha), borrar. Todo local más `POST /api/hub/edit` por la cola offline | ✅ | 1.5.20 |
| 2.3b | **Vista por semana.** Todo ítem con fecha cae en su semana ISO; la vista "Semana" agrupa por día lo de todas las listas y permite moverlo a la siguiente. "Para la semana que viene: renovar el seguro" y "el jueves: mandar el informe" caen solos en su semana | ⬜ | Reemplaza a las listas por número de semana que llevás a mano |
| 2.3c | Voz para listas: "agregá a la casa: cambiar el foco", "en trabajo: …", "compras: leche y huevos" (dos ítems). Sin lista nombrada el clasificador elige por contexto o lo deja en Entrada para ordenarlo después | ⬜ | |
| 2.3d | Importación de las listas actuales (texto, Excel o lo que uses) al servidor, y edición desde la página web del Hono | ⬜ | |
| 2.4 | Lista de compras: es una lista más, de tipo compras: ítems sueltos, se vacía de a uno o entera | ⬜ | Como el Sticky |
| 2.5 | Notas: se dictan por Hablar, mosaico Notas las lista (dos líneas), OK abre la nota completa, Atrás largo la borra | ✅ | 1.5.20 |
| 2.6 | TTS con Piper en el mismo Railway (sin tokens): un proceso por idioma con el modelo cargado (~0,2 s por frase), WAV → 16 kHz → IMA ADPCM (4 s = 32 KB). La respuesta de Hablar viaja en el mismo cuerpo que el JSON y suena al mismo tiempo que aparece el texto; se habla toda confirmación y traducción, y las respuestas a preguntas solo si son cortas. Los avisos ("Recordatorios: título", "¡Tiempo!") se bajan en la sincronización a `/.crosspoint/tts/` y suenan sin WiFi, antes del pitido | ✅ | 1.5.21; ajuste Voz hablada (Nunca / Solo cortas / Siempre) en Settings → Sistema desde 1.5.23 |
| 2.7 | **Traductor, app propia** (mosaico Traductor), además del "traducí…" de Hablar. Modo conversación: elegís el otro idioma, OK = hablo yo (idioma de la UI → otro), Arriba = habla el otro (otro → mío); cada toma se muestra en los dos idiomas y se lee en voz alta en el idioma de destino con Piper. `POST /api/translate?from=&to=` (audio → JSON + ADPCM) | ✅ | 1.5.22 |
| 2.8 | Mosaico Tiempo: temporizador (1 a 60 min), cronómetro y Pomodoro (25/5 con rondas), dígitos grandes de 7 segmentos, OK pausa/sigue, Atrás vuelve, pitido por el parlante al terminar; refresco parcial por segundo con limpieza cada 40. Por voz: "poné 10 minutos" o "alarma a las 7" abre el temporizador con el tiempo ya puesto | ✅ | 1.5.20. El temporizador vive solo mientras la pantalla está abierta (no sigue en deep sleep) |
| 2.9 | Alarma / despertador: "alarma a las 7", "despertame a las 6 y media todos los días" se guarda como recordatorio (con repetición si la pide) y usa el mismo timer wake: suena aunque el aparato esté dormido; OK apaga, Atrás pospone 10 min | ✅ | 1.5.24 |
| 2.10 | Agenda del día desde un calendario ICS (`HUB_ICS_URL`: Google, Apple, Outlook, Nextcloud; varias URLs separadas por coma): eventos de hoy en el widget (o los de mañana si hoy no queda nada), repeticiones diarias y semanales expandidas | ✅ | 1.5.24, `server/src/agenda.ts`; sin URL sigue `hub-data.json` |
| 2.11 | Memoria del asistente: "acordate que la patente es AB123CD", "tené presente que Ana es alérgica al maní" → se guarda (hasta 100 datos) y entra en el prompt de Hablar y de Preguntar; se ven y borran desde la página web | 🔧 | 1.5.24; falta verlas y borrarlas en `/board` |
| 2.12 | Sonidos del sistema por el parlante: pitido corto al abrir el mic (agudo) y al cerrarlo (grave) en toda grabación, pitidos de recordatorio y temporizador (`AlertBeep`) | ✅ | 1.5.24 |

## Fase 3 · Contenido

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 3.1 | Biblia (mosaico Biblia): libros → capítulos → texto paginado; cada capítulo se trae del servidor (Reina-Valera, King James, Bible de l'Épée, Schlachter, Almeida, Sinodal según el idioma) y queda en la SD (`/.crosspoint/bible/<lang>/`), marcado con un punto en la lista; recuerda el último lugar leído. Atrás largo = por voz: "Juan 3 16" abre directo, "salmo 23" también, "misericordia y verdad" busca en todo el texto y lista los versículos (sin LLM: parser de referencias y búsqueda en el servidor) | ✅ | 1.5.25. Falta: descargar un libro entero para leer sin WiFi y la búsqueda offline con índice en SD |
| 3.2 | Versículo del día (`GET /api/bible/day`) y frase del día en el hub | 🔧 | Servidor listo; falta mostrarlo en el hub |
| 3.3 | Reproductor MP3 desde la SD **con pinta de Winamp**: ventana principal con contador grande de tiempo, título del tema en la marquesina, kbps y kHz, barra de posición, botones ⏮ ▶ ⏸ ⏹ ⏭, shuffle y repeat, volumen; playlist debajo con numeración y duración, y el tema actual resaltado; carpetas de la SD como playlists. Todo en blanco y negro con el estilo de bordes biselados del skin clásico. Decodificación con `ESP32-audioI2S` en una tarea del core 1, botones y trackball para navegar | ⬜ | Música y audiolibros propios; sin streaming |
| 3.4 | RSS y lectura web: el servidor baja y limpia el artículo, el aparato lo muestra con el paginador del diccionario | ⬜ | Fuentes configuradas en la web UI |
| 3.5 | Álbum de imágenes: el servidor convierte a 4 grises 800x480, el aparato las guarda en la SD y las muestra como salvapantallas de sueño o en un mosaico | ⬜ | Del Sticky y el Note 4; fotos, tarjetas, texto empujado |
| 3.6 | Clima detallado: pronóstico de varios días, adentro vs. afuera | ⬜ | |
| 3.7 | Radio por streaming, dashboard Casa Cerebro, lectura en voz alta de libros | ❌ | |
| 3.8 | Spotify: las playlists descargadas en la app están cifradas con DRM y solo las reproduce la app, no se pueden copiar a la SD. Lo único posible es Spotify Connect online con `cspot` (ESP32-S3 con PSRAM, cuenta Premium, WiFi arriba), sin offline | ❌ | Descartado por ahora; la música offline es 3.3 con MP3 propios en la SD |

## Fase 4 · Juegos

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| 4.1 | Damas contra la máquina (motor local, minimax corto) | ⬜ | Tablero 8x8, cursor con trackball o UP/DOWN + OK |
| 4.2 | Cartas: rummy (contra la máquina), solitario Klondike, blackjack; chinchón y escoba si sobra tiempo | ⬜ | Baraja dibujada a 4 grises |
| 4.3 | Retos mentales: sudoku, acertijos y trivia que manda el servidor (por voz se responde), cálculo mental, secuencias lógicas | ⬜ | Los acertijos se cachean en la SD |
| 4.4 | Memoria: parejas (Memory), Simón (secuencias con sonido por el parlante), recordar listas de palabras o números con puntaje | ⬜ | |
| 4.5 | Tetris: viable a velocidad baja con refresco parcial (4 grises, refresco completo cada tantas piezas); necesita el trackball o los botones extra para izquierda/derecha/girar | ⬜ | Experimental; 2048 como alternativa más apta para la tinta |
| 4.6 | Ajedrez contra el servidor | ⬜ | Opcional |

## Idiomas

El aparato y el servidor hablan español, inglés, francés, alemán, portugués y ruso. El idioma
se elige en Settings → Idioma; el aparato manda su código (`lang`) en cada llamada y el servidor escucha
(transcripción), contesta y etiqueta el clima y las fechas en ese idioma. El traductor traduce desde el idioma
de la UI al que se pida (si no se dice: al inglés, o al español si la UI está en inglés).

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| L.1 | Strings nuestros traducidos a es, en, fr, de, pt-BR, pt-PT, ru | ✅ | 1.5.18. Los de CrossPoint upstream ya estaban |
| L.2 | Servidor multi-idioma: STT, prompts, clima, fechas, frases del día | ✅ | 1.5.18, `server/src/lang.ts` |
| L.3 | Chino mandarín | ❌ | Sacado: la UI no tiene fuente CJK y no vale la complejidad |
| L.4 | Nombres de las listas de fábrica en el idioma de la UI | ⬜ | Hoy son Entrada, Casa, Trabajo, Administrativo, Compras |

## Transversal

| # | Función | Estado | Notas |
|---|---------|--------|-------|
| T.1 | OTA desde el servidor propio, versión estricta | ✅ | |
| T.2 | Web UI en el aparato: WiFi, servidor y token, libros | ✅ | Se le suman los ajustes del hub |
| T.3 | Página web en el Hono con el token: mandar mensajes, ver y editar recordatorios y listas, subir imágenes | ⬜ | Sustituye a la app del teléfono de Sticky y Note 4 |
| T.6 | Servidor completo en `server/` de este repo (Bun + Hono): OTA, ask, transcribe, hub, voice, store. Railway con Root Directory = `server` | ✅ | 1.5.18, ver `server/README.md` |
| T.4 | Modo bajo consumo: deep sleep con wake por botón, RTC y sincronización programada | ⬜ | |
| T.5 | Idiomas: español e inglés en toda la UI nuestra | ✅ | |

## Contrato del servidor (a agregar bajo `/api`, Bearer del aparato)

- `GET /api/hub` → `{clock, weather, reminders[], events[], messages[], quote}` para la caché del hub.
- `POST /api/voice` (audio/wav) → `{ok, intent, text, reply, speak?, reminder?, items?}`.
- `GET/POST/PATCH/DELETE /api/reminders`, `/api/todos`, `/api/shopping`, `/api/notes`, `/api/messages`.
- `POST /api/tts` (`{text}`) → audio.
- `GET /api/bible/day`, `GET /api/rss`, `GET /api/rss/:id`, `GET /api/images`.
- `GET/POST /api/lists`, `GET/POST/PATCH/DELETE /api/lists/:id/items`, `GET /api/week/:iso` (ítems de la semana).
- `GET /api/games/riddles`, `GET /api/games/sudoku`.
