# CrossPoint port — Waveshare ESP32-S3-ePaper-3.97 ("ws397")

Fork de [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) con soporte para la placa
Waveshare ESP32-S3-ePaper-3.97 (SSD1677 800x480, ESP32-S3-WROOM-1-N16R8, ES8311 + NS4150B, PMIC AXP2101-compatible,
RTC PCF85063, IMU QMI8658, SD 4-bit). El submódulo `freeink-sdk/` apunta al fork propio con el perfil de placa.

Idiomas del producto: español, inglés, francés, alemán, portugués y ruso (chino descartado). Todo string nuestro
va en los yaml de esos idiomas; el aparato manda
`lang` (`src/voice/Lang.h`) y el servidor escucha, contesta y traduce según ese idioma (`server/src/lang.ts`).

Idioma con el usuario: español rioplatense/mexicano, informal y directo. Respuestas cortas. Él prueba en hardware
y devuelve correcciones puntuales; no pedir que especifique todo de antemano.

REGLA FIJA: el aparato NUNCA tiene entrada por teclado (ni en pantalla ni físico). Toda pregunta o texto que el
usuario tenga que ingresar entra por voz (mic → servidor → transcripción). Las respuestas pueden ser texto en
pantalla. No usar `KeyboardEntryActivity` en nada nuestro.

## Estado (2026-09-04)

Funciona: boot, pantalla (orientación y polaridad correctas), botones, SD, WiFi, web UI, deep sleep,
batería vía PMIC, RTC, OTA desde servidor propio.

Pendiente de verificar en hardware: refresco periódico de un solo destello (parche `halfrefresh`), porcentaje de
batería real, hora tras apagado sin WiFi.

Audio verificado en hardware (1.5.9): graba y reproduce bien, se escucha bajo. Pendiente: control de volumen (DAC reg 0x32,
hoy fijo en 0xB2 = vendor 70 %; PGA del mic reg 0x14). Detalle: Settings → System → Audio test graba 3 s por el mic del ES8311 y
los reproduce; captura por `AudioManager::beginCapture`, DIN GPIO21, MCLK-fed init del vendor).

Pendiente de implementar (Fase 0): trackball + 2 botones vía PCF8574 en I²C (SDA 41 / SCL 42, INT GPIO44) cuando
llegue el hardware.

## Build y release

- `pio run -e ws397` — env en `platformio.ini`. Versión = `1.5.<WS397_BUILD>-ws397` desde `include/ws397_version.h`
  (NO ponerla en un -D flag: fuerza rebuild completo).
- `.\release.ps1` (Windows) o `./release.sh` (Linux / Claude Code web) — bump del build, compila, sube el .bin al
  servidor. Solo el .ps1 acepta `-Usb COMx` para flashear por cable; en la nube el aparato se actualiza por OTA.
- Después de cada release, commitear `include/ws397_version.h` y `.ws397-build` para que el siguiente build
  parta del número correcto.
- En la nube no hay hardware: pedirle al usuario que pruebe en el aparato y reporte.
- En la nube (Claude Code web): `setup-cloud.sh` trae los workarounds del proxy (registro de PlatformIO y
  `github.com/*/archive` bloqueados; SCons y las libs se traen de PyPI/GitHub). Sin `WS397_OTA_URL` y
  `WS397_OTA_TOKEN` en el environment no hay release: el .bin queda con la URL de OTA vacía y no se puede subir.
- OTA: el aparato consulta `WS397_OTA_URL` (`https://paper-esp32.up.railway.app/firmware/latest`, JSON con la forma
  de un release de GitHub). Comparación estricta major.minor.patch.
- Proveedor de IA configurable desde la web (`server/src/config.ts` → `/data/config.json`, `server/src/llm.ts`):
  Anthropic (Claude) o cualquier API compatible con OpenAI (Groq gratis, DeepSeek barato, OpenAI). `chatText()` y
  `chatJson()` son lo único que usan `ask.ts` y `voice.ts`; en Anthropic el esquema va por `output_config` y el
  capítulo por `cache_control`, en las compatibles el esquema se explica en el system y se pide `json_object`.
  La transcripción sale de la misma config (`stt.baseUrl/model/key`). Las claves se guardan en el volumen y **no
  se devuelven nunca** por la API (solo `hasKey`). El token del aparato también se puede cambiar desde la web; el
  `DEVICE_TOKEN` del entorno sigue valiendo siempre para no quedar afuera.
