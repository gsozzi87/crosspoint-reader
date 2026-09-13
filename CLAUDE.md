# CrossPoint port — Waveshare ESP32-S3-ePaper-3.97 ("ws397")

Fork de [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) con soporte para la placa
Waveshare ESP32-S3-ePaper-3.97 (SSD1677 800x480, ESP32-S3-WROOM-1-N16R8, ES8311 + NS4150B, PMIC AXP2101-compatible,
RTC PCF85063, IMU QMI8658, SD 4-bit). El submódulo `freeink-sdk/` apunta al fork propio con el perfil de placa.

Idiomas del producto: español, inglés, francés, alemán, portugués y ruso (chino descartado). Todo string nuestro
va en los yaml de esos idiomas; el aparato manda
`lang` (`src/voice/Lang.h`) y el servidor escucha, contesta y traduce según ese idioma (`server/src/lang.ts`).

REGLA FIJA: los strings que ve el usuario en el aparato y en la web van en **español neutro**, no rioplatense:
"elige" y no "elegí", "presiona" y no "apretá", "puedes" y no "podés", "encuentra" y no "encontrá". El voseo es
solo para hablar con el usuario en el chat, nunca para el producto.

Idioma con el usuario: español rioplatense/mexicano, informal y directo. Respuestas cortas. Él prueba en hardware
y devuelve correcciones puntuales; no pedir que especifique todo de antemano.

REGLA FIJA: el aparato NUNCA tiene entrada por teclado (ni en pantalla ni físico). Toda pregunta o texto que el
usuario tenga que ingresar entra por voz (mic → servidor → transcripción). Las respuestas pueden ser texto en
pantalla. No usar `KeyboardEntryActivity` en nada nuestro.

## Estado (2026-09-10)

Funciona: boot, pantalla (orientación y polaridad correctas), botones, SD, WiFi, web UI, deep sleep,
batería vía PMIC, RTC, OTA desde servidor propio, audio (graba y reproduce), música, voz, hub.

**Pendiente de verificar en hardware (1.5.47, lo más nuevo y lo más riesgoso):**

- **PWR por el PMIC** (`src/util/PowerKey`): toque corto = limpiar pantalla, mantener 3 s = dormir con la
  barrita. La polaridad del flanco se APRENDE en caliente y se loguea (`press edge = …` en `/board/log`),
  porque ni la hoja de datos ni el proyecto de referencia coinciden. Si el botón no responde, ahí está la
  respuesta. El corte duro del PMIC está programado a los 10 s como escape de emergencia.
- **Coordinador de refresco** (`lib/GfxRenderer/PanelRefreshCoordinator`): 12 parciales → HALF, cada 2 HALF
  un FULL, y una sombra de 48 KB en PSRAM que saltea el pintado cuando el cuadro es idéntico. Falta medir si
  0xD7 restaura el blanco o hace falta bajar `CLEANS_BEFORE_FULL`.
- **Pre-roll de grabación**: el micrófono abre ANTES del pitido y las muestras del tono se recortan
  (`spokenStart`). Falta confirmar que la primera palabra ya no se pierde.
- **IMU**: hay que calibrar los ejes en Ajustes → Movimiento antes de creerle a los gestos, y confirmar que
  el motor de golpes del chip contesta (la pantalla lo dice).
- Lo de siempre: porcentaje de batería real, hora tras apagado sin WiFi.

Descartado: trackball + 2 botones vía PCF8574. El usuario decidió que el aparato va con la palanca y el
botón del costado, y nada más ("me acomodé bien con la palanca y el botón del costado").

## Build y release## Build y release

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
- Volumen: el registro 0x32 del ES8311 es logarítmico (dB = -95,5 + 0,5·N) y `setVolume()` mapeaba el porcentaje
  lineal sobre N, así que el 70 % quedaba en -23 dB (de ahí que "se escuchara bajo"). Ahora el porcentaje se mapea a
  decibeles: 1 % = -40 dB, 100 % = +8 dB (0xCF), 0 = mudo, y el 70 % cae justo en 0xB2, el default del vendor.
  Hay **un solo volumen** para todo (`HubStore::musicVolume`): lo usan la música, la voz de Piper y los pitidos, y se
  toca desde la música o desde /board → Ajustes.
- Temperatura interior: `src/util/Shtc3` (I²C 0x70, wakeup 0x3517 / medición 0x7866 / sleep 0xB098, CRC del
  datasheet, caché de un minuto). El SDK no maneja el SHTC3 en esta placa (`SensorsConfig` mapea un SHT40), así que
  va con `Wire` directo sobre el bus de los sensores. Se ve en el widget del clima del hub como "Interior 23°".
- Audio: ES8311 en 0x18 (I²S bclk 14, ws 47, dout 48, din 21, mclk 13), amp enable GPIO39 (compartido con IMU
  INT1; IMU va por polling). Mic es analógico al ES8311 (MIC1), capturado por el ADC del códec sobre el mismo puerto
  I²S (full duplex, una sola tasa para reproducir y grabar); no PDM. Init del códec = vendor `es8311_init` con MCLK
  desde el pin (reg01 0x3F), volumen 0xB2.
- Sensores: SHTC3 en 0x70 (sin driver aún), QMI8658 en 0x6B.

## Modo memoria USB (la tarjeta como disco)

- `-DFREEINK_CAP_USB_MSC=1 -DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=1` en el env `ws397`. La variante prebuilt
  `qio_opi` que usa esta placa ya trae TinyUSB con `CONFIG_TINYUSB_MSC_ENABLED=1`, así que **no** hay que cambiar
  `board_build.arduino.memory_type` (el X4 Pro lo hace por otro motivo).
- `ARDUINO_USB_MODE=0` cambia el tipo de `Serial` de `HWCDC` a `USBCDC`; `lib/Logging/Logging.h` lo contempla ahora
  (una referencia del tipo equivocado no compila). El log por cable sigue saliendo por USB CDC, pero acá el log que
  importa es el que se sube al servidor y se lee en `/board/log`.
- Se llega por **Ajustes → Sistema → Modo memoria USB** (`SettingAction::UsbDrive` → `UsbDriveActivity` del SDK) y
  también por Transferir archivos, donde en las placas con MSC la memoria USB es la **primera** opción
  (`menuModes[]` en `NetworkModeSelectionActivity`: el orden de la pantalla dejó de coincidir con el de
  `NetworkMode`). Cuesta ~23 KB de RAM y ~60 KB de flash.
- Con esto se cargan libros y MP3 sin sacar la tarjeta. La subida de archivos por la web quedó **descartada**.

## Identidad del aparato y cuentas (multiusuario)