- **Servidor: `server/` en este mismo repo** (Bun + Hono; Railway con Root Directory = `server`, volumen en
  `/data`; rutas, variables y despliegue en `server/README.md`). Todo cambio de servidor va ahí, no en archivos
  sueltos. `bun install && bunx tsc --noEmit` en `server/` como chequeo.
- Compile checks rápidos sin toolchain: `g++ -std=c++17 -fsyntax-only` con stubs de Arduino/Wire (ver historial).

## Decisiones de hardware (freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h, perfil `WS397`)

- Panel: secuencias del vendor `EPD_3in97.cpp` = config Sticky (FULL 0xF7, PARTIAL 0xFF, borde 0x01/0x80).
  HALF = 0xD7 con temp 0x6A (vendor "Fast", un solo ciclo). El X4 default (0xFC) deja ghosting.
- Regla del panel: refresco completo cada 10-15 parciales. No desactivar.
- Botones: UP=GPIO4, OK=GPIO5, DOWN=GPIO6, BOOT=GPIO0 (back). Estilo `DigitalConfirmPowerHold`: OK = confirm +
  power (hold duerme, press despierta por EXT1). BOOT NO se usa para despertar (strap de arranque).
- PMIC: AXP2101-compatible en 0x34 (IC_TYPE 0x4A). SoC en reg 0xA4, VBAT 0x34/0x35, estado en STATUS2. Solo se tocan
  bits de medición; rieles y corriente de carga se dejan como los configuró el PMIC.
- RTC: PCF85063 en 0x51, bloque de hora en 0x04, OS flag = seconds bit7. INT en GPIO45 (futuro wake por alarma).
- Audio: ES8311 en 0x18 (I²S bclk 14, ws 47, dout 48, din 21, mclk 13), amp enable GPIO39 (compartido con IMU
  INT1; IMU va por polling). Mic es analógico al ES8311 (MIC1), capturado por el ADC del códec sobre el mismo puerto
  I²S (full duplex, una sola tasa para reproducir y grabar); no PDM. Init del códec = vendor `es8311_init` con MCLK
  desde el pin (reg01 0x3F), volumen 0xB2.
- Sensores: SHTC3 en 0x70 (sin driver aún), QMI8658 en 0x6B.

## Servidor propio (Fase 1, cliente HTTP común)

- `lib/ServerClient/`: `ServerCredentialStore` (`/.crosspoint/server.json`: URL del servidor y token del aparato,
  editables en la web UI → Servidor; URL vacía = origen de `WS397_OTA_URL`) y `ServerClient` (singleton
  `SERVER_CLIENT`): `get`/`postJson`/`postOrQueue` con `Authorization: Bearer <token>`, JSON, 3 intentos con
  backoff (500/1500 ms) ante fallo de transporte, 429 y 5xx, header `X-Request-Id` estable entre reintentos.
- Cola offline: `/.crosspoint/server-queue.json` (máx. 50 POSTs, 4 KB c/u); `flushQueue()` la reproduce en orden
  con WiFi arriba y descarta lo que el servidor rechaza con 4xx. Las llamadas son síncronas: van desde una
  Activity de red (WiFi solo está arriba ahí) o desde una tarea propia, nunca desde el render.
- Diagnóstico: Settings → System → Prueba de servidor (`ServerTestActivity`): `/firmware/latest` sin token,
  `/api/ping` con token, y vacía la cola.
- Contrato que el Hono tiene que cumplir: `GET /api/ping` con Bearer válido → 200 `{"ok":true}`; sin token o
  token inválido → 401. Los endpoints de features van bajo `/api/…` con el mismo Bearer y pueden usar
  `X-Request-Id` para deduplicar reintentos.

## Preguntarle al libro (Fase 1)

- Menú del lector → "Preguntarle al libro" (`MenuAction::ASK_BOOK`). `EpubReaderActivity::launchAskBook()` junta el
  texto leído hasta la página actual (`Section::getTextUpToPage`, últimas páginas, máx. 24 KB) y la página actual,
  guarda el progreso, suelta el libro y reemplaza el lector por `AskBookActivity` (mismo esquema que KOReader sync:
  WiFi + TLS necesitan el heap del libro; al salir `silentRestartToReader()`).