- El aparato se genera su token solo, la primera vez que arranca: 32 bytes de `esp_random` en hexa
  (`ServerCredentialStore::ensureToken()`, llamado desde `setup()`), guardados en `/.crosspoint/server.json`.
  `ServerCredentialStore::deviceId()` es la MAC de fábrica en hexa y es la identidad **pública**.
- **El token NO se deriva de la MAC**, ni siquiera por HMAC: eso obliga a meter un secreto de fábrica en el
  firmware, y cualquiera que baje un `.bin` lo saca y calcula el token de cualquier aparato a partir de su MAC —
  que va impresa en la caja. Perder el token no pierde datos: los datos son de la CUENTA, así que se vuelve a
  vincular con el código y listo.
- Vincular sin teclado (`DevicePairActivity`, Ajustes → Sistema → Vincular con mi cuenta): `POST /api/pair/start`
  `{deviceId, token}` **sin Bearer** → `{ok, code, expiresIn}`; la pantalla muestra el código de seis dígitos en
  dígitos de segmentos y consulta `GET /api/pair/status` cada 3 s hasta que del otro lado lo escriban en
  `/board` → Aparatos. `ServerClient::postJson` acepta `auth=false` justamente para el primero (un Bearer que el
  servidor todavía no conoce daría 401 antes de llegar al handler).

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
  `/data/store.json`: recordatorios, DOS listas fijas —compras y tareas—, notas). `hub.ts` toma los recordatorios y
  las listas del store; `hub-data.json` queda para la agenda. Las listas viajan con `key` (clave canónica del store,
  la que hay que devolver al mover un ítem) y `name` (el nombre ya traducido, el que se muestra).
- Recordatorios y listas en el aparato (`AgendaActivity`, mosaico Recordatorios): tres secciones y nada más —
  Recordatorios, Compras y Tareas—, con sus ítems desde la caché de `HubStore` (`reminders[{id,title,when}]`,
  `lists[{key,name,items[{id,text}]}]` que trae `GET /api/hub`); OK tilda: se saca de la caché y `POST /api/hub/done`
  sale por `postOrQueue` (cola offline si no hay WiFi, la vacía la próxima sincronización). **Las categorías de
  listas se sacaron en 1.5.44** ("son muchas cosas"): el servidor migra solo lo que hubiera en Entrada, Casa,
  Trabajo, Administrativo o en proyectos sueltos a la lista de tareas.
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
- **OK mantenido: la razón por la que "nunca llegaba" se había vuelto falsa.** Hasta 1.5.46 era cierto y tenía
  explicación: con `InputStyle::DigitalConfirmPowerHold` el SDK NO levanta el bit de Confirm mientras la tecla está
  abajo (`updateConfirmPowerHold` sólo emite un clic sintético al soltar), así que `isPressed(Confirm)` era siempre
  false y `wasLongPressed` no podía dar true ni queriendo. En 1.5.47 el encendido pasó al PMIC y la placa cambió a
  `InputStyle::DigitalButtons`, donde `getDigitalState()` sí levanta el bit mientras se mantiene — pero el atajo
  quedó: `EpubReaderActivity::confirmLongPressThreshold()` tenía un `if (WS397) return 0;` que apagaba la rama
  entera, y con ella el marcador del lector. **Se sacó en 1.5.49**; ahora manda el ajuste de siempre
  (Ajustes → Controles → menú de pulsación larga, que de fábrica viene apagado).
  Para comprobarlo en el aparato sin cable: **Ajustes → Sistema → Memoria** mide el último OK mantenido y dice si
  el evento llegó. El Clima sigue siendo mosaico propio, que igual está mejor.
- **El botón PWR es del PMIC, no un GPIO** (`src/util/PowerKey`, singleton `POWER_KEY`, `pump()` desde el loop):
  está cableado al PWRKEY del AXP2101 y el chip lo reporta por su IRQ (GPIO38, `pmicIrq` en el perfil). Toque corto =
  **menú de pantalla** (limpiar, bloquear, dormir; se dibuja encima de lo que haya y sin pasar por una Activity, así
  funciona también dentro del lector, que es donde se acumula el fantasma); mantener 3 s = barrita y a dormir; el corte
  duro del PMIC está a los 10 s como escape. Bloqueada, PWR es lo único que llega: ni la palanca, ni OK, ni los gestos,
  y los botones dejan de contar como actividad (el aparato en la mochila puede reposar aunque la palanca se apriete
  sola). Un recordatorio desbloquea. NO pasa por el `InputManager` del SDK a propósito: su antirrebote de 5 ms se come una
  pulsación entera que aparece y desaparece entre dos lecturas. La polaridad del flanco se aprende en caliente y se
  guarda en RTC RAM. GPIO38 no es RTC GPIO, así que **el que despierta sigue siendo OK** (GPIO5), y eso ahora está en
  el perfil de la placa (`InputPins.wakePin`) en vez de escondido en el código.
- Atajo de voz global: **dos toques de Atrás** abren Hablar desde cualquier pantalla tranquila
  (`checkVoiceShortcut()` en el loop de `main.cpp`, ventana de 500 ms). ARRIBA/ABAJO es una palanca física
  (arriba XOR abajo, nunca las dos) y Atrás mantenido ya sincroniza o actualiza.
  Atrás en el hub no hace nada: el hub es el fondo (antes abría el último libro y no había forma de quedarse).