- `AskBookActivity`: popup con preguntas predefinidas + "Preguntar por voz" (graba por el mic hasta 10 s, OK
  termina; WAV 16 kHz mono en PSRAM), WiFi, `POST /api/transcribe` (body `audio/wav`, timeout 60 s) → texto de la
  pregunta, `POST /api/ask` (timeout 90 s) con `{book, chapter, text, page, question, lang}`, y la respuesta en
  `DictionaryDefinitionActivity` (paginada, la pregunta transcripta como título). Back en el popup vuelve al lector.
- Servidor: `src/api.ts` monta `ask` y `transcribe` bajo `/api` (heredan el Bearer). `src/ask.ts`:
  `@anthropic-ai/sdk`, `ANTHROPIC_API_KEY` en Railway, modelo `claude-haiku-4-5` por defecto (`ASK_MODEL` para
  cambiarlo; el código no usa parámetros específicos de modelo), texto del capítulo en el system prompt con
  `cache_control`; responde con lo leído y, si no alcanza, con conocimiento general aclarándolo; nunca adelanta
  trama (sin spoilers); respuestas cortas en texto plano. Devuelve `{ok, answer, model, usage}`.
  `src/transcribe.ts`: STT por un endpoint compatible con OpenAI (`STT_API_KEY` o `OPENAI_API_KEY`, `STT_BASE_URL`
  default `https://api.openai.com/v1`, `STT_MODEL` default `whisper-1`; Groq sirve con
  `https://api.groq.com/openai/v1` + `whisper-large-v3-turbo`). Devuelve `{ok, text}`.

## Hub (Fase 1)

- `src/activities/home/HubActivity.{h,cpp}`: home de la ws397. `ActivityManager::goHome()` con `Board::WS397` abre
  el hub (salvo cuando vuelve de browser/recents/OPDS/transfer, que caen en la home clásica);
  `goToClassicHome()` es la home original de CrossPoint (mosaico Leer) y su Back vuelve al hub.
- Mosaicos 3x3 (Leer, Hablar, Traductor, Recordatorios, Tiempo, Notas, Biblia, Música, Ajustes) con íconos Lucide de 48 px generados en
  `src/components/icons/hubIcons.h` (manifest al lado; `gen_icons.py` del SDK, en la nube con `resvg-py` en vez de
  rsvg-convert). Barra de estado: hora del RTC (`--:--` si no está en hora), batería, WiFi si hay link. Widget
  "Continuar leyendo" con el último libro; Back en el hub lo abre. Recordatorios/Biblia/Música muestran
  "Próximamente". El reloj se repinta solo cuando cambia el minuto.
- Preguntar = `AskBookActivity` en modo general (constructor sin libro): graba de entrada, `POST /api/ask` sin
  `text`; el Hono responde con conocimiento general (`generalPrompt`). Back vuelve al hub con `silentRestart()`.
- Sincronización (`HubSyncActivity`, `src/HubStore.{h,cpp}` → `/.crosspoint/hub.json`): `GET /api/hub` devuelve
  `{ok, now, weather{line,detail}, reminders[{title,when}], events[{when,title}], messages[{from,text}], quote}`.
  Se dispara al entrar al hub con caché de más de 6 h (reintento a la hora si falló; sin RTC solo la primera vez),
  manteniendo Atrás 1,2 s en el hub, o desde Settings → Sincronizar hub. Pone en hora el RTC con `now` del servidor
  si difiere más de 2 min (`HalClock::getEpochUtc/setFromEpochUtc`), vacía la cola offline y termina con
  `silentRestart()`. Servidor: `src/hub.ts` (Open-Meteo con `HUB_LAT`/`HUB_LON`/`HUB_TZ`; recordatorios, agenda y
  mensajes de `/data/hub-data.json` hasta la Fase 2; frase del día de una lista).
- Lugar del clima por voz (`HubLocationActivity`, Settings → Lugar del clima): graba, `POST /api/transcribe`,
  `GET /api/hub/location/search?q=` (geocoding de Open-Meteo, lista de candidatos en un `OptionPopup`),
  `POST /api/hub/location` y pasa a `HubSyncActivity` con el WiFi ya arriba. El servidor lo guarda en
  `/data/hub-settings.json`; `HUB_LAT`/`HUB_LON` quedan de respaldo.
- Hablar (`VoiceActivity`, mosaico Hablar): graba hasta 12 s, `POST /api/voice` (audio/wav, 90 s) →
  `{ok, text, intent, reply, saved[]}`; si guardó algo, `HubSyncActivity::fetchNow()` refresca la caché y el título
  del visor dice qué guardó; una pregunta se muestra con lo entendido como título. Back → hub con `silentRestart()`.
  Servidor: `src/voice.ts` (transcribe con `transcribeWav` de `transcribe.ts`, clasifica con salida estructurada de
  Claude, modelo `VOICE_MODEL`/`ASK_MODEL` default `claude-haiku-4-5`, ejecuta contra `src/store.ts` →
  `/data/store.json`: recordatorios, listas por nombre con Entrada/Casa/Trabajo/Administrativo/Compras de fábrica,
  notas, mensajes). `hub.ts` toma recordatorios y mensajes del store; `hub-data.json` queda para la agenda.
- Recordatorios y listas en el aparato (`AgendaActivity`, mosaico Recordatorios): secciones (Recordatorios y cada
  lista con su cantidad) e ítems desde la caché de `HubStore` (`reminders[{id,title,when}]`, `lists[{name,
  items[{id,text}]}]` que trae `GET /api/hub`); OK tilda: se saca de la caché y `POST /api/hub/done` sale por
  `postOrQueue` (cola offline si no hay WiFi, la vacía la próxima sincronización).
- Recordatorios que suenan (`ReminderAlertActivity`): `HubStore::Reminder.dueAt` (epoch del servidor). Al dormir,
  `armReminderWake()` en main.cpp arma el timer de deep sleep al próximo `dueAt` (GPIO45 del RTC no es RTC GPIO, no
  sirve para despertar); al arrancar por timer, si hay uno vencido se muestra el alerta y si no vuelve a dormir. En el
  hub, el tick de 15 s también lo dispara. OK = hecho (`POST /api/hub/done`), Atrás = posponer 10 min (`snooze` en
  el mismo POST); el servidor corre los repetidos al próximo ciclo.
- Listas: Atrás largo sobre un ítem abre el menú (mover, fecha, borrar) → `POST /api/hub/edit` por `postOrQueue`.
  Notas (`NotesActivity`): lista, OK abre, Atrás largo borra. Tiempo (`TimerActivity`): temporizador, cronómetro y
  Pomodoro con dígitos de 7 segmentos; `VoiceActivity` lo abre directo cuando el servidor devuelve `timerSeconds`.
  Pitido común en `src/voice/AlertBeep`.
- Reglas del temporizador (lo que estaba roto hasta 1.5.32): **Atrás sale y lo deja corriendo**, solo Atrás largo (1 s)
  cancela; el estado vive en `HubStore` como tiempo absoluto (`timerEndAt`, `timerPausedLeft` para la pausa,
  `stopwatchStartAt`/`stopwatchAccumS` para el cronómetro, `timerRound` para el pomodoro), así que sobrevive a salir,
  dormir y reiniciar. `preventAutoSleep()` solo es true mientras suena (con tope de 3 min): dormir es lo correcto,
  porque el que lo hace sonar es el wake por deep sleep. El hub lo muestra en la barra de estado en minutos (repinta
  solo cuando cambia, si no fantasmea el panel) y con un punto en el mosaico Tiempo.
- Alarmas desde cualquier pantalla: `checkTimeAlarms()` en el loop de `main.cpp` (cada 5 s) abre `TimerActivity` o
  `ReminderAlertActivity` sobre las pantallas tranquilas (hub, home, agenda, notas, ajustes, clima), no solo desde el
  tick del hub. Todo camino de deep sleep pasa por `sleepNow()`, que arma el wake: antes el re-sleep por wake espurio
  del botón dormía sin nada armado y el temporizador quedaba mudo para siempre.
- OJO con los botones: en esta placa OK es confirm+power compartidos, así que `wasLongPressed(Confirm, ...)` NUNCA es
  cierto (mantener OK apaga). Las funciones que estaban colgadas de "OK largo" no existían: el Clima quedó como
  mosaico propio (en el lugar de Juegos, que decía "Próximamente").
- Atajo de voz global: **dos toques de Atrás** abren Hablar desde cualquier pantalla tranquila
  (`checkVoiceShortcut()` en el loop de `main.cpp`, ventana de 500 ms). ARRIBA/ABAJO es una palanca física
  (arriba XOR abajo, nunca las dos), OK es el botón de encendido y Atrás mantenido ya sincroniza o actualiza.
  Atrás en el hub no hace nada: el hub es el fondo (antes abría el último libro y no había forma de quedarse).