- **El movimiento es una entrada más** (`src/input/MotionInput`, singleton `MOTION`, `poll()` cada 80 ms desde el
  loop): inclinar, sacudir, girar, horizontal, boca abajo y doble golpe. Tres son globales y salen de
  `checkMotionGestures()` en `main.cpp`: boca abajo calla lo que suena, sacudir cancela (corta la grabación abierta o
  descarta la alarma), doble golpe abre Hablar. El resto los consume cada pantalla con `MOTION.take(Event)`.
  El INT1 del IMU está cableado al enable del amplificador (GPIO39), así que **no se puede usar la interrupción**: se
  consulta. El doble golpe lo detecta el motor del propio chip, porque a 80 ms no hay forma de ver un golpe de 10 ms.
  **Cómo está montado el sensor no está documentado**: hay que calibrar los ejes en Ajustes → Movimiento
  (`HubStore::imuMap`) o "inclinar a la derecha" puede ser cualquier eje.
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
- **La web se hizo de nuevo (servidor, después de 1.5.68)** ("super chafa… la web tiene que hacerse prácticamente de nuevo"). Vive en
  `server/public/board/` (`index.html`, `app.js`, `style.css`) como archivos estáticos que sirve `board.ts`; ya NO es
  un template literal adentro del `.ts` (el Dockerfile copia `public/`). Es una app de teléfono: barra de abajo con
  **Hoy · Agenda · Listas · Notas · Más**, un `+` flotante para agregar rápido, y todo se edita en una hoja que sube
  desde abajo al tocar la fila (todos los campos + Borrar). Bajo Más: Fotos, Noticias, Viajes, Memoria, Ajustes,
  Aparatos (multiusuario), IA y Contenido (admin), Log y la cuenta. Modo oscuro por `prefers-color-scheme`.
  **Regla de datos**: hay UN estado (`GET /api/board/state`, todo junto) y después de cada cambio se vuelve a pedir
  entero y se repinta (`change()` en `app.js`); así no queda una parte vieja y otra nueva en la misma pantalla, que
  era la queja ("al sincronizar cualquier cosa le faltan otras"). Lo que se agregó en el servidor para que TODO se
  pueda tocar: `POST /api/board/item` con `id` (texto y `done` en los dos sentidos), `/note` con `id`, `/memory`
  (alta y edición), `/feed` con `id` (renombrar), `uiSound` en `/settings`, `GET /api/photos/preview` y
  `/api/attachment/preview` (BMP 2 bpp → PNG con `bmpToPng`), `GET /api/log/meta` (última subida, firmware y motivo
  del arranque, parseados del log). Los borrados siguen por `POST /api/hub/edit {kind, id, action:"delete"}`.
  Las imágenes se bajan con `fetch` + `Authorization` y se muestran como blob: el token nunca va en una URL.
  Se probó con Chromium (Playwright) recorriendo todas las pantallas y editando desde la UI; no hay hardware que
  sincronice contra este servidor de prueba, así que lo que falta ver es el aparato tomando los ajustes.
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
- Música. **POR QUÉ NUNCA SONÓ hasta 1.5.44**: `AudioManager::parseWavHeader` no lee la cabecera de corrido, la
  recorre por chunks — `seek(0)`, `seek(12)` para el "fmt " y `seek(36)` para el "data" antes del `seek(44)` final —
  y el `seek` de `Mp3Source::wavSource()` sólo aceptaba 0 y 44, así que el segundo devolvía false y `play()` fallaba
  siempre. Ahora acepta cualquier posición dentro de la cabecera sintética.
  `src/music/MusicPlayer` (singleton `MUSIC`): el reproductor vive **fuera de la Activity**, así salir no corta la
  canción, el hub muestra qué suena (chip en la barra + punto en el mosaico) y `MUSIC.pump()` en el loop de
  `main.cpp` encadena la pista siguiente. Mientras hay música, `UiSound` se calla; `AlertBeep`, `SpeechOut` y
  `VoiceRecorder` la cortan primero (el I2S es uno solo). Dormir la corta (`sleepNow`).
  **La pantalla es un Winamp vertical** (1.5.45; la lista pelada de 1.5.44 no le gustó a nadie): barra de título
  negra, visor con el contador de 7 segmentos (`src/components/SevenSegment.h`, compartido con el temporizador), el
  analizador, título/artista y la línea "192 kbps 44 kHz estéreo"; barra de posición con cursor; botonera de seis
  botones biselados; corredera de volumen; y abajo la lista con pinta del editor de listas de Winamp.
  **El analizador NO es una FFT**: son los picos reales de cada bloque que decodifica `Mp3Source` (`level(i)`),
  guardados en un anillo de 24. Con el panel repintando cada varios segundos una FFT no tendría sentido, y esto
  igual dice la verdad sobre el audio.
  Por dentro sigue siendo **una sola lista**: las seis primeras posiciones son los botones de la botonera, la
  séptima el volumen y de la octava en adelante las carpetas o las pistas. Por eso la palanca recorre la botonera
  de izquierda a derecha y sigue de largo hacia abajo, sin "zonas" ni modos escondidos (las tres zonas invisibles
  de 1.5.43 se fueron). El volumen tiene su modito: OK sobre la barra y la palanca sube y baja. También se toca
  desde Ajustes → Sistema. Carpeta `/Music` o `/music` (se prueban las dos, y `/MUSIC`, `/Musica`, `/musica`).
  `src/music/Mp3Source` decodifica con Helix (`lib/HelixMp3`, C puro, RPSL) dentro del `read()` de una
  `AudioManager::WavSource` con cabecera WAV sintética; tags ID3v2/v1; volumen en `HubStore::musicVolume`.
  Pausa = volumen 0.
- Noticias (`NewsActivity`, mosaico Noticias): `GET /api/rss` y `/api/rss/article` (`server/src/rss.ts`, feeds que se
  cargan en `/board`, artículo limpiado a texto sin LLM); titulares y artículos leídos cacheados en `/.crosspoint/rss/`.
  El hub quedó en 14 mosaicos (1.5.48): fila ancha "Mi día" + Conversor, y debajo 4x3 con Leer, Hablar, Traductor,
  Recordatorios, Tiempo, Notas, Biblia, Música, Noticias, Fotos, Juegos, Ajustes. **Ya no hay "Próximamente"**: los
  catorce abren de verdad.
- Fotos (`PhotosActivity`, mosaico Fotos): `GET /api/photos` y `/api/photos/file?id=` (`server/src/photos.ts`). La
  foto se sube **tal como sale del teléfono** y la convierte el servidor con `sharp` (`toDeviceBmp`: rota por EXIF,
  escala a 480x800, 4 grises con Floyd-Steinberg y BMP de 2 bpp, ~150 ms); el navegador ya no arma nada. El aparato
  pide la lista al servidor cada vez que se entra, baja a `/Photos` de la SD y dibuja con el **pipeline de grises**
  del SDK (base BW + pasada LSB + pasada MSB + `displayGrayBuffer`): una sola pasada en modo BW pintaba de negro todo
  lo que no fuera blanco puro y la foto salía como una mancha.
- Conversor de unidades (`UnitsActivity`, mosaico Conversor, 1.5.48): **la cuenta es toda del aparato**; lo único
  que necesita servidor es pasar la voz a texto. Siete familias — longitud, peso, temperatura, volumen, superficie,
  velocidad y **cocina** (con ingrediente, para pasar tazas a gramos) — con la cantidad en dígitos de 7 segmentos,
  la equivalencia grande y el resto de la familia en una lista debajo. La palanca cambia el dígito o el campo y OK
  pasa al siguiente; sólo la temperatura admite signo. Se dicta ("doce pulgadas a centímetros") por
  `POST /api/transcribe` y lo resuelve `parseSpoken()` en el aparato, tomando la familia de la pantalla cuando el
  dictado no nombra unidad. La última familia, unidad e ingrediente se guardan en un archivo propio de la SD.
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
- Widgets del hub (sumario de cuatro renglones separados por reglas de 1 px, 1.5.48): clima (temperatura en UI_14,
  interior del SHTC3 con humedad a la derecha), próximo recordatorio, **música-o-libro** y agenda de hoy (o la frase
  si no hay eventos). El renglón de medios es uno solo: con música sonando muestra la pista y "3 / 14 · 4:12", y si
  no suena nada muestra el libro abierto — **la música salió de la barra de estado**. Íconos de 24 px en
  `src/components/icons/hubWidgetIcons.h`.
- **Los mensajes se sacaron del sistema en 1.5.44** ("me parece algo irrelevante"): no están más ni en el aparato,
  ni en `GET /api/hub`, ni en la Pizarra, ni como intención de voz (lo que el modelo clasifique como mensaje se
  guarda como nota).
- **El sistema visual está en `docs/ws397/DISENO.md`** (salió de un panel de tres propuestas con maquetas y tres
  jueces). Regla número uno: **nunca hay letras sobre trama**. El resalte (`src/components/Selection.h`,
  `drawSelectionRow()`) es pestaña negra de 5 px a la izquierda + marco + franjas tramadas SOLO en los márgenes, con
  el centro blanco y el texto negro: el negro macizo con texto invertido pegaba un salto de contraste enorme y dejaba
  fantasma, y la trama sobre toda la fila dejaba el renglón elegido como el menos legible de la pantalla. Estilos
  `Row` (listas) y `Tile` (mosaicos). Vale para el hub y para toda lista nuestra.
- **El sistema visual se aplicó a TODAS las pantallas nuestras en 1.5.48** (ola B, siete paquetes en paralelo con
  revisión adversarial: 51 hallazgos, 24 refutados, 19 arreglados). Lo compartido vive en dos archivos:
  `src/activities/ListStyle.h` (namespace `listui`: margen de 24 px, grilla de 8, fila de dos renglones
  título UI_12 + detalle UI_10, metadato a la derecha, casilla de 18x18, encabezado UI_14 con regla, paginador
  "Página 2 de 5") y `src/activities/games/GameUi.{h,cpp}` para los juegos. Pasaron por ahí: hub, Recordatorios,
  Notas, Noticias, Fotos, Viajes, Mi día, Música, visores, diálogos y popups, Ajustes → Movimiento y los doce juegos.
  Se fueron las pastillas negras macizas con texto blanco (pestañas, distintivos, cartas emparejadas) y los
  paginadores "1/12" en una esquina.
- **Escala tipográfica**: `SevenSegment` para los números grandes, **UI_14** (Ubuntu 14 bold, nueva en 1.5.48) para
  títulos, UI_12 para el cuerpo, UI_10 para etiquetas, SMALL (NotoSans 8, **ahora con negrita**) para pies. Las dos
  caras nuevas cuestan 122 KB de flash y se generan con `lib/EpdFont/scripts/convert-builtin-fonts.sh` +
  `build-font-ids.sh`.
- `GfxRenderer::drawPixel` ya NO escribe una línea de log por píxel fuera de pantalla: los cuenta y avisa una vez por
  segundo. Un solo cartel más ancho que la pantalla dejaba miles de líneas de "Outside range" y se comía el log
  entero (el que mandó el usuario en 1.5.43 tenía 2900 líneas y 2877 eran eso).

## Energía: el reposo en tres etapas (1.5.48)

- Hasta 1.5.47 había dos estados y nada en el medio: despierto (~40 mA, el loop cada 10 ms) o deep sleep, que es
  un reset al volver. `src/util/IdleSleep` (singleton `IDLE_SLEEP`, `tick()` desde el loop) agrega la etapa del
  medio: a los **30 s** de quietud entra en `esp_light_sleep_start()`. La pantalla queda como estaba (el panel es
  biestable: retener no cuesta nada), el estado sigue vivo y vuelve en menos de 10 ms.
- Despiertan: los cuatro botones (arriba 4, OK 5, abajo 6, BOOT 0) por nivel bajo, la IRQ del PMIC (GPIO38, por
  ahí entra PWR), el INT del RTC (GPIO45) y el timer. **En light sleep no hace falta que el pin sea RTC GPIO**:
  eso es lo que destraba GPIO38 y GPIO45, que para el deep sleep no sirven.
- **El movimiento NO despierta** (1.5.72). Hasta 1.5.71 el ciclo duraba 2 s y cada vez miraba el acelerómetro para
  encenderse solo al levantarlo; con el umbral que fuera, el aparato entraba y salía del reposo cada dos segundos
  para siempre (el log estaba lleno de `despertó por movimiento tras 2000 ms`) y el reposo no existía en la
  práctica. Se sacó por decisión del usuario: "SIEMPRE APRETARE UN BOTON para despertarlo". Ahora el ciclo dura lo
  que diga `capNextRest()` y **el acelerómetro se apaga al entrar al reposo** (`halTiltSensor.deepSleep()` desde
  `tick()`); `MotionInput::poll()` lo vuelve a encender solo en cuanto el loop corra.
- **El tope del ciclo son dos cosas, y las dos hacen falta**: lo que falte para la próxima alarma
  (`msUntilNextAlarm()`) y **lo que falte para el deep sleep**. Sin lo segundo el aparato se queda en light sleep
  para siempre y nunca baja al sueño profundo, porque el contador de ocio sólo crece mientras el loop corre y
  reposando no corre. Con el ciclo de 2 s eso quedaba tapado; al sacarlo quedó a la vista.
- Más de una hora hasta la alarma: si el INT del RTC está usable no se arma timer y el reposo dura toda la noche;
  si **no** lo está, el timer se corta a la hora y se vuelve a recontar. Nunca se deja el reposo sin ninguna fuente
  de despertar teniendo algo pendiente. Por eso también `RTC_ALARM.begin()` va **antes** de `IDLE_SLEEP.begin()`:
  limpia la bandera AF, que si no deja GPIO45 en bajo y `probeRtcInt()` lo marcaría inusable toda la sesión.
- **Un rechazo del kernel no es actividad** (`Woke::Rejected`): si un pin ya está en el nivel de despertar
  (un botón pegado, la IRQ del PMIC trabada), `esp_light_sleep_start()` rechaza. Tratarlo como si alguien hubiera
  tocado el aparato reiniciaba el contador de ocio en cada pasada, así que no reposaba **y** tampoco llegaba nunca
  al deep sleep: 40 mA hasta agotar la batería. Ahora se cuenta, se loguea y el ocio sigue corriendo.
- Tampoco se reposa por menos de `MIN_REST_MS` (500 ms): con una alarma vencida que la pantalla de turno no atiende
  (el lector, a propósito) el tope salía 1 ms y el aparato giraba entrando y saliendo del light sleep sin parar.