- OJO con el audio: el I2S es uno solo y cada clase (`SpeechOut`, `AlertBeep`, `VoiceRecorder`) tiene su propio
  `AudioManager`. Abrir el micrófono mientras habla el parlante da "Falló la captura del micrófono", y navegar
  mientras habla corta la frase. Regla: `speech.stop()` antes de grabar, y si hay que hacer algo después de hablar,
  esperar a `!speech.isPlaying()` (estado `SPEAKING` de `VoiceActivity`).
- Avisos hablados: los clips de Piper se cachean en `/.crosspoint/tts/<hash>.bin`, con el hash de
  (voz + idioma + texto) — `src/voice/SpeechCache.h`. El servidor manda la voz en uso en `ttsVoice` de `GET /api/hub`;
  cambiar la voz cambia el nombre del archivo, así que el clip viejo no se puede reusar (antes el aviso del
  temporizador siguió con la voz masculina vieja para siempre). La sincronización borra los clips que ya no están
  en la lista.
- Listas con barra de botones: el margen de abajo va `buttonHintsHeight + verticalSpacing`, no solo el alto de los
  hints, o la última fila queda pegada a los botones y parece tapada.
- TTS (`server/src/tts.ts`, Piper en el Dockerfile con una voz por idioma; español = `es_MX-claude-high`, femenina neutra): `POST /api/voice` devuelve un cuerpo
  binario `[u32 LE largo JSON][JSON][ADPCM]` (`application/x-ws397-voice`); `VoiceActivity` lo parte, decodifica
  (`src/voice/Adpcm`) y reproduce (`src/voice/SpeechOut`) mientras muestra el texto. `GET /api/tts?text=&lang=`
  da clips para los avisos; `HubSyncActivity::cacheSpokenNotices()` guarda los de los próximos 5 recordatorios y
  la frase del temporizador en `/.crosspoint/tts/`. El aparato nunca decodifica MP3 para la voz.
- Traductor (`TranslatorActivity`, app propia): elige el otro idioma (guardado en `HubStore::translatorLang`), OK =
  hablo yo, Arriba = habla el otro, Abajo = cambiar idioma; `POST /api/translate?from=&to=` (`server/src/translate.ts`,
  mismo cuerpo binario que `/api/voice`) y la traducción se lee con Piper en el idioma de destino.
- Página web `GET /board` (`server/src/board.ts`), con pestañas: Pizarra (mensajes, recordatorios, memoria), Listas,
  Notas, Fotos, Noticias, **IA** (proveedor, modelo, claves, token), Ajustes (clima, idioma, voz, volumen) y Log.
  Todo desde el teléfono con el token del aparato; altas en
  `POST /api/board/*`, borrados por `POST /api/hub/edit {kind, id, action:"delete"}` (kind = reminder, item, note,
  feed, memory). Los mensajes llegan por `GET /api/hub` (`messages[{id,from,text}]`) y se ven en Recordatorios →
  Mensajes (OK = leído, `POST /api/hub/done {kind:"message"}`).
  El token se pide en un formulario de la propia página (no `prompt()`) y se guarda en `localStorage`; los botones
  de las listas van por delegación con `data-act`, nunca por `onclick` armado con comillas (una comilla escapada
  dentro del template literal rompía el script entero y dejaba la página muerta).
- Ajustes desde la web (`store.ts` → `settings{rev,lang,speak,musicVolume,translatorLang}`, `POST /api/board/settings`):
  viajan en `GET /api/hub` y `HubStore::applySettings` los aplica solo si `rev` subió respecto de `settingsRev`, así
  lo que se cambia en el aparato no se pisa en cada sincronización. El idioma lo aplica `HubSyncActivity::applyUiLanguage()`
  sobre `SETTINGS.language`. El lugar del clima también se elige ahí (buscador → `POST /api/hub/location`); sin lugar
  guardado ni `HUB_LAT`/`HUB_LON`, el clima llega vacío.
- Ajuste Voz hablada (Settings → Sistema, `HubStore::speakMode`): `&speak=none|short|all` en `/api/voice`.
- Biblia (`BibleActivity`, mosaico Biblia): `GET /api/bible/books|chapter|book|find|day?lang=` (`server/src/bible.ts`,
  JSON de thiagobodruk/bible bajado en el Dockerfile a `/opt/bible`, nombres de libros por idioma en
  `bibleNames.ts`); capítulos sueltos cacheados en `/.crosspoint/bible/<lang>/<libro>-<cap>.txt`, último lugar en
  `HubStore::bibleBook/bibleChapter`; Atrás largo graba y `find` resuelve referencia o búsqueda de texto.
- Biblia entera en la SD: la última fila de la lista de libros la baja completa (`GET /api/bible/book`, un archivo
  por libro en `/.crosspoint/bible/<lang>/bNN.txt` con "#<capítulo>" y los versículos numerados; 3,8 MB en español,
  un libro por pasada del loop para que la pantalla siga viva). Con eso, leer y **buscar** funcionan sin WiFi:
  `parseRefLocal()` resuelve la cita ("primera de Juan 4 8") con los nombres que ya están en la SD y `searchStep()`
  busca todas las palabras en cada versículo, un libro por pasada. Lo único que sigue necesitando el servidor es
  pasar la voz a texto. Lógica probada de escritorio con `g++` contra el archivo real de Juan.
- Música (`MusicActivity`, mosaico Música), con pinta de Winamp pero al tamaño de esta pantalla (480x800): título y
  artista grandes, contador de 7 segmentos de 68 px, barra de posición gruesa, botones de transporte de 56x38 y
  volumen con número; la playlist va en filas de 38 px. La versión anterior copiaba las proporciones de la skin
  original (275x116) y en el aparato quedaba todo minúsculo. MP3 de `/Music/<carpeta>/` en la SD. `src/music/Mp3Source` decodifica con
  Helix (`lib/HelixMp3`, C puro, RPSL) dentro del `read()` de una `AudioManager::WavSource` con cabecera WAV
  sintética, así el SDK no cambia; tags ID3v2/v1; volumen en `HubStore::musicVolume`. Pausa = volumen 0.
- Noticias (`NewsActivity`, mosaico Noticias): `GET /api/rss` y `/api/rss/article` (`server/src/rss.ts`, feeds que se
  cargan en `/board`, artículo limpiado a texto sin LLM); titulares y artículos leídos cacheados en `/.crosspoint/rss/`.
  El hub pasa a 3x4: Leer, Hablar, Traductor, Recordatorios, Tiempo, Notas, Biblia, Música, Noticias, Fotos, Juegos,
  Ajustes (Juegos todavía dice "Próximamente").
- Fotos (`PhotosActivity`, mosaico Fotos): `GET /api/photos` y `/api/photos/file?id=` (`server/src/photos.ts`). La
  foto se sube **tal como sale del teléfono** y la convierte el servidor con `sharp` (`toDeviceBmp`: rota por EXIF,
  escala a 480x800, 4 grises con Floyd-Steinberg y BMP de 2 bpp, ~150 ms); el navegador ya no arma nada. El aparato
  pide la lista al servidor cada vez que se entra, baja a `/Photos` de la SD y dibuja con el **pipeline de grises**
  del SDK (base BW + pasada LSB + pasada MSB + `displayGrayBuffer`): una sola pasada en modo BW pintaba de negro todo
  lo que no fuera blanco puro y la foto salía como una mancha.
- Clima: Open-Meteo primero y **met.no de respaldo** (`server/src/metno.ts`, mismo formato traducido con
  `wmoFromSymbol`, User-Agent obligatorio): desde Railway Open-Meteo devolvía 502 sin parar y el hub quedaba vacío.
- Clima detallado (`WeatherActivity`, mosaico Clima): `GET /api/hub/forecast?lang=` (Open-Meteo: ahora, horas y seis
  días), último pronóstico cacheado en `/.crosspoint/forecast.json` con `savedAt`; si tiene más de una hora se
  refresca solo al entrar y Atrás mantenido lo fuerza. Un 503 del servidor (no hay lugar cargado) se muestra como
  "cargá el lugar en la web", no como error genérico. El widget del hub sale de `HUB_STORE.weatherLine` (de
  `GET /api/hub`), que es otra fuente: si viene vacío con `weather.noPlace`, el hub también dice que falta el lugar,
  y `shouldAutoSync()` reintenta a la hora en vez de esperar el ciclo entero.