- `preventAutoSleep()` **reinicia el contador de ocio**, así que una Activity que lo pida para siempre mantiene el
  aparato despierto para siempre. `ReminderAlertActivity` lo hacía: una alarma a las 3 AM que nadie atendía se
  comía la batería hasta la mañana. Ahora lo pide sólo mientras suena, como el temporizador.
- **No reposa** con música, grabación, red arriba, USB enchufado, la tarjeta prestada (modo memoria USB), el menú
  de pantalla abierto o una Activity que pida `preventAutoSleep()`. El deep sleep tiene precedencia: el reposo se
  decide DESPUÉS, así nunca puede impedirlo.
- **Tiempo para dormir** (Ajustes → Sistema, `STR_TIME_TO_SLEEP`): es un número suelto de 1 a 31 minutos, default
  10, donde **31 quiere decir "nunca"** (`getSleepTimeoutMs()` devuelve 0 y `sleepNow` no se llama nunca por
  inactividad). No hay tres opciones con nombre: se diseñaron en su momento y **nunca llegaron al árbol**; si esta
  línea vuelve a decir "Ahorro / Normal / Siempre encendido", está mintiendo.
  Con 31, o sea "nunca", el reposo (light sleep) sigue funcionando: "nunca" significa **no bajar a deep sleep**,
  no "quedarse a pleno". Tiene red de seguridad: si el reposo queda bloqueado media hora seguida estando ocioso y
  sin USB, se duerme igual y lo dice en el log (`REST_BLOCKED_GIVE_UP_MS` en main.cpp, y sólo aplica con
  `sleepTimeoutMs == 0`, o sea con 31 puesto). Si no, una red que quedó arriba se come la batería en una noche.
  **Decidido para la venta** (`docs/ws397/PLAN_IMPLEMENTACION.md`, Ola 2): en la ws397 la fila se esconde y los
  tiempos quedan fijos; el usuario no elige modo de energía.
- **Todos los caminos de sueño pasan por `sleepNow()`**, y ahí se apagan el IMU (`halTiltSensor.deepSleep()`) y el
  enable del amplificador (`AudioManager::silenceAmp()`). Antes eso vivía en `enterDeepSleep()`, por el que **no**
  pasan los tres re-sleep del `setup()`: el aparato se dormía con el QMI8658 muestreando a 250 Hz. El único camino
  que sigue sin pasar por `sleepNow()` es el reintento a 60 s de "timer wake sin reloj", que hace las dos llamadas
  a mano.
- **El reloj se congela reposando**: la hora en pantalla queda en el minuto en que entró. Es a propósito —
  despertar cada minuto a repintar sería un parcial por minuto, o sea un completo cada cuarto de hora para
  siempre, que es exactamente lo que la regla del panel prohíbe. Se corrige sola en cuanto alguien lo toca.
- **Alarma del RTC** (`src/util/RtcAlarm`, singleton `RTC_ALARM`): PCF85063, registros 0x0B-0x0F, AIE/AF en
  Control_2 (0x01). Se arma al próximo recordatorio o al fin del temporizador con la hora en UTC, que es lo que
  guarda el RTC. El timer del light sleep se corta a la hora; la alarma del chip aguanta las esperas largas y
  despierta en el segundo exacto. **La bandera AF se limpia siempre**: si queda puesta, GPIO45 se queda en bajo,
  el light sleep se rechaza en bucle y el aparato gasta más despierto que sin reposo. Sigue sin servir para el
  deep sleep (GPIO45 no es RTC GPIO): eso lo arma `armReminderWake()` con el timer, y así queda.

## Batería: no hay miliamperímetro, hay un diario (1.5.49)

- **El AXP2101 NO tiene registro de corriente de batería.** Tiene ADC de VBAT, VBUS, VSYS, TS y temperatura de
  pastilla, y nada más; el AXP192 sí lo tenía, este no. El medidor del SDK sólo expone porcentaje, milivoltios y
  si carga. O sea que un "consumo instantáneo en mA" en esta placa **no se puede** y no hay que volver a
  intentarlo.
- Así que se mide como se mide la autonomía de verdad: **anotando el porcentaje contra el reloj y mirando la
  pendiente**. `src/util/BatteryLog` agrega una línea a `/.crosspoint/battery.csv` cada 10 minutos y, sobre todo,
  **justo antes de dormir** (esa es la que abre el tramo largo, que es el que dice la verdad). 300 líneas como
  mucho, después rota a la mitad.
- `analyze()` toma la ventana **más larga** que termine en la muestra más nueva y que sea una descarga limpia: sin
  carga en el medio y sin que el porcentaje haya estado por debajo del actual (si hacia atrás baja, en algún
  momento subió, o sea que lo enchufaron). Menos de diez minutos no se mide: el medidor tiene 1 % de resolución,
  que en una batería de 1500 mAh son 15 mAh.
- La cuenta vive en el header como función pura, sin nada del aparato adentro, justamente para poder probarla sin
  placa: **`./test/battery_drain/run.sh`** (17 casos, incluido el de haberlo enchufado en el medio).
- Se ve en **Ajustes → Sistema → Memoria**: "%/h · quedan N h (N días)" y sobre qué ventana se midió.

## Disciplina de tareas (1.5.48)

- `src/TaskConfig.h` declara núcleo, prioridad, stack y para qué de cada tarea, y ahora eso se puede **medir**:
  cada una se anota al arrancar (`tasks::attach`) y `usage()` devuelve la marca de agua del stack. Las del SDK,
  que nacen y mueren solas (`audio_play`), se buscan por nombre. El loop de Arduino está en el registro porque es
  el que más cerca está del límite: por ahí pasan el TLS, el parseo de EPUB y todo lo que no tiene tarea propia.
- `tasks::runBounded(nombre, stack, fn, arg)` corre trabajo pesado en una tarea de vida corta con el stack
  declarado y devuelve cuántos bytes usó. **No** es para no frenar la UI (el llamador espera, igual que antes):
  es para que un stack grande exista sólo mientras dura ese trabajo en vez de estar reservado para siempre en el
  loop. Su primer usuario de verdad son las apps en Lua.
- **Ajustes → Sistema → Memoria** (`TaskStatsActivity`): heap interno con su mínimo histórico y el bloque
  contiguo mayor (el número que decide si una asignación grande entra), PSRAM, el stack usado contra el declarado
  de cada tarea, y cómo va el reposo. Se repinta sólo si algún número se movió más de 2 KB.

## Apps en Lua desde la tarjeta (1.5.48)

- Una app es **un archivo** en `/Apps` de la tarjeta (`/Apps/dados.lua`). Se copia por el modo memoria USB y se
  abre en **Juegos → Apps de la tarjeta**. Contrato de callbacks (`on_open`, `on_key`, `on_tick`, `on_draw`), no
  de bucle propio: en tinta el refresco lo decide el firmware, y una app con su `while true` se comería el loop,
  los recordatorios y el reposo. El contrato entero está en `docs/ws397/APPS_LUA.md` y hay ejemplos en
  `examples/Apps/`.
- **El cajón** (`src/lua/LuaSandbox.cpp`): están `math`, `string`, `table`, `utf8`, `coroutine` y la base; NO
  están `io`, `os`, `package`, `debug`, `require`, `load`, `loadstring`, `dofile`, `loadfile` ni `string.dump`
  (los `.c` de esas bibliotecas ni se copiaron a `lib/Lua`). Topes: 192 KB de memoria desde PSRAM, 400.000
  instrucciones por llamada (un `while true do end` termina en error de la app, no en un aparato colgado) y
  32 KB de stack en un worker de `runBounded`.
- Está separado de `LuaApp` justamente para poder probarlo sin placa: **`./test/lua_sandbox/run.sh`** verifica de
  escritorio que lo que tiene que estar está, que lo que no, no, que la guardia corta un bucle infinito sin tocar
  uno normal, que el techo de memoria aguanta y que las apps de ejemplo corren sus callbacks sin error.
- Lua 5.4.7 con `LUA_32BITS` (el S3 tiene FPU de simple precisión) cuesta **108 KB** de flash. La configuración va
  editada en `lib/Lua/src/luaconf.h` y no con un `-D`: ese archivo define `LUA_32BITS` sin protección, un `-D`
  quedaría pisado, y además lo incluyen tanto el intérprete como nuestro código.

## Revisión externa de 1.5.52 (1.5.53)

Diecisiete hallazgos sobre el commit `2c6b069`, verificados uno por uno contra el árbol (dieciséis seguían
vigentes; uno ya estaba arreglado). Lo que salió de ahí:

- **F02** — `postOrQueue` no encolaba los 5xx y `flushQueue` los DESCARTABA: un 502 de Railway mientras
  sincronizás y la nota, la actividad del calendario o el ítem del viaje se perdían en silencio. Ahora lo
  temporal (sin red, transporte, 429, 5xx) se conserva y solo el 4xx definitivo se tira.
- **F03** — la sincronización bajaba primero y vaciaba la cola después, así que la instantánea que quedaba
  guardada era la de ANTES de aplicar lo pendiente: la tarea tildada sin WiFi reaparecía sin tildar. Se invirtió.
- **F16** — `SDCardManager::writeFile` borraba el archivo viejo antes de tener el nuevo. Ahora escribe a `.tmp`,
  verifica el largo y recién ahí reemplaza; `readFile` rescata del `.tmp` si el destino no está.
- **F08** — `armReminderWake` podía armar el deep sleep para el recordatorio SIGUIENTE y saltearse el vencido.
  `nextWakeInstant()` es ahora el único criterio y lo vencido siempre gana.
- **F05** / **F09** — Noticias respeta el `cache:false` del servidor y rescata los artículos cortos; Notas guarda
  por `postOrQueue`.
- **F10** — `POST /api/pair/start` le daba el código a CUALQUIERA que supiera el deviceId. Ahora un aparato ya
  vinculado exige su token (403 `device_owned`) y el código del período de espera solo vuelve al mismo token.
- **F07** — las alarmas estaban mudas en casi toda la máquina: `checkTimeAlarms()` usaba la lista blanca de
  "pantallas tranquilas", que existe para no robarle los controles a cada app. El temporizador vencido en un
  juego, en una app de Lua, en Noticias, Fotos, la Biblia o el Traductor no sonaba nunca. Ahora se pregunta al
  revés: suena en todos lados menos donde molestaría (ocupada = `preventAutoSleep()`, más Timer/ReminderAlert,
  que SON la alarma, y el lector, que está excluido a propósito desde siempre).
- **F06** — confirmar un recordatorio que repite, sin WiFi, lo borraba de la caché y con él se iba la alarma.
  `HubStore::completeReminder()` le corre la fecha a la próxima ocurrencia (y sigue corriendo hasta pasar la hora
  actual, así un diario confirmado cuatro días tarde no suena cuatro veces). Es una aproximación a propósito: el
  servidor manda la versión buena en la próxima sincronización.
- **F04** — pausar era bajar el volumen a 0. El MP3 se seguía decodificando y el I2S escribiendo a velocidad de
  hardware, o sea que la pausa gastaba lo mismo que sonar y la pista se terminaba sola "en pausa".
  `AudioManager::setPaused()` frena la tarea, baja el amplificador y apaga el canal TX (parche 0018 del SDK).
- **F11** — el aparato no tenía noción de cuenta. Desde `/board` se lo puede mudar a otra SIN que le cambie el
  token, y los ids del store se numeran desde 1 EN CADA CUENTA: la cola offline de la cuenta vieja tildaba o
  borraba lo que le tocara el mismo número en la nueva. `HubStore::account` guarda de quién es lo que hay;
  `HubSyncActivity` lo consulta ANTES de vaciar la cola y `DevicePairActivity` al vincular.
- **F17** — el paquete de contenido daba por bueno cualquier cuerpo de 200 y guardaba el sha ANUNCIADO. Ahora se
  compara el tamaño y se recalcula el sha256 corto antes de tocar la tarjeta.
- **F01** — tildar un recordatorio que repite no era idempotente: un reintento que llega porque se perdió la
  RESPUESTA avanzaba un ciclo de más. El POST lleva `at` (el `dueAt` de la ocurrencia) y el servidor no avanza si
  ya no coincide.
- **F12** / **F13** — `store.ts` y `attachments.ts` eran los dos documentos que seguían con leer-modificar-escribir
  sin candado. `store.mutate()` y `mutateDoc` lo hacen todo adentro del candado; las llamadas al modelo quedan
  afuera a propósito.
- **F14** — el tope mensual medía cuatro rutas y tres más llamaban al modelo por afuera (`/api/bible/ask`,
  `/api/calendar/dictate`, `/api/suggest`). `METERED` es ahora una tabla con `llm` y `audio` por ruta.

**Queda sin hacer, a propósito: F15** (`setInsecure()`). El aparato cifra pero NO autentica al servidor: no
verifica el certificado ni el nombre del host, así que un intermediario puede hacerse pasar por el servidor,
quedarse con el token o cambiar la descarga OTA. Arreglarlo no es sacar el `setInsecure`: hay que embeber las
raíces, chequear el nombre del host en `SecureClient` (hoy no se llama a `wolfSSL_check_domain_name` en ningún
lado) y, sobre todo, **poner el reloj en hora ANTES del primer TLS** — hoy nadie llama a `settimeofday` y un
certificado se valida contra la fecha. Sin eso el arreglo deja al aparato sin red. Es una ola aparte.