- Log: `src/util/DeviceLog` engancha `setLogSink` de `lib/Logging` y guarda cada línea en `/.crosspoint/device.log`
  (rota a 64 KB); `HubSyncActivity` lo sube con `POST /api/log` (también cuando la sincronización falla, que es
  cuando más sirve) y se lee en `/board/log` **con el token** (`GET /api/log`): ahí adentro están los nombres de las
  redes WiFi y todo lo que se dicta por voz, así que la página no puede ser pública.
- El audio de subida va en ADPCM (`adpcm::encode`), una cuarta parte de un WAV: es lo que más tardaba. El servidor
  acepta `audio/adpcm` o `audio/wav` (`toWav` en `transcribe.ts`) y devuelve tiempos por etapa en `ms`.
- Voz común: `src/voice/VoiceRecorder` (toma de hasta N s a PSRAM, `start/pump/stop/abort`, pitidos al abrir y cerrar el mic) y
  `src/voice/SpeechToText::transcribe` (`POST /api/transcribe`). Toda Activity que grabe usa eso.
- Widgets: clima, próximo recordatorio, agenda de hoy (o la frase si no hay eventos), contador de mensajes en la
  barra. Íconos de 24 px en `src/components/icons/hubWidgetIcons.h`. Pendiente: temperatura interior (SHTC3).

## Roadmap acordado

La lista completa de funciones, con fase, estado y contrato del servidor, está en `docs/ws397/FUNCIONES.md`
(fusión de lo planeado con lo que hacen el reTerminal Sticky y el ZecTrix Note 4). Resumen:

0. Hardware: volumen, trackball/botones PCF8574, wake por alarma del RTC, driver SHTC3, deep sleep medido, IMU
   por polling (boca abajo = silenciar, doble golpe = PTT, sacudir = cancelar; modo atril horizontal para el hub).
1. Hub + preguntarle al libro + cliente HTTP + sincronización con `GET /api/hub` y widgets + Hablar con
   clasificador de intención (hecho). Falta: pizarra de mensajes desde el teléfono (1.8), ajustes del hub en la web UI.
2. Voz: el servidor clasifica la intención de una sola grabación (pregunta, tarea, recordatorio, compras,
   nota, mensaje, temporizador, traducción, alarma); recordatorios con repetición y alarma del RTC; varias
   listas de tareas (Entrada, Casa, Trabajo, Administrativo, Compras, proyectos) con vista por semana ISO;
   TTS con Piper (hecho); traductor en modo conversación (hecho); temporizador, cronómetro y Pomodoro (hecho); agenda; memoria.
3. Contenido: Biblia (hecha, capítulos cacheados; falta descarga por libro e índice offline), MP3 estilo Winamp
   (hecho), versículo/frase del día, RSS/lectura web, álbum de imágenes en 4 grises, clima detallado.
4. Juegos: damas, cartas (rummy, solitario, blackjack), retos mentales (sudoku, acertijos, cálculo), memoria
   (parejas, Simón), Tetris experimental, ajedrez opcional.

Descartado: radio por streaming, Casa Cerebro, lectura en voz alta de libros, Spotify (DRM; solo Connect online
con cspot, no offline), auto-rotación por IMU, chino.

Principios: un solo botón de voz (PTT) desde cualquier pantalla; respuesta escrita siempre y hablada cuando
aporta; todo funciona sin WiFi con la caché de la SD; CrossPoint sigue siendo el lector y lo nuestro entra
como Activities en el hub; todo lo pesado (STT, LLM, TTS, render) en el servidor; audio y red en tareas
FreeRTOS separadas de la UI; el aparato nunca guarda claves de Anthropic.

## Convenciones

- Commits: prefijo `ws397:`. Cambios al SDK en el submódulo, con su propio commit.
- Los commits ws397 del SDK (perfil, waveform, battery, rtc, wake, halfrefresh) están exportados como `.patch` en
  `docs/ws397/` (`git format-patch`); si al submódulo le falta alguno, `git am docs/ws397/NNNN-*.patch` dentro de
  `freeink-sdk/`. Regenerarlos cuando se agregue un commit al SDK.
- No tocar la lógica upstream fuera de lo necesario para la placa; preferir `case Board::WS397` sobre `#if`.
- Antes de un release: `pio run -e ws397` limpio y probar en hardware.