## Lo que se rompió y por qué (1.5.54 / 1.5.55)

- **El log no servía y por eso no se podía diagnosticar nada más.** `devlog::tail()` leía PREVIOUS primero y
  CURRENT después; como el archivo rotado suele estar lleno (64 KB), se comía el presupuesto de 24 KB entero y a
  CURRENT le quedaban CERO bytes. El aparato subía siempre el final del log VIEJO y nunca una línea de lo que
  acababa de pasar: en `/board/log` se veía la misma tanda de hace semanas en cada sincronización, y vaciarla no
  cambiaba nada. Ahora manda lo nuevo y PREVIOUS entra solo si sobra lugar.
- **Un solo códec, un solo amplificador, cinco AudioManager.** `ensureI2s()` ya le pedía el puerto al dueño, pero
  `stop()` y `powerDown()` seguían bajando el amp, muteando el DAC y cortando el riel del códec **desde una
  instancia que ya no era dueña de nada**. O sea que el clic que sonó hace 250 ms desarmaba el códec por debajo de
  la canción que acababa de empezar. De ahí "la música no suena" y "se come la mitad del audio" de las cartas.
- **Los sonidos del sistema.** `UiSound` le preguntaba al driver si se podía crear un canal I2S. Eso valía hasta
  que el puerto pasó a tener dueño: `stop()` NO suelta los canales (sólo `end()`), así que en cuanto sonaba el
  primer pitido esa instancia se quedaba con el puerto y el driver contestaba "ocupado" para siempre. Después del
  primer sonido, ningún clic volvía a sonar. Ahora se pregunta `AudioManager::portBusy()` (¿hay alguien
  reproduciendo o grabando?), que es lo que de verdad importa.
- **El amplificador tarda en arrancar.** Un clase D no pasa de apagado a amplificando en cero: los primeros
  milisegundos salen mudos. Con clips cortos se oye como que empieza tarde. Ahora va silencio también DESPUÉS de
  levantar el enable (~64 ms).
- **El recordatorio nacía vencido.** "A las ocho de la mañana" dicho a las 12:52 daba las 08:00 de HOY y sonaba en
  el acto. El modelo ahora tiene la regla escrita y, por si igual se equivoca, `rollForwardIfPast()` lo corre a la
  próxima ocurrencia antes de guardarlo. Sólo con hora explícita: sin hora, correr la fecha convertiría "recordame
  HOY comprar pan" en mañana.
- **La lista blanca de "pantallas tranquilas" era demasiado chica** y ya había mordido a las alarmas (F07). El
  **doble golpe** tenía el mismo problema: no existía en los juegos, en Noticias, en Fotos ni en la Biblia, que es
  justo donde uno lo prueba y concluye que los gestos no están hechos. Como no usa ningún botón, no le roba los
  controles a nadie: ahora anda en cualquier pantalla que no esté ocupada. El **doble Atrás** sigue atado a la
  lista blanca a propósito: ahí Atrás es un botón que cada app usa para salir.

## PWR hace UNA sola cosa: mantenerlo (1.5.59)

- Antes de **1,2 s**: nada. **No hay toque corto y no hay menú de pantalla** — se sacó en 1.5.59
  ("el menú al presionar PWR no lo quiero más, sólo la barrita que carga").
- A los **1,2 s** aparece la barrita.
- **Soltar con la barrita en pantalla = suspender** (deep sleep; las alarmas siguen vivas y el RTC despierta).
- **Llegar a los 3 s = apagar**: se pinta el fondo de pantalla, se desmonta la tarjeta y el AXP2101 corta los
  rieles (bit0 de 0x10, soft off). No hay alarmas ni reloj; se vuelve con PWR mantenido 1 s (PressOn).
- Suspender pasa al SOLTAR a propósito: si durmiera al cruzar el umbral con el botón abajo, nunca se podría
  llegar a los 3 s. El corte duro del PMIC sigue a los 10 s como escape de emergencia.
- Con el menú se fueron sus otras dos entradas: **limpiar pantalla** (que igual lo hace solo el coordinador de
  refresco, un completo cada 12 parciales) y **bloquear**, que quedó sin puerta. Si hace falta, van a
  Ajustes → Sistema.

## Lo dictado se abre donde vive (1.5.55)

- `POST /api/voice` devuelve ahora el `id` de lo que guardó en cada entrada de `saved[]`. Con **una sola** cosa
  guardada, `VoiceActivity` no muestra un cartel que dice "listo": abre esa cosa en su pantalla.
  Un recordatorio entra **directo al editor** de `AgendaActivity` (hora, fecha y repetición a la vista, OK
  confirma, Atrás largo borra); un ítem de lista deja el cursor sobre él en su sección; una nota abre `NotesActivity`
  posicionada. Con varias acciones de un tirón no se abre nada: no hay "la pantalla correcta" para tres cosas.
- Así una hora mal entendida se ve ANTES de que suene, que es de lo que se trataba.
- "Mensaje" salió de los ejemplos de la pantalla de Hablar: la pizarra se sacó del producto en 1.5.44 y el ejemplo
  seguía prometiendo algo que el aparato ya no hace.

## Lo que salió y por qué (1.5.63 / 1.5.65)

- **Los juegos compilados y las tarjetas se fueron.** El mosaico Juegos abre DIRECTO las apps en Lua ("la pestaña
  juegos es sólo para LUA… quita apps lua de ahí, va a ser lua en general"). Las tarjetas alcanzaron a tener
  mosaico propio y duraron una versión: "tarjetas afuera, se va, luego lo hacemos en LUA". El hub volvió a trece
  mosaicos. En el servidor el catálogo (`CARDS`) y el dibujante siguen en `assets.ts`, pero `CARDS_IN_PACK = false`:
  el paquete de contenido NO manda más ni los BMP ni los dos audios por tarjeta.
- **Guiones de partición de palabras**: `-DHYPH_PRODUCT_LANGS=1` deja sólo los seis idiomas del producto y saca
  fi/it/pl/sv/uk (−63,5 KB). El flash está en 87 %. El que sobra es el alemán, 201 KB él solo; las fuentes son
  1994 KB. Lua no es la palanca para el flash: son las fuentes y los guiones.
- **El doble golpe se bloqueaba a sí mismo.** En 1.5.58 se pidió 1,2 s de quietud después de cualquier movimiento
  grande para que azotar el aparato no se leyera como doble golpe — pero un doble golpe ES un movimiento grande, así
  que los propios golpes reseteaban el contador y con golpes firmes el gesto no entraba nunca. Ahora la marca la
  pone sólo una SACUDIDA emitida (oscilación sostenida), que es lo que de verdad hay que distinguir de un golpe seco.
- **Los sonidos del sistema estaban activados y no se oían**, y no era el ajuste: (1) el silencio con el que el SDK
  ceba la línea antes y después de levantar el amplificador se contaba en BUFFERS de 512 cuadros, o sea 32 ms a
  16 kHz pero 10,7 ms a 48 kHz, que es justo la tasa de los clics — el clase D recién arrancaba cuando el clic de
  26 ms ya había terminado; ahora va en milisegundos (`AMP_PRIME_MS`/`AMP_SETTLE_MS`, parche 0021); y (2) el clic de
  navegación salía a 0,09 de escala completa, doce decibeles por debajo del pitido del recordatorio (0,37) que sí se
  oye. "Normal" ahora deja los clics en el entorno del pitido y "suave" a la mitad.
- **Convivencia de sonidos y música**: con música sonando los clics no suenan, punto (`MUSIC.isActive()` en
  `UiSound::play`). Es a pedido: "cuando se reproduzca la música los demás sonidos no deben oírse, sólo la música".
  Los avisos (`AlertBeep`, Piper) sí mandan: cortan la canción, porque el I2S es uno solo.

## Lo de 1.5.70 (y el servidor que va con él)

- **Hablar vuelve a donde se abrió.** `VoiceActivity::leave()` era siempre `goHome()`: desde Notas o la agenda con
  dos Atrás, al salir se caía al hub ("cuando salgo de escuchar la nota de voz vuelve solo al hub"). Si hay una
  pantalla debajo (`ActivityManager::hasStackedActivities()`) y no es el lector, `finish()` y `WiFi.mode(WIFI_OFF)`
  sin reinicio silencioso; el reinicio queda para el lector, que necesita el heap entero.
- **Agenda vacía: OK dicta uno nuevo.** Sin ítems no había forma de crear el primero. `AgendaActivity::dictateNew()`
  abre Hablar desde una sección vacía o al tildar el último; el renglón vacío lo dice
  (`STR_AGENDA_EMPTY_VOICE`).
- **Las listas no repiten** (`store.addListItem`): mismo texto sin acentos ni mayúsculas = mismo ítem; si estaba
  hecho vuelve a pendiente. Lo usan `/api/voice` y `/api/board/item`.
- **La clave del WiFi entra por el teléfono** (`WifiSelectionActivity`, estado `PHONE_ENTRY`, solo ws397): en vez
  del teclado en pantalla, el aparato levanta su punto de acceso abierto `CrossPoint-Reader` con portal cautivo y
  la página `/settings` de siempre; la persona agrega la red ahí y el aparato, mirando el `WifiCredentialStore`
  cada 500 ms, se conecta solo. Vale también para la red oculta (cualquier credencial nueva). **Dictar la clave
  deletreada sin red NO se puede**: en el S3 no hay reconocedor de voz en español que quepa (ESP-SR MultiNet es
  inglés y chino y pide varios MB de flash; estamos en 87 %), y con red no hace falta porque ya está conectado.

## Roadmap acordado

La lista completa de funciones, con fase, estado y contrato del servidor, está en `docs/ws397/FUNCIONES.md`
(fusión de lo planeado con lo que hacen el reTerminal Sticky y el ZecTrix Note 4). Resumen:

0. Hardware: volumen (hecho), botón PWR por el PMIC (hecho, 1.5.47), coordinador de refresco (hecho), driver SHTC3
   en dos tiempos (hecho), IMU por polling con boca abajo = silenciar, doble golpe = PTT y sacudir = cancelar
   (hecho), reposo en tres etapas con light sleep y alarma del RTC por GPIO45 (hecho, 1.5.48), consumo y stacks
   medidos desde Ajustes → Sistema → Memoria (hecho, 1.5.48). Descartado: trackball y botones PCF8574.
1. Hub + preguntarle al libro + cliente HTTP + sincronización con `GET /api/hub` y widgets + Hablar con
   clasificador de intención (hecho). Falta: pizarra de mensajes desde el teléfono (1.8), ajustes del hub en la web UI.
2. Voz: el servidor clasifica la intención de una sola grabación (pregunta, tarea, recordatorio, compras,
   nota, mensaje, temporizador, traducción, alarma); recordatorios con repetición y alarma del RTC; varias
   listas de tareas (Entrada, Casa, Trabajo, Administrativo, Compras, proyectos) con vista por semana ISO;
   TTS con Piper (hecho); traductor en modo conversación (hecho); temporizador, cronómetro y Pomodoro (hecho); agenda; memoria.
3. Contenido: Biblia (hecha, capítulos cacheados; falta descarga por libro e índice offline), MP3 estilo Winamp
   (hecho), versículo/frase del día, RSS/lectura web, álbum de imágenes en 4 grises, clima detallado.
4. Juegos: damas, cartas (rummy, solitario, blackjack), retos mentales (sudoku, acertijos, cálculo), memoria
   (parejas, Simón), Tetris experimental, ajedrez opcional. Y lo que no viene compilado: apps en Lua desde la
   tarjeta (hecho, 1.5.48).

Descartado: radio por streaming, Casa Cerebro, lectura en voz alta de libros, Spotify (DRM; solo Connect online
con cspot, no offline), auto-rotación por IMU, chino.

Principios: un solo botón de voz (PTT) desde cualquier pantalla; respuesta escrita siempre y hablada cuando
aporta; todo funciona sin WiFi con la caché de la SD; CrossPoint sigue siendo el lector y lo nuestro entra
como Activities en el hub; todo lo pesado (STT, LLM, TTS, render) en el servidor; audio y red en tareas
FreeRTOS separadas de la UI; el aparato nunca guarda claves de Anthropic.

## Convenciones

- Commits: prefijo `ws397:`. Cambios al SDK en el submódulo, con su propio commit.
- Los commits ws397 del SDK (perfil, waveform, battery, rtc, wake, halfrefresh, dueño del puerto I2S, escritura
  segura en la SD, pausa de verdad del audio) están exportados como `.patch` en
  `docs/ws397/` (`git format-patch`); si al submódulo le falta alguno, `git am docs/ws397/NNNN-*.patch` dentro de
  `freeink-sdk/`. Regenerarlos cuando se agregue un commit al SDK.
- No tocar la lógica upstream fuera de lo necesario para la placa; preferir `case Board::WS397` sobre `#if`.
- Antes de un release: `pio run -e ws397` limpio y probar en hardware.
