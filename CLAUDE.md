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
  `github.com/*/archive` bloqueados; SCons y las libs se traen de PyPI/GitHub). `WS397_OTA_TOKEN` es obligatorio
  para publicar; `WS397_OTA_URL` solo sobrescribe el destino predeterminado.
- OTA: el aparato consulta la URL pública compilada desde `include/CrossPointOtaConfig.h`
  (`https://paper-esp32.up.railway.app/firmware/latest`, JSON con la forma
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
- **En este sandbox el proxy no deja salir a los feeds ni a los diarios**, así que la parte de red de las noticias
  sólo se puede probar en Railway. Lo que sí se prueba acá es la lógica pura (`./test/news_pack/run.sh`) y que el
  servidor levante y conteste (`bun run src/index.ts` con `STORE_FILE`/`NEWS_FILE` apuntando a un temporal).

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
  editables en la web UI → Servidor; URL vacía = origen de la URL OTA compilada) y `ServerClient` (singleton
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
  (`checkVoiceShortcut()` en el loop de `main.cpp`, ventana de 1,2 s). El otro camino, y el que el usuario usa de
  verdad, es el **doble golpe** sobre la tapa, que anda en cualquier pantalla que no esté ocupada. ARRIBA/ABAJO es una palanca física
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
  **Hoy · Agenda · Listas · Notas · Ajustes** (fueron seis entre 1.5.78 y 1.5.92, con Viajes; antes eran cinco y la
  quinta, "Más", era un cajón con diez filas iguales adentro), un `+` flotante para agregar rápido, y todo se edita
  en una hoja que sube desde abajo al tocar la fila (todos los campos + Borrar). Modo oscuro por
  `prefers-color-scheme`.
  **Ajustes dejó de ser un cajón**: arriba lo del aparato (idioma, voz, sonidos, volumen, clima) que es lo que se toca, después
  Noticias, Memoria y Aparatos, y abajo un bloque **Avanzado** con IA, Contenido y Log. Las direcciones viejas
  (`#mas/...`) **siguen andando**: redirigen, porque alguien puede tener una guardada en la pantalla de inicio del
  teléfono. Y `/board/` con barra final daba **404** — que es justo como la escribe a mano el que la teclea —, así
  que ahora redirige a `/board`.
  Se probó con Chromium a 360 px de ancho recorriendo las trece pantallas (incluidas las tres direcciones viejas) y
  comprobando que las seis pestañas midan todas lo mismo y que nada se vaya de ancho.
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
- **Sincronización oportunista (1.5.76, `src/sync/Sync`)**: si la red ya está arriba por CUALQUIER motivo, el loop
  aprovecha para subir lo pendiente y bajar lo que cambió. Antes la única sincronización de verdad era
  `HubSyncActivity` y sólo se abría desde el hub; trece o catorce Activities levantan WiFi (Hablar, Noticias, la
  Biblia, el Clima, el Traductor, Preguntarle al libro, Vincular) y ninguna bajaba nada de paso.
  Tres guardias, y las tres hacen falta:
  1. **La pantalla tiene que estar desocupada** (`!preventAutoSleep()`): casi toda Activity de red lo pide MIENTRAS
     trabaja, así que la guardia espera sola a que termine y usa la ventana en la que la red sigue arriba y ya no
     hay nada en curso.
  2. **Tres segundos de quietud** (`QUIET_MS`): esto es SÍNCRONO y bloquea el loop unos segundos; hacerlo encima de
     una pulsación se siente como que el aparato se colgó.
  3. **El hub sólo se baja si la pantalla de turno no guarda índices dentro de `HubStore`.** `AgendaActivity` cachea
     `sectionIndex`/`itemIndex` sobre `lists` y `reminders`, y `NotesActivity` lo mismo: reemplazar esos vectores
     por debajo no deja una pantalla fea, deja una lectura fuera de rango. Con Agenda, Notas, Hub, Tiempo o el
     alerta de recordatorio en frente se sube y se bajan **las noticias** —que son archivos y no los referencia
     nadie— y el hub espera. No se pierde nada: en esas pantallas el hub ya sincronizó al entrar.
  Como mucho una vez cada `MIN_GAP_MIN` (30 min), y `HubSyncActivity` llama a `markFresh()` para que no se repita.
- **Noticias: el servidor mastica, el aparato sólo lee (1.5.75).** El usuario carga los feeds en `/board`; el
  servidor (`server/src/news.ts`) los recorre **solo, cada hora**, se mete en cada noticia, la limpia y las diez más
  nuevas además pasan por el modelo, que las reescribe para leerlas en una pantalla chica. Eso arma un **paquete**
  por cuenta, guardado en el volumen (`news.json`), que sobrevive al redeploy.
  El aparato se lo baja **cuando se conecta por cualquier motivo** (`newspack::sync()` desde `HubSyncActivity`), y
  va en dos pasos a propósito: primero el **manifiesto** (`GET /api/news/pack`, ~4 KB, con un sha por nota) y
  después **una nota por vez** (`GET /api/news/item?id=`) a su propio archivo en `/.crosspoint/news/`. Nada de un
  JSON grande: `ServerClient` no tiene streaming y copia el cuerpo DOS veces, así que 100 KB serían 200 KB de heap
  interno sobre los ~230 KB que hay. Lo que ya está y no cambió de sha no se vuelve a bajar, y lo que salió del
  manifiesto se borra de la tarjeta.
  **Con eso Noticias funciona sin red**: `NewsActivity::loadPack()` arma la lista desde el manifiesto y el cuerpo
  sale de la tarjeta. El camino viejo (`/api/rss` + `/api/rss/article`, limpiado sin modelo y bajo demanda) sigue
  ahí como respaldo para la cuenta que todavía no tiene paquete.
  El masticado gasta modelo **sin que nadie lo pida**, así que respeta el mismo tope mensual (`overQuota`): pasado
  el tope el paquete se arma igual, con el texto limpiado a mano. Topes por variable de entorno:
  `NEWS_PACK_ITEMS` (18), `NEWS_DIGEST_PER_RUN` (10), `NEWS_REFRESH_MS` (1 h).
  El reparto de titulares entre medios (uno de cada feed y después la segunda vuelta, para que un diario que
  publica cada diez minutos no se coma el paquete) es una función pura y se prueba sin red:
  **`./test/news_pack/run.sh`**.
  El hub tiene **13 mosaicos** (`TILE_COUNT` en `HubActivity.h`): fila ancha "Mi día" y debajo Leer, Hablar,
  Traductor, Recordatorios, Tiempo, Notas, Biblia, Música, Noticias, Juegos (= apps en Lua), Clima y Ajustes.
  **Ya no hay "Próximamente"**: los trece abren de verdad. (Esta línea decía 14 con Conversor y Fotos hasta
  1.5.86; los dos salieron del hub y nadie corrigió el texto.)
- **Las fotos salieron del producto (1.5.74).** El fondo de pantalla ya no es una imagen elegida a mano: es la
  **pantalla de información** que queda en el vidrio cuando el aparato se muere (`src/activities/home/SleepScreen`).
  Dice **SUSPENDIDO** o **APAGADO**, y con eso **cómo se vuelve**, que no es lo mismo: suspendido despierta **OK**
  (GPIO5, el único botón que es RTC GPIO en el S3) y apagado enciende **PWR mantenido 1 s** (PressOn del AXP2101,
  lo único que funciona sin ESP). La hora va como **sello**, no como reloj — nadie la va a actualizar —, y lo que
  va en dígitos de segmentos es la **próxima alarma**, que es el dato que sigue siendo cierto durmiendo. Debajo:
  clima, libro abierto y los **titulares** que entren (de la caché de Noticias, leída ANTES de que
  `prepareForDeepSleep()` desmonte la tarjeta). Se pinta **una sola vez** y con FULL: antes se pagaban DOS pantallas
  completas (la de sueño del SDK y encima la foto), y ahora `goToSleep()` acepta `render=false` para no pintar la
  primera. De paso el cuadro de Quick Resume queda siendo la pantalla anterior, que es lo que corresponde.
  El dibujante de BMP a pantalla completa (`src/util/FullScreenBmp`) no era de las fotos: era de los adjuntos de
  los viajes, y **se borró con ellos en 1.5.93**, igual que `server/src/deviceBmp.ts` (`toDeviceBmp`/`bmpToPng`) y
  las cuatro dependencias nativas que solo usaban los adjuntos (`sharp`, `mupdf`, `bwip-js`, `zxing-wasm`). La
  línea de arriba decía que `sharp` no se podía sacar "porque lo usan los adjuntos y el paquete de contenido":
  el paquete de contenido **no** lo usaba — `assets.ts` dibuja solo —, así que con los adjuntos afuera no queda
  nadie.
- Conversor de unidades — **salió del hub**; lo que sigue describe cómo estaba hecho, por si vuelve.
  (`UnitsActivity`, 1.5.48): **la cuenta es toda del aparato**; lo único
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
- **El tema manda en NUESTRAS pantallas desde 1.5.77, y antes no.** Existía `UITheme` con cuatro temas (Clásico,
  Lyra, Lyra Extendido, RoundedRaff) y el default era Lyra — de ahí el `[UI] Using Lyra theme` del log —, pero el
  hub, la agenda, las notas, las noticias y la Biblia dibujan con `src/activities/ListStyle.h`, que
  tenía las caras **fijas**. O sea que cambiar de tema cambiaba el lector y dejaba igual justo donde el usuario
  pasa el tiempo. Ahora `listui` pide las cuatro caras al tema (`listTitleFont`, `listDetailFont`,
  `sectionTitleFont`, `sectionDatumFont`; 0 = el default de siempre) y son **funciones**, no constantes, porque el
  tema se cambia en caliente.
  La geometría (margen de 24 px, grilla de 8) NO se movió al tema a propósito: es del sistema visual, no del tema.
- **La ws397 tiene UN tema y no se elige: Lyra (1.5.91).** Hubo un momento en que la placa iba a tener su propio
  tema (Diario) y un selector propio con Diario, Bento y Lyra, y elegir no servía de nada: hasta 1.5.82 `listui`
  sólo miraba las cuatro CARAS del tema, así que entre Bento y Lyra la única diferencia era el renglón de detalle.
  Eso se arregló —ahora también manda la forma— pero la conclusión del usuario fue la correcta igual ("el cambio de
  interfaz es una verga"): un tema es una cosa para mantener y probar, y tres son tres. La decisión final es **una
  sola interfaz, unificada, Lyra en todas las pantallas**.
  `UITheme::wantedTheme()` devuelve Lyra **fijo** en esta placa —y no el valor guardado— para que una tarjeta que
  venga con otro número puesto no mezcle métricas ni tipografías entre pantallas. La fila `STR_UI_THEME` está
  escondida en la ws397; las otras placas siguen con su selector.
  **Se borraron los dos temas propios**: `BentoTheme` en 1.5.86 y `DiarioTheme` en 1.5.91, con sus métricas, su
  directorio, su clave `STR_THEME_DIARIO` y su valor del enum. `BENTO` (5) y `DIARIO` (4) eran el ÚLTIMO valor de
  `UI_THEME` cuando se sacaron, así que ninguno renumeró a los de arriba: lo que se persiste es el NÚMERO, y mover
  cualquier otro le cambiaría el tema a quien ya eligió en las otras placas. Borrar del final es lo único seguro.
  Ojo: **Riel y Estación siguen siendo maquetas, no código** (`docs/ws397/maquetas/`).
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
  Notas, Noticias, Mi día, Música, visores, diálogos y popups, Ajustes → Movimiento y los doce juegos.
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
  comía la batería hasta la mañana. Ahora lo pide sólo mientras suena, como el temporizador. `OpdsBookBrowserActivity`
  sigue pidiéndolo fijo, pero es upstream y no se toca: para eso está la red de seguridad.
- **Pausar no es estar sonando**: `MUSIC.isActive()` quiere decir "hay una pista cargada" y sigue siendo cierto en
  pausa, así que usarlo para decidir si dormir dejaba el aparato despierto para siempre con la canción pausada —
  lo contrario de lo que uno espera al pausar. El contador de ocio y el bloqueo del reposo preguntan `isSounding()`.
- **Red de seguridad, la buena** (1.5.72): mide contra `lastUserInputTime`, que **sólo** lo reinician los botones y
  los gestos, nunca `preventAutoSleep()` ni la música. Si nadie tocó el aparato en media hora, no está enchufado y
  el reposo sigue bloqueado, se duerme igual. Antes esta red sólo corría con el tiempo en "nunca"
  (`sleepTimeoutMs == 0`), o sea que con el valor forzado de la ws397 habría quedado muerta justo cuando más hace
  falta: el auto-sleep de los diez minutos **no** alcanza, porque una Activity que pide "no duermas" congela el
  contador de ocio y con él ese auto-sleep.
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
  instrucciones por llamada, 64 KB de tamaño de archivo y 32 KB de stack en un worker de `runBounded`.
  El tope de instrucciones corta de verdad **incluso si la app se come el error con `pcall`**: el error de la
  guardia es un error de Lua común y `pcall` lo atrapaba, así que hasta 1.5.90 un bucle envuelto en `pcall` no
  terminaba nunca y —como `runBounded` espera con `portMAX_DELAY`— colgaba el loop de Arduino, no la app. Ahora
  pasarse del tope CONDENA al intérprete: el asignador deja de dar memoria y Lua no puede ni armar el objeto de
  error ni entrar a un `pcall`. Ver `stepHook` en `LuaSandbox.cpp`.
- Está separado de `LuaApp` justamente para poder probarlo sin placa: **`./test/lua_sandbox/run.sh`** verifica de
  escritorio que lo que tiene que estar está, que lo que no, no, que la guardia corta un bucle infinito sin tocar
  uno normal, que el techo de memoria aguanta y que las apps de ejemplo corren sus callbacks sin error.
- **`cp.time()` y tres apps de fábrica (1.5.81)**: `cp.time()` es la única forma que tiene una app de saber la hora
  (`os` no está en el cajón, y `os.execute` y `os.remove` vienen en la misma biblioteca). Devuelve `year`, `month`,
  `day`, `hour`, `min`, `sec`, `wday` (1 = lunes) y `epoch` en UTC, y **`nil` cuando el aparato no está en hora**,
  que es un estado real y frecuente. En la tarjeta de fábrica van `reloj.lua`, `ahorcado.lua` y `tresenraya.lua`
  (`examples/Apps/`); `contador.lua` y `dados.lua` quedan como ejemplos para leer.
  `reloj.lua` existe también para dejar escrito **cuándo** repinta una app en tinta: mira el minuto y devuelve
  `true` sólo cuando cambió. Devolver `true` en cada `on_tick` sería un parcial cada 120 ms, o sea un completo
  cada segundo y medio, para siempre.
  `./test/lua_sandbox/run.sh` corre las cinco con el reloj puesto y sin el reloj puesto, y juega **partidas
  enteras** de ahorcado y de tres en raya: ahí es donde aparecen el índice fuera de rango y el cursor que se
  cuelga con el tablero lleno, que un toque a cada callback no encuentra.
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
- **Permisos (1.5.79)**: `POST /auth/register` daba admin a quien se registrara con el correo de `ADMIN_EMAIL`,
  además de a la primera cuenta. `seedFromFiles()` crea esa cuenta **sólo cuando la base está vacía**, así que en
  un servidor donde `ADMIN_EMAIL` se configuró DESPUÉS —lo más normal— ese correo nunca llegó a la base y
  cualquiera podía registrarse con él y quedar de administrador. Un correo es público: no es una credencial.
  Ahora admin **sólo** si la base está vacía, y se loguea. Además `REGISTER_CLOSED=1` cierra el alta de cuentas
  nuevas (abierta por omisión, que es como venía). Se prueba sin base de datos: `./test/register_admin/run.sh`.
- **Reconstruibilidad (1.5.79)**: el repo padre apuntaba a un commit del submódulo que **nunca se había subido**
  (cuatro commits sin pushear), así que un `git clone --recursive` desde GitHub no podía traer el SDK: el firmware
  que se vende se compilaba únicamente en la máquina donde ese commit existía. Subidos. Y queda
  **`./tools/verificar-reconstruible.sh`**, que le pregunta al remoto si tiene el commit anotado, si el submódulo
  está limpio y si hay un `.patch` por cada commit ws397 del SDK.
- **F14** — el tope mensual medía cuatro rutas y tres más llamaban al modelo por afuera (`/api/bible/ask`,
  `/api/calendar/dictate`, `/api/suggest`). `METERED` es ahora una tabla con `llm` y `audio` por ruta.

**F15, primera mitad hecha (1.5.79): el reloj.** Eran dos relojes y se mantenía uno solo. El RTC guardaba bien la
hora, pero el del SISTEMA arrancaba en 1970 en cada arranque porque **nadie llamaba a `settimeofday`**. Eso no se
notaba en ningún lado salvo en el único que importa: la validez de un certificado se comprueba contra el reloj del
sistema, y en 1970 todo certificado del mundo parece "todavía no válido". Ahora `HalClock::applyToSystemClock()`
corre en el arranque, y `ServerClient::ensureClockForTls()` cubre el caso que el RTC no puede cubrir (aparato
recién armado o que estuvo sin batería): una vez por sesión de red, si el reloj no es creíble —anterior a 2024—
pide la hora por NTP antes de abrir el primer TLS.

**Queda sin hacer, a propósito: la otra mitad de F15** (`setInsecure()`). El aparato cifra pero NO autentica al servidor: no
verifica el certificado ni el nombre del host, así que un intermediario puede hacerse pasar por el servidor,
quedarse con el token o cambiar la descarga OTA. Arreglarlo no es sacar el `setInsecure`: hay que embeber las
raíces y chequear el nombre del host en `SecureClient` (hoy no se llama a `wolfSSL_check_domain_name` en ningún
lado). El tercer requisito —el reloj— **ya está**, así que lo que queda es el certificado propiamente dicho.
Va aparte a propósito: si la verificación queda mal, **el aparato se queda sin red** y hay que flashearlo por
cable. No se toca sin poder probar en hardware.

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

## PWR hace UNA sola cosa: mantenerlo (1.5.59) — SUPERADO en 1.5.99

**Desde 1.5.99 apretar y soltar = suspender**, a cualquier largo antes de los 3 s (ver "PWR colgaba el aparato
desde el primer reposo"). Lo de abajo queda como historia de por qué el toque corto "no hacía nada".

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
  mosaicos. En el servidor el catálogo (`CARDS`) y el dibujante quedaron en `assets.ts` apagados por
  `CARDS_IN_PACK = false` hasta que **se borraron enteros en 1.5.91**.
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

## Primer arranque, la tarjeta y el cronómetro del panel (1.5.80)

- **Las carpetas se crean solas** (`src/util/CardLayout`, `cardlayout::ensure()` desde el arranque, sólo ws397):
  `/Books`, `/Music`, `/Apps`, `/fonts` y `/dictionaries`. Antes el log decía `Fonts directory not found` y
  `No /dictionaries directory` y no pasaba nada más; ahora las carpetas existen, y desde el modo memoria USB se ve
  **dónde va cada cosa**, que es el problema real del que acaba de abrir la caja. Sólo carpetas vacías: el contenido
  (la tipografía, el diccionario español, el libro de muestra) va en la imagen de fábrica de la tarjeta.
- **Asistente de primer arranque** (`src/activities/home/SetupActivity`): idioma → WiFi por el teléfono → vincular →
  lugar del clima → **los tres gestos que no se adivinan** (dos toques de Atrás = hablar, Atrás mantenido =
  sincronizar, PWR mantenido = suspender). Los tres del medio son las pantallas que ya existen en Ajustes y se
  pueden saltar con ABAJO.
  Sale **una sola vez y sólo en un aparato que se ve nuevo**: sin `setupDone`, sin redes WiFi cargadas y sin haber
  sincronizado nunca. El que ya lo venía usando no lo ve al actualizar por OTA.
  **Por qué se guarda también el paso** (`HubStore::setupStep`): Vincular y el Clima hacen un **reinicio silencioso**
  al soltar la red, y después de ese reinicio el aparato ya no parece nuevo (tiene WiFi cargado), así que sin el
  paso guardado el asistente no volvía nunca. Con él, `pending()` pregunta primero si ya había empezado.
- **El panel se cronometra** (`PanelRefreshCoordinator::stat()`): `GfxRenderer` mide la escritura + la onda de cada
  refresco bloqueante y el coordinador acumula promedio, cantidad y máximo por forma. Se ve en
  **Ajustes → Sistema → Memoria → Panel** y va en la línea del log (`refresh FAST hint=page 612ms …`).
  Es lo que decide entre las dos explicaciones posibles de una página lenta, que piden arreglos opuestos: si el
  FAST tarda ~2 s, la onda parcial del panel ES así y hace falta una **LUT propia** (registro 0x32, trabajo con
  hardware delante); si el FAST es rápido pero hay muchos HALF/FULL, se están colando limpiezas y eso es política
  del coordinador. **No se toca ninguna onda a ciegas**: equivocarse deja el vidrio peor y no hay forma de verlo
  desde la nube. El asíncrono no entra en la cuenta: ahí lo que tarda es el `waitRefreshComplete()` del lector.

## Lo que el aparato encontró en 1.5.80 (arreglado en 1.5.82)

- **`/api/log` no existía en el servidor desde 1.5.74, y por eso no había diagnóstico.** El commit que sacó las
  fotos borró la línea `api.route("/log", deviceLog)` **entera**: el comentario de `/api/photos` había quedado
  pegado al final de ESA línea, así que al sacar el comentario se fue la ruta con él. `deviceLog` seguía
  importado y nadie lo montaba. Desde entonces `POST /api/log` daba 404 — un 4xx, o sea definitivo, o sea que
  `ServerClient` lo **descartaba** en vez de encolarlo — y `GET /api/log` devolvía el JSON de `app.notFound()`,
  que es lo que se veía en `/board/log` en lugar del log. La fecha calza exacto: la última subida buena es del
  12 de septiembre con 1.5.69, el día anterior al commit. Moraleja: **un comentario al final de una línea de
  ruta es una trampa**; van arriba.
  De paso `apiText()` en `app.js` no miraba el estado: ahora un 404 o un 500 tira error en vez de pegar el
  cuerpo del error adentro de la caja del log.
- **El tema seguía sin mandar del todo: faltaba la FORMA.** En 1.5.77 se conectaron las cuatro CARAS a `listui`,
  pero Bento ya declaraba además tarjetas redondeadas (`listRowRadius`), aire entre filas (`listRowGap`), título
  en negrita (`listTitleBold`) y cabezal sin filete (`headerUnderlineSize = 0`) — y eso lo leía **sólo** la UI de
  upstream (`freeink::ui`). O sea que en nuestras pantallas Bento quedaba en UI_12/UI_14/SMALL y Lyra en
  UI_12/UI_14/UI_10: **la única diferencia entre los dos era el renglón de detalle**, invisible. Era el mismo
  agujero de 1.5.77 una capa más abajo. Ahora `listui` lee los cuatro.
  El aire se come **de adentro** del alto que da la pantalla, no del paso: así ninguna de las veinte pantallas
  que llaman a `row()` tiene que cambiar su cuenta de posiciones.
  `headerUnderlineSize` se usa como SÍ o NO y no como grosor: allá arriba es el filete del cabezal de pantalla, y
  traerlo tal cual dejaría cada división de sección con una barra de 3 px, que a esta escala es una mancha.
  (Diario también ponía `listRowRadius = 0`, pero el tema se borró en 1.5.91: la placa va con Lyra y nada más.)
- **Ajustes → Sistema → Memoria pintaba las filas encima del título.** `visible()` deja pasar la fila que cruza
  el borde de arriba —tiene que hacerlo, o la lista saltaría de a bloques enteros—, así que esa fila sobresalía
  sobre el cabezal. Ahora el cabezal se dibuja **al final**, sobre una banda tapada en blanco, y lo mismo abajo
  con la barra de botones. Además el tope del área útil sale del pie REAL de ese cabezal (que esta pantalla
  dibuja a mano) y no de `listui::contentTop()`, que sale del tema: los dos no coincidían, y el recorte y el
  tope del desplazamiento tienen que salir de la misma cuenta o la última fila queda inalcanzable.

### Lo que dijeron los números del panel (medidos en el aparato, 1.5.80)

    FAST · 581 ms   (94 refrescos, máx 619)
    HALF · 1788 ms  (5,  máx 1793)
    FULL · 2190 ms  (5,  máx 2194)

**La onda parcial NO es el problema**: 581 ms. El `display=2227ms` del log viejo era un **FULL**, no un parcial.
O sea que la página lenta de 2,5 a 4,8 s no se arregla con una LUT propia — eso queda descartado — y lo que
falta medir es el RENDER (`bw_render`, las dos pasadas de gris, `cleanup`), que es justamente lo que dice la
línea `Page render:` del log… que no llegaba porque `/api/log` estaba caído. Primero el log, después el número.

## La velocidad de lectura, medida de verdad (1.5.83)

Con el log arreglado llegaron las líneas `Page render:`, y la historia era otra. Una página **de texto** cuesta
**1230 ms**, no 2,5 a 4,8 s:

    prewarm=35  bw_render=53  display=583  gray_lsb=79  gray_msb=81  gray_display=366  cleanup=27  total=1230

Las de 3149 y 4793 ms del log son **páginas con imagen la primera vez**: ahí `bw_render` salta a 1985 y 2399 ms
porque se saca el JPEG del ZIP, se decodifica y se escribe el `.pxc`. La segunda visita a esa página ya no paga
eso. Y la de 2744 ms es una que le tocó el **FULL** de la cadencia (`display=2194`). O sea que el "2,5 a 4,8 s por
página" nunca fue el caso normal.

De los 1230 ms, **949 son dos ondas del panel**: la base (`display`, 583) y la pasada de gris
(`gray_display`, 366). El resto —CPU— son 281 ms.

**Lo que se arregló**: los dos planos de gris (48 KB cada uno) se pedían con `new[]` y el permiso se miraba
contra `ESP.getFreeHeap()`. Con un libro abierto ese heap está bajo, así que `planeBufFits()` daba false y el
camino **asíncrono** —el que pinta los planos MIENTRAS corre la onda de la base— no se tomaba casi nunca: por eso
el log decía `Page render (tiled)` y nunca `(tiled async)`. Ahora los planos salen de **PSRAM** por
`heap_caps_malloc` y el hueco se mide contra la PSRAM, que es donde de verdad iban a caer. Son 160 ms que pasan a
ser gratis, y de paso 96 KB que dejan de amenazar al heap interno, que es el escaso (234 KB contra 8 MB) y el que
necesitan el TLS y el parseo del EPUB. Cuando aun así no alcance, ahora **lo dice el log** en vez de que la
página lenta parezca cosa del panel.

**Lo que queda para bajar de 1 s** son las dos ondas, y las dos salidas cuestan algo:
- **apagar el antialiasing** saca `gray_lsb + gray_msb + gray_display + cleanup` = 553 ms (página ≈ 680 ms), a
  cambio de texto sin suavizar;
- **combinar la base con la pasada de gris en una sola onda** (lo que `combinesGrayscaleBase()` ya hace en el
  Paper Mono) ahorraría los 583 ms de la base, pero en este panel **no está probado** y si sale mal la página
  queda con los grises sin base debajo.
Es una decisión del usuario, no técnica: hay que preguntarle antes de tocarla.

Además: `DictionaryRegistry::discover()` corre en CADA reconstrucción del menú de Ajustes, y en un aparato sin
diccionarios su `No /.dictionaries directory` solo llenaba el log de 24 KB que se sube al servidor. Ahora avisa
una vez por arranque y por raíz.

## Ajustes → Pantalla: lo que sobraba (1.5.84)

El selector de Interfaz ya anda, pero la sección tenía **cinco filas que no hacían nada en esta placa** y una de
ellas además costaba plata:

- **Fondo de pantalla, Modo de portada, Filtro de portada y Quick Resume**: se fueron con las fotos en 1.5.74.
  Acá `goToSleep()` va con `render=false` y **la pantalla de sueño del SDK no se pinta nunca** — la pinta
  `SleepScreen`, que dice SUSPENDIDO o APAGADO y no tiene portadas ni filtros. Las cuatro filas cambiaban un
  número que en la ws397 ya no lee nadie.
- **Arreglo de desvanecido al sol**: es el `turnOff` del refresco, y las tres secuencias de este panel (0xFF,
  0xD7, 0xF7) **ya traen los bits de apagado**, así que el driver lo ignora (`sequencePowersOff` en
  `Ssd1677Driver::refresh`). Lo único que hacía encendido era apagar el camino asíncrono
  (`supportsAsyncRefresh()` es `!fadingFix && …`) y sumarle **~160 ms a cada página**. Costo sin beneficio.
  Y como esconder la fila sin más dejaría a quien lo tuviera encendido pagando eso para siempre y sin puerta
  para apagarlo, `main.cpp` además lo **fuerza a 0** en el arranque, igual que `shortPwrBtn`.

Quedan cuatro filas y las cuatro mandan de verdad: Ocultar batería, Frecuencia de refresco, Interfaz y Modo
noche.

**Modo noche no se toca pero hay que saber lo que cuesta**: prende `display.setInverted(true)`
(`ActivityManager`, una vez por render), y tanto `supportsStripGrayscale()` como `supportsAsyncRefresh()` son
`!_inverted && …`. O sea que con el modo noche encendido la página pierde **las dos** cosas: el gris por tiras y
el solape, y cae al camino viejo (`storeBwBuffer`, dos renders enteros de página, `restoreBwBuffer`). En el log
se distingue solo: la línea pasa a ser `Page render:` con `bw_store` y `bw_restore` en vez de
`Page render (tiled async):`.

## Auditoría externa y caza de huérfanos (1.5.86)

El usuario mandó a auditar el repo por afuera y pidió, textual, "verifiques que no te hayas mandado otro pedo
cuando sacaste cosas, porque cada vez que revisas me salta un pedo nuevo". Las dieciséis observaciones se
verificaron **una por una contra el árbol**: **8 reales, 7 exageradas o dadas vuelta, 1 falsa**. Lo que salió:

- **El token del aparato se podía BAJAR, y eso la auditoría sólo lo vio a medias.** Marcó que PROPFIND de WebDAV
  no chequea `isProtectedPath` —cierto, pero PROPFIND devuelve propiedades, no cuerpo—. Lo grave estaba al lado:
  `handleDownload` miraba **sólo el último segmento** de la ruta, así que `/download?path=/.crosspoint/server.json`
  daba `server.json`, que no empieza con punto, y contestaba 200 con el token en claro. Igual `wifi.json` y
  `device.log`. Y en la ws397 ese servidor se levanta sobre el punto de acceso **abierto** de "clave por el
  teléfono": alcanzaba con estar cerca. Arreglado en 1.5.85 con `isProtectedItemPath()` (todos los segmentos) y la
  comprobación de WebDAV movida al despacho.
- **CI estaba muerto, no incompleto**: el disparador era `push` a `master` y **`master` no existe en el remoto**.
  Nunca corrió. Ahora corre en `ws397`, compila las seis placas, y suma las cuatro pruebas de escritorio que
  existían hace versiones sin que las corriera nadie, más el `tsc` del servidor.
- Vinculación con `crypto.randomInt` y freno de intentos; registro con freno **antes** del Argon2; SSRF
  (`::ffff:127.0.0.1` pasaba derecho) con prueba propia en `./test/ssrf/run.sh`; y tope de cuerpo por ruta.

Y la caza de huérfanos —la parte que el usuario pidió— encontró **dos cosas mías, del mismo tipo que el
`/api/log`**:

- **`blobCache` y `loadBlob` los borró el commit de las fotos (1.5.74) creyendo que eran de las fotos**, y los
  compartía la vista previa de los adjuntos de **Viajes**. Desde entonces tocar un papel de un viaje en `/board`
  tiraba `ReferenceError` y **no mostraba nada ni avisaba**: la excepción cae adentro de una promesa sin await, o
  sea fuera del `try` del handler, así que ni siquiera se veía como error. `node --check` pasaba igual. Restaurados.
- **Las tarjetas se seguían mandando.** `CARDS_IN_PACK = false` (1.5.65) apagó la GENERACIÓN, pero el manifiesto
  se sirve desde el índice **persistido**, así que un servidor que ya las había armado las anunciaba para siempre:
  en el aparato del usuario eso eran **787 archivos** en el manifiesto. `loadIndex()` las poda. (Esa poda filtraba
  por `kind`, que dejaba pasar los 480 audios; se completó en 1.5.91, cuando además se borró `cards.ts` entero.)
- **"Menú de pulsación larga" estaba escondido con una premisa que había dejado de ser cierta.** Se escondió en
  1.5.38 porque con OK = confirm + power la rama no se disparaba nunca; en 1.5.47 el encendido pasó al PMIC y en
  1.5.49 se sacó el `if (WS397) return 0;`. O sea que desde 1.5.49 la rama funciona **pero el ajuste que la
  enciende estaba escondido**, y esconder una fila acá no es sólo sacarla del menú: `toJson`/`fromJson` y el
  `/api/settings` del aparato recorren la MISMA `getSettingsList()`, así que el campo queda clavado en su default
  para siempre. El marcador por OK mantenido era inalcanzable y CLAUDE.md decía que "ahora manda el ajuste de
  siempre". Decía mal. Vuelve a estar.

**Regla que sale de esto**: esconder una fila de `getSettingsList()` en una placa **también** le saca la clave al
archivo de ajustes y al `/api/settings`. El campo queda en el default del struct y no hay puerta para moverlo. Por
eso cada entrada de esa lista tiene que decir por qué el default es el valor correcto — y hay que volver a mirarla
cuando cambia el motivo.

## Auditoría de la cabeza sin revisar (1.5.91)

Cuatro fusiones (1.5.87 a 1.5.90) habían entrado sin que las mirara nadie: 121 archivos, ~1600 líneas. Se
auditaron con quince agentes en paralelo y cada hallazgo se verificó adversarialmente contra el árbol antes de
creerle: **118 hallazgos, 28 confirmados, 48 refutados, 42 sin verificar** (se acabó el límite de sesión). Lo que
salió:

- **El token se podía LEER otra vez, y no por un archivo.** En 1.5.85 se tapó `/download`; la puerta de al lado
  quedó abierta. `getSettingsList()` tiene dos entradas `DynamicString` cuyos getters devuelven
  `SERVER_STORE.getToken()` y `KOREADER_STORE.getPassword()`, y `handleGetSettings()` las emitía como
  `doc["value"]`. `GET /api/settings` **no pide credencial**, así que sobre el punto de acceso ABIERTO de "clave
  por el teléfono" alcanzaba un `curl` desde la vereda. Ahora hay una marca `secret` en `SettingInfo`: el valor se
  ESCRIBE pero no se LEE (viaja `hasValue` y el valor vacío, la misma regla que ya cumple el servidor de Railway
  con las claves de IA). Como la página sólo manda las claves que cambiaron, no tocar el campo no lo borra.
- **DECISIÓN DEL DUEÑO: `POST /api/settings` queda SIN credencial, a propósito.** Sí, eso significa que un vecino
  en el punto de acceso abierto puede escribir `srvUrl` y `srvToken` y apuntar el aparato a su propio servidor.
  Se deja así porque ese endpoint ES la forma de cargar la clave del WiFi desde el teléfono y de configurar el
  servidor desde la web, y cerrarlo sacaría las dos cosas. **No cerrarlo sin preguntar**: ya está evaluado.
- **El arreglo de 1.5.85 era sólo de lectura.** `/upload`, `/mkdir` y el subidor por WebSocket nunca llamaron a
  ninguna guardia: se podía escribir dentro de `/.crosspoint`, que es peor que leer — plantando ahí un
  `server-queue.json` armado a mano, el aparato manda esos POST **firmados con su Bearer** en la próxima
  sincronización — y en un aparato nuevo hasta se podía CREAR el directorio.
- **La regla de rutas protegidas estaba escrita DOS veces** (`CrossPointWebServer.cpp` y `WebDAVHandler.cpp`) y
  las copias se fueron separando: por eso 1.5.85 arregló una sola. Vive en **`src/network/ProtectedPaths.h`** y la
  usan las dos. De paso tapa el **alias 8.3 de FAT**, que esquivaba el filtro por nombre (`.crosspoint` tiene
  nombre corto `CROSSP~1`, que no empieza con punto). Un `libro~1.epub` de verdad no cae: el 8.3 no llega a cuatro
  letras de extensión.
- **La guardia de Lua no servía contra `pcall`.** `stepHook` tira un error de Lua común y `pcall` está en la base,
  así que `while true do pcall(function() while true do end end) end` se lo comía en cada vuelta. Y como
  `runBounded` espera al worker con `portMAX_DELAY`, el que se colgaba no era la app: era el loop de Arduino, sin
  recordatorios, sin reposo y sin salida salvo cortar la corriente. La promesa escrita acá valía sólo para el caso
  ingenuo. Se cierra por donde Lua no puede seguir: pasarse del tope **condena al intérprete** y el asignador deja
  de dar memoria, así que no puede ni armar el objeto de error ni entrar a un `pcall`. Va en dos tiempos para no
  perder el mensaje entendible en el caso normal. `lua_close` también arma la guardia: corre los finalizadores
  `__gc` —código de la app— desde el loop de Arduino, o sea que un `__gc` infinito colgaba el aparato justo en la
  puerta de salida.
- **Noticias perdió dos cosas al pasar de `fetch()` a un cliente propio** (el pin de DNS de 1.5.90): la
  descompresión **gzip**, que el `fetch` global hacía gratis y sin la cual un feed comprimido llega como bytes
  crudos y el parser no encuentra nada; y el **plazo**, porque `request.setTimeout` es INACTIVIDAD del socket —un
  servidor que gotea lo resetea para siempre— y la resolución de nombres no tenía ninguno.
- **Las tarjetas se seguían mandando a medias.** La poda de 1.5.86 filtraba por `kind !== "cards"`, pero los DOS
  audios de cada tarjeta viven bajo `sounds/<lang>/<id>` con kind `"sounds"`: se iban los 240 BMP y quedaban los
  480 audios anunciados para siempre. **La mitad de un arreglo es peor que ninguno, porque parece hecho.**
- **`.ws397-build` decía 89 con el header en 90.** Es el único archivo que lee la aritmética de `release.sh`, así
  que el próximo release publicaba un binario DISTINTO bajo el número 1.5.90 — que ningún aparato que ya lo
  tuviera habría instalado nunca (la comparación es `major.minor.patch` estricta).

Y la limpieza que pidió el dueño, de lo que ya estaba afuera del producto: `GameUi.{h,cpp}` (253 líneas de los
juegos compilados), `gen_game_icons.py`, `search24.h`, los íconos de Fotos y del Conversor, `HubStore::wallpaperName`,
**197 claves de traducción × 7 idiomas**, `server/src/cards.ts` entero con todo su dibujante de emojis de Noto
(`assets.ts` pasó de 694 a 424 líneas), `photosDir()`/`PHOTOS_DIR`, un import muerto y el CSS de la grilla de fotos.
Ojo con el número de las claves: la tabla de strings bajó 29 KB pero **el binario sólo bajó 644 bytes**, porque el
build ya las venía descartando. Es limpieza para que no vuelvan si alguien regenera sin `--strip-unused`, no flash.

**Lo que NO se tocó y hay que saberlo**: los 14 íconos de 64 px de `hubIcons.h` no los usa nadie (`TILES[]` usa los
de 48; el comentario del manifiesto decía lo contrario y se corrigió). Se dejan porque son un par generado, no una
función muerta.

## La noche en que se vació la batería (1.5.92)

El aparato quedó en reposo boca abajo sobre la mesa y a la mañana estaba en 0 %. El log lo dijo entero: la
misma tanda repetida cada diez minutos, toda la noche.

```
=== 1.5.90-ws397 | arranque por temporizador === bateria 6%
[1110] [MOTION] boca abajo
[1112] [REMIND] gesto: se pospone el recordatorio
[2324] [RTCAL] alarma armada para ... 20:10:00 UTC
```

- **ESTAR boca abajo no es DARLO VUELTA.** `MotionInput::faceDown_` arranca en false en cada arranque, así que
  la primera lectura de un aparato apoyado sobre la tapa se leía como la transición y emitía el gesto ~1,1 s
  después del boot. Con un recordatorio sonando, eso es posponerlo solo. Ahora **la primera muestra sólo CEBA**
  las trabas de posición (`faceDown_`, `level_`, `tilted_`) y no emite nada, y la pantalla del recordatorio
  además exige ver un "boca arriba" antes de aceptar el gesto. Vale para todos los gestos de POSICIÓN: la
  posición en la que el aparato ya estaba nunca es un gesto.
- **Una alarma que nadie atiende era un ciclo infinito, y eso existía desde siempre.** A los 60 s dejaba de
  pitar pero **no se posponía**, así que el recordatorio seguía vencido; con un vencido `nextWakeInstant()`
  devuelve "ahora", el deep sleep se arma al piso de 5 s y el aparato se despierta a repetir. Ahora a los 60 s
  **se posterga sola** y hay un **tope de 3 postergaciones** (`HubStore::MAX_SNOOZES`, contador persistido en
  `hub.json` porque cada repique es un arranque distinto): a la cuarta se **descarta** — si el recordatorio
  repite, la ocurrencia de hoy se pierde y queda armada la próxima; si no repite, se borra. Son cuatro repiques
  en media hora y se acabó. El aviso al servidor lleva `dismissed: true` para que "hecho" y "me cansé de sonar"
  no se confundan.
- **Volver al hub después de una alarma que nadie atendió cuesta diez minutos de aparato encendido.** El hub
  puede levantar WiFi para sincronizar y, sobre todo, el auto-sleep recién corta a los diez minutos — justo
  cuando la alarma vuelve a sonar. O sea el 100 % de la noche despierto. `src/util/SleepRequest.h` deja que una
  pantalla que se abrió sola y se resolvió sola **pida dormir**; lo atiende el loop de `main.cpp`, que es el
  único que puede llamar a `enterDeepSleep()` con el aparato consistente.
- **Y de paso: la alarma volvía al hub desde CUALQUIER lado.** `checkTimeAlarms()` la abre con `pushActivity()`,
  que no deja `resultHandler`, así que el `else` de `leave()` mandaba a todas al hub: una alarma que sonaba en
  Notas o en la agenda te dejaba en el hub al atenderla. Es el mismo defecto que tenía Hablar en 1.5.70 y se
  arregla igual, preguntando por `hasStackedActivities()`.

**La tarjeta dañada hacía que el aparato se creyera nuevo.** Al tirón de batería le siguió un arranque con el
asistente de primeros pasos y un token nuevo. Los dos salen de la misma confusión: `loadFromFile()` devuelve
false igual si el archivo NO ESTÁ que si está y no parsea, y un JSON ilegible se lee como store vacío.

- `ensureToken()` acuñaba identidad nueva con sólo ver el token vacío. Acuñar **cambia la identidad del
  aparato**: hay que volver a vincularlo a mano y, hasta que alguien lo haga, lo que suba va a otra cuenta.
  Ahora, si `/.crosspoint/server.json` **existe**, no se acuña nada: se queda sin token (el servidor contesta
  401, que se ve) y espera. Perder el token no pierde datos —son de la CUENTA— pero inventarlo sí confunde.
- `SetupActivity::pending()` preguntaba "¿sincronizó alguna vez?" y "¿hay redes?", que se contestan con lo que
  se pudo LEER y no con lo que hay. Ahora, si `hub.json` o `wifi.json` **existen**, el aparato tuvo una vida
  antes aunque hoy no se los pueda leer.

**El log subía siempre lo mismo.** El aparato mandaba el final de su log en cada sincronización y el servidor
**apenda**, así que `/board/log` era la misma tanda repetida seis veces con arranques de firmware de hace
semanas en el medio; buscar lo de recién era imposible. Ahora el aparato sube **sólo lo nuevo** desde la última
subida confirmada (marca de bytes en `/.crosspoint/log.sent`, que vale entre arranques porque el archivo se
abre en modo agregar; rotar la borra y eso se dice en el archivo nuevo), y el servidor **poda lo guardado a
24 h** con los sellos ISO que ya escribía. La poda nunca deja la página vacía: si nada entra en la ventana,
queda la última subida igual — "viejo" es un diagnóstico, una pantalla en blanco no es ninguno. Se prueba sin
servidor: **`./test/device_log/run.sh`**.

## Noticias no traía nada, y no era de los diarios (1.5.92)

En `/board` → Ajustes → Noticias, los tres feeds decían lo mismo: **`Invalid IP address: undefined`**. Que los
TRES fallen igual ya dice que no es ningún diario caído.

El DNS se fija a propósito (`server/src/net.ts`, desde el pin de 1.5.90): se resuelve el nombre, se comprueba
que la dirección no sea de la red interna de Railway y la conexión se clava a ésa, así un feed no puede
redirigir a 169.254.169.254. Para eso se le pasa un `lookup` propio a `http.request`. **Pero `http.request` no
llama a ese lookup como uno lo escribiría: le pasa `{ all: true }` y espera un ARRAY de `{address, family}`.**
El nuestro devolvía siempre un string suelto, así que el cliente hacía `results.sort(...)` sobre algo que no era
un array —o leía `results[0].address`, que da `undefined`— y la salida moría **antes de abrir el socket**. Todos
los feeds, todos los artículos, siempre.

Es un error que no se ve leyendo el código: hay que ejercitarlo. Por eso queda **`./test/net_lookup/run.sh`**,
que levanta un servidor local y comprueba las dos cosas: que el callback nuevo conecta y que el viejo falla.

De paso, dos cosas que hacían que el síntoma no se pudiera leer:
- `selectResolvedAddress()` filtraba sólo por "no es red interna", así que una entrada sin dirección pasaba
  derecho hasta el socket. Ahora se pide `isIP()` primero.
- "el DNS no devolvió nada" y "resuelve a red interna" eran el mismo mensaje. Son cosas distintas y mandan a
  buscar el problema a lugares distintos.

**PUBLICAR FIRMWARE Y DESPLEGAR EL SERVIDOR SON DOS COSAS DISTINTAS, y hay que hacer las dos.** Esto se descubrió
acá: los commits de servidor de 1.5.91 —gzip, plazos, la poda de tarjetas— nunca habían llegado a producción,
porque `release.sh` sube el `.bin` al volumen y **eso no redespliega nada**. Railway construye desde la rama
**`ws397`** (Settings → Source: ahí están la rama Y el Root Directory = `server`, que son dos ajustes distintos
y es fácil confundirlos). Todo eso se fusionó y se desplegó junto con 1.5.92.

Regla, entonces: un arreglo del servidor no existe hasta que **`ws397`** lo tiene. Si el síntoma sigue después de
"arreglarlo", lo primero que hay que mirar es `git log origin/ws397..HEAD`.

## Lo que encontró la revisión adversarial de 1.5.92

Los arreglos de arriba se mandaron a revisar por afuera (un agente sobre los caminos de energía, otro
adversarial sobre el propio diff) y cada hallazgo se verificó contra el árbol antes de creerle. Salieron
**dos arreglos a medias míos** y **cuatro defectos de fondo que venían de antes**:

- **EL LOG SE BORRABA EN CADA ARRANQUE, y ése era el problema de verdad.** `openFileForWrite()` del SDK abre
  con `O_TRUNC`, así que el `if (append) f.seek(f.size())` de `devlog::begin()` era un no-op sobre un archivo
  que ya estaba en cero. O sea que `device.log` nunca acumuló nada entre arranques, y lo que quedaba para subir
  era casi siempre `device.prev.log` —viejo y que no cambia—, que se comía el presupuesto de 24 KB. De ahí las
  mismas tandas repetidas con firmware de hace semanas. El arreglo de 1.5.55 (mandar CURRENT antes que PREVIOUS)
  atacó el orden, que era la mitad. Ahora se abre con `Storage.open(CURRENT, O_RDWR | O_CREAT)`, que sí agrega.
- **La red de seguridad del reposo no podía dispararse NUNCA.** La compuerta medía `lastActivityTime`, que unas
  líneas más arriba —en la misma pasada del loop— reinician `preventAutoSleep()`, la música y PWR. O sea que
  cuando el reposo estaba bloqueado POR alguna de esas tres, la compuerta valía ~0 ms y `restBlockedSince` se
  reiniciaba: código muerto, y justo para el caso que dice cubrir. Los dos plazos miden ahora `lastUserInputTime`.
- **"Clave del WiFi por el teléfono" no tenía plazo.** Ese estado levanta el punto de acceso, atiende un servidor
  web y pide `skipLoopDelay()` + `preventAutoSleep()`: radio transmitiendo y CPU al 100 % sin que el auto-sleep
  pueda intervenir. Y está en el paso 2 del asistente de primer arranque, o sea que alcanza con distraerse. Diez
  minutos y se baja, como ya hacía `UsbDriveActivity` con su `HOST_WAIT_TIMEOUT_MS`.
- **Sin reloj, posponer escribía `dueAt = 600`** (enero de 1970, o sea vencido para siempre): el retorno de
  `getEpochUtc()` se ignoraba. Con eso `nextWakeInstant()` devuelve "ahora", el deep sleep se arma al piso de 5 s
  y el aparato arranca en bucle — y el tope de postergaciones no lo corta, porque `giveUp()` reinicia la racha.
  Ahora sin reloj no se toca el `dueAt`.
- **El descarte no llegaba al servidor.** `at` es la guardia antirreplay de `markDone`, y después de tres
  postergaciones no puede coincidir nunca: el `dueAt` del aparato es "ahora + 600" con segundos y el del servidor
  está truncado al minuto y corrido por el desfase de reloj que el firmware tolera hasta 120 s **sin corregir**.
  El descarte se perdía entero y la sincronización siguiente resucitaba la alarma. El `dismissed` ya no manda
  `at`: su idempotencia sale del ESTADO (sólo se cierra lo que sigue vencido), que es cierto aunque los relojes
  no coincidan. Y queda anotado en `dismissedAt`, que antes era un `console.log` y se perdía.
- **El mismo desfase rompía el contador de postergaciones**: la ventana de "misma ocurrencia" eran 120 s, menos
  que el error que el propio sistema tolera. Son 15 minutos, que separan holgadamente una postergación (10 min)
  de la ocurrencia siguiente (24 h en la repetición más corta).
- **Cebar la posición del IMU no alcanzaba con hacerlo al arrancar**: el reposo apaga el acelerómetro, así que la
  primera lectura al volver se juzgaba como transición contra trabas de hace horas. Un recordatorio que vence
  durante el reposo se auto-postergaba igual. Ahora se ceba también al reencender el chip.
- **`RTC_ALARM.fired()` contaba como actividad del usuario.** `clearFlag()` no comprueba la escritura, así que un
  bus que lee bien pero no escribe deja AF puesta y `fired()` en true para siempre: el contador de ocio se
  rearmaba cada cinco segundos y el aparato no volvía a dormir nunca.
- **"Timer wake sin reloj" reintentaba cada 60 s sin tope**: con la pila del RTC agotada, sesenta arranques por
  hora para siempre. Cinco intentos (contador en RTC RAM) y después se espera el botón.
- **Mi guardia del asistente lo mataba en un aparato REALMENTE nuevo.** "Existe hub.json → no es nuevo" es falso:
  apagar con PWR en la pantalla de idioma pasa por `powerOffNow()`, que hace `HUB_STORE.saveToFile()`. La
  pregunta correcta es "existe y NO se pudo leer". De paso: `WIFI_STORE` no lo carga nadie en el arranque, así
  que esa guardia leía cero siempre y no guardaba nada.
- **`ensureToken()` dejaba un callejón sin salida**: sin token, el servidor rechaza el vacío y vincular tampoco
  se podía. Ajustes → Vincular llama ahora a `mintToken()`, que acuña igual — porque ahí sí hay alguien
  pidiéndolo, que es exactamente lo que faltaba.
- Y lo chico: la marca del log confirmaba `n` aunque `read()` devolviera menos (se perdían bytes en silencio);
  la prueba de servidor decía "no se pudo enviar el log" cuando simplemente no había nada nuevo; la repetición se
  calculaba desde el `dueAt` ya postergado, así que un diario sin WiFi se corría media hora por día
  (`baseDueAt`); una sacudida dada ANTES de que la alarma existiera se consumía como respuesta a la alarma; y
  `devlog::tail()` quedó sin llamadores y se borró.

**Queda sin hacer y anotado**: el fondo de pantalla se repinta con un FULL (2190 ms) en cada sueño sin comparar
contra lo que ya está en el vidrio; `OpdsBookBrowserActivity` pide `preventAutoSleep()` incondicional (es
upstream); Ajustes → Movimiento a mitad de calibración no deja dormir; la música con repetir no se corta nunca; y
`msUntilNextAlarm()` hace una lectura I²C del RTC en cada pasada del loop.

## El DOBLE GOLPE que no abría Hablar (1.5.92)

Tres gates, y los tres estaban mal por el mismo motivo: se escribieron pensando en el aparato **apoyado en la
mesa**, y el doble golpe se da con el aparato **en la mano**.

- **`faceUp` estaba al revés.** Pedía `r.n > 0,3`, o sea la pantalla mirando para arriba, a menos de unos 70° de
  la horizontal. Sostenido como se lee, la normal queda casi horizontal (`n ≈ 0`) y el gesto se descartaba
  **siempre**. Lo único que hay que evitar es confundirlo con apoyarlo boca abajo —que es `n` bien NEGATIVA—, así
  que ahora se pregunta eso: no que mire arriba, sino que **no mire abajo**.
- **Un evento de posición sin consumir bloqueaba 1,5 s.** `emit()` no pisa lo que ya está pendiente si tiene menos
  de 1,5 s, e **inclinar** y **horizontal** se emiten solos con sólo mover el aparato — y en el hub NADIE los
  consume (`checkMotionGestures()` sólo toma sacudir, boca abajo y doble golpe). O sea que levantar el aparato y
  darle los dos golpecitos, que es exactamente cómo se usa, caía casi siempre adentro de esa ventana. Ahora un
  gesto **deliberado** (doble golpe, sacudida) le gana a uno de posición: lo que pisa no es de nadie.
- **El antirrebote genérico de 350 ms frenaba al motor de golpes del chip.** Ese antirrebote existe para que los
  gestos por UMBRAL no se disparen en cadena entre ellos, y el doble golpe **no sale de un umbral**: lo detecta el
  chip. Levantar el aparato emite "inclinar", y el golpe que venía justo después se descartaba. Se sacó de ese
  gate; el doble golpe ya tiene las dos guardias que le corresponden — `rested` (1,5 s entre golpes) y `quiet`
  (1,2 s después de una sacudida de verdad).

La línea del log (`golpe: st1=… tap=… doble n=…`) lleva ahora la normal medida, que es lo que hacía falta para ver
cuál de los gates cortaba. Si el motor del chip directamente no contesta, eso se ve en el arranque
(`golpes: el motor del chip contestó` o el error con el motivo) y en **Ajustes → Movimiento**.

## El doble Atrás que no abría Hablar (1.5.92)

Dos defectos, y el segundo explica por qué el primero no se podía diagnosticar.

- **La ventana eran 500 ms, que es la medida de un doble clic de MOUSE.** Esto no es un mouse: es un botón
  físico en un aparato de tinta que **no da ninguna señal entre un toque y el otro**. El que lo prueba toca, no
  ve pasar nada, y recién ahí toca de nuevo — eso son 700 u 800 ms tranquilamente. Y si el primer toque cambió
  de pantalla (en Notas o en la agenda, Atrás sale), en el medio hay un cambio de Activity que toma el candado
  del render. Son **1,2 s**, que sigue lejos de dos Atrás separados de verdad.
- **Una pulsación LARGA de Atrás contaba como toque.** `wasLongPressed()` marca la suelta como suprimida, pero
  `wasReleased()` **no mira esa marca** (la única que la mira es `consumeSuppressedRelease()`, que usa el camino
  del botón de despertar). Así que mantener Atrás —que en el hub sincroniza y en las listas abre el menú del
  ítem— llegaba igual como un toque: sincronizar y después tocar una sola vez abría Hablar sin que nadie lo
  pidiera. Ahora un Atrás de más de 600 ms no cuenta y además cierra la ventana.
- **Y el atajo ahora dice por qué no disparó.** Era una función que sólo dejaba una línea en el log cuando
  FUNCIONABA, o sea justo cuando no hace falta: las cuatro causas de "no pasó nada" (toques demasiado separados,
  pantalla que usa Atrás para salir, grabación abierta, lector) eran idénticas desde el vidrio. Cada una deja su
  renglón con el número de milisegundos.

Recordar que la lista de pantallas tranquilas (`isCalmScreen`) sigue valiendo **a propósito**: en Noticias, la
Biblia, el Traductor o una app de Lua, Atrás es el botón con el que se sale, y robárselo dejaría pantallas sin
salida. Ahí el log lo dice en vez de no hacer nada en silencio.

## Lo que el aparato encontró en 1.5.92 (arreglado en 1.5.93)

- **La batería medía "desde el origen de los tiempos".** `analyze()` toma la ventana más larga hacia atrás que
  sea una descarga limpia, pero **no miraba las FECHAS**. Una sola muestra anotada con el reloj sin poner en hora
  —1970, o el **2000-01-01 con el que arranca el PCF85063 sin pila**, que pasa cualquier prueba de `> 0`— estira
  la ventana veinticinco o cincuenta y seis años: la pendiente se va a cero y la autonomía, a siglos. Ahora se
  exige fecha creíble (`credibleEpoch`, el mismo umbral de 2024 que usa el TLS) tanto al ANOTAR como al medir, la
  ventana se corta si la fecha va hacia adelante mirando hacia atrás (alguien puso el reloj en hora en el medio) y
  hay tope de dos semanas: una ventana más larga no es una descarga, es el aparato apagado. Ocho casos nuevos en
  **`./test/battery_drain/run.sh`** (25 en total).
- **EL REPOSO NO DECÍA NUNCA POR QUÉ NO ENTRABA.** De las cuatro razones por las que no reposa, sólo una dejaba
  rastro (el rechazo del kernel): el bloqueo y el tope diminuto devolvían `NotSlept` en silencio. O sea que "el
  reposo nunca entró" era un síntoma sin una sola línea detrás. Ahora se anota cuál de las siete cosas lo bloquea
  (`el reposo no entra: el cable está puesto`), y sólo cuando CAMBIA y sólo con el aparato ocioso.
- **Y había un bloqueo de verdad: un vencido que la pantalla de turno no va a atender apagaba el reposo.**
  `msUntilNextAlarm()` devuelve 1 ms cuando hay algo vencido y con ese tope `IdleSleep::tick()` no reposa
  (`MIN_REST_MS` son 500). Está bien mientras la alarma esté por sonar — pero **el lector está excluido a
  propósito** de `checkTimeAlarms()`, así que un recordatorio que vence leyendo dejaba el tope en 1 ms PARA
  SIEMPRE: 40 mA sin reposar hasta el auto-sleep, y de nuevo con cada repique. Ahora, si la pantalla de turno no
  lo va a atender, el vencido no cuenta para el tope: reposar no lo hace más tarde de lo que ya está. Las cuatro
  guardias viven en **`alarmWouldRingHere()`**, que consultan el reposo Y `checkTimeAlarms()`: escritas dos veces
  se habrían separado, como pasó con las rutas protegidas en 1.5.91.
- **El medidor de OK mantenido salió de Ajustes → Sistema → Memoria.** Existía para contestar sin cable si el
  evento de pulsación larga llegaba (hasta 1.5.46 no llegaba nunca y nadie lo había vuelto a probar). Ya está
  contestado y el marcador del lector anda: era diagnóstico ocupando una pantalla que se mira por otra cosa. Se
  fueron con él sus cuatro claves × 7 idiomas.
- **Una línea por pintada llenaba el log.** `Time = NN ms from clearScreen to displayBuffer` salía en CADA
  pantalla dibujada y, con la del refresco, era más de la mitad de los 24 KB: leer ahí lo que acababa de pasar era
  imposible. Dibujar cuesta 40-90 ms y eso no es noticia; **un pico de 1,4 s sí** (los hay, en Ajustes), y es lo
  único que se busca al abrir este log. Ahora sale sólo por encima de 150 ms, y las normales se cuentan de a
  tandas para que el número no se pierda.

**Lo que el log confirmó y no hay que volver a discutir**: el gate del doble golpe estaba rechazando dobles de
verdad. La línea `golpe: st1=1 tap=B2 doble (no mira arriba: se ignora)` es exactamente eso, y un minuto antes
está la misma con el aparato en otra posición abriendo Hablar. El arreglo de 1.5.92 tiene evidencia.

**Sin explicar todavía**: `[DREG] No /.dictionaries directory on SD card` sale una vez por entrada a Ajustes pese
a que la guardia de una-vez-por-arranque está puesta desde 1.5.83 y es un `static bool` común. Es una línea DBG,
no rompe nada, pero **la guardia no está haciendo lo que dice** y eso hay que mirarlo con el aparato delante.

## La Biblia con menú, y Viajes afuera (1.5.93)

**La Biblia se lee como un libro, así que tiene que tener el menú de un libro.** Leyendo un capítulo, **OK abre
el menú** igual que en el lector de CrossPoint: *Buscar una palabra*, *Preguntar sobre este capítulo*, *Buscar
por voz* y *Seleccionar capítulo*. Antes lo único que había era Atrás mantenido, y la barra de abajo mostraba
"Mantén: preguntar" **en el lugar de "Atrás"**: contaba lo que hace mantenido y escondía lo que hace tocando.

- El menú vive en `DictionaryDefinitionActivity`, que es el visor de TODO lo que no es un libro (la respuesta de
  Preguntarle al libro, la de Hablar, una nota, un capítulo). El que lo abre agrega sus entradas con
  `addMenuItem()`; sin entradas, OK no hace nada y la pantalla se comporta como siempre.
- **"Buscar una palabra" la pone el visor solo**, y tiene que ser así: el único que sabe dónde cayó cada palabra
  en el vidrio es el que dibujó los renglones. El cursor recorre las palabras de la página EN PANTALLA en orden
  de lectura y sale de los mismos `lines` y las mismas medidas que `drawBody` — si fueran dos maquetaciones, el
  resalte caería en otro lado que la letra. El renglón que abre un versículo es donde se vería: el número va en
  SMALL negrita y corre el texto a la derecha, así que el cursor arranca de `penX`, no del margen.
- El cursor dibuja **sin la pasada de grises**: cada movimiento pagaría los 160 ms de los dos planos para
  suavizar un texto que ya se leyó. Entrar y salir del cursor sí piden refresco limpio, porque lo que hay en el
  vidrio salió del pipeline de grises y el cursor es blanco y negro.
- **La lógica del diccionario estaba por duplicarse y se sacó a `src/util/DictionaryLookup.h`** (`dictlookup`):
  abrir el diccionario elegido, armarle el `.qidx` la primera vez y traducir cada modo de falla a su cartel eran
  cuarenta líneas adentro de `DictionaryWordSelectActivity`. Ahora las usan las dos pantallas. Dos copias de una
  regla se separan solas: ya pasó con las rutas protegidas en 1.5.91.
- Sin diccionario instalado la fila lo dice (`STR_DICT_NO_DICT_SET`) y no pasa nada más. **Buscar significa
  diccionario local** y nunca se convierte solo en una consulta a la IA: para eso está *Preguntar*.

**Viajes salió del producto, entero.** Decisión del usuario: *"viajes sale de 'mi día', sale del firmware por
completo y de la web"*, y vuelve como app de Lua más adelante.

- Firmware: `TripActivity.{h,cpp}`, la tercera fila de "Mi día" (`ROW_TRIPS`), "Trip" de la lista de pantallas
  tranquilas de `main.cpp` y **17 claves de traducción × 7 idiomas**.
- Servidor: `src/trips.ts`, `src/attachments.ts` y `src/deviceBmp.ts`; las rutas `/api/trips`, `/api/trip*`,
  `/api/attachment*`, `/api/board/attachment` y `/api/suggest/trip`; el documento `trips` y el `attachments` de
  `fsjson.ts` con su `attachmentsDir()`.
- Web: la pestaña Viajes (**cinco** pestañas otra vez) con todo su editor, los adjuntos y su vista previa.
  `#viajes` y `#mas/viajes` redirigen a Hoy: alguien puede tener la dirección guardada en el teléfono.
- **Y los huérfanos que deja, que es donde está el pedo de siempre**: `src/util/FullScreenBmp` (el dibujante de
  BMP a pantalla completa) no lo usaba nadie más — CLAUDE.md decía que era de los adjuntos y tenía razón—;
  `deviceBmp.ts` tampoco; y con ellos se van **cuatro dependencias nativas** que solo estaban por los adjuntos:
  `sharp`, `mupdf`, `bwip-js` y `zxing-wasm`. La línea que decía que `sharp` no se podía sacar "porque lo usan
  los adjuntos y el paquete de contenido" era falsa en la segunda mitad: `assets.ts` dibuja solo. **Hay que
  regenerar `bun.lock`**, o el `bun install --frozen-lockfile` del Dockerfile falla.
- El viaje **espejaba sus ítems en `/data/calendar.json`** como eventos con `tripId`. Esos eventos ya no tienen
  dueño —nadie los puede editar ni borrar—, así que `normalizeCalendar()` los descarta al leer. Es de una sola
  vía a propósito: el viaje que los explicaba no existe.
- De paso se fue `STR_AGENDA_EMPTY`, que no lo usaba nadie desde que existe `STR_AGENDA_EMPTY_VOICE`.

## El reloj, la pila del RTC y un log de hace diez versiones (1.5.94)

El aparato volvía del apagado sin hora, la agenda decía que no estaba en hora y sólo se acomodaba al sincronizar
con el servidor. Y el log que vino con el reporte era de **1.5.82 o anterior**, cosa que se puede fechar sin
preguntar y conviene saber hacer:

- dice `[UI] Using Bento theme`, y `BentoTheme` se borró en **1.5.86**;
- repite `[DREG] No /.dictionaries directory` una vez por visita a Ajustes, y la guardia de una-vez-por-arranque
  entró en **1.5.83**;
- dice `Time = NN ms from clearScreen to displayBuffer` en cada pintada, y esa línea pasó a salir sólo arriba de
  150 ms en **1.5.93**.

O sea que casi todo lo reportado ya estaba arreglado en versiones que ese aparato nunca corrió: el reposo que no
entraba por un vencido que la pantalla de turno no iba a atender (1.5.93), la batería medida "desde 1970"
(1.5.93), la red de seguridad del reposo que era código muerto y el log que se truncaba en cada arranque
(1.5.92). **Antes de diagnosticar un log, fecharlo.**

Lo que NO arreglaba actualizar, y por eso va acá:

- **Eran dos relojes y se mantenía uno solo, otra vez.** 1.5.79 arregló el arranque (`applyToSystemClock()` le
  pasa la hora del RTC al reloj del sistema), pero `setFromEpochUtc()` —el camino por el que el servidor pone
  en hora al aparato— escribía el RTC y **no tocaba el reloj del sistema**. O sea que justo después de
  sincronizar, `time(nullptr)` seguía en 1970 hasta el próximo arranque, y `ensureClockForTls()` ya había
  gastado su único intento de la sesión, así que nadie lo volvía a corregir. Ahora los dos caminos que ponen el
  RTC en hora (servidor y NTP) ponen también el del sistema, por `HalClock::applySystemClock()`.
- **"El RTC no tiene una hora creíble" eran TRES cosas distintas con un solo cartel**, y mandan a buscar el
  problema a lugares que no tienen nada que ver: que el chip **no conteste por I2C** es cableado; que conteste
  con la bandera OS puesta es que **el oscilador se paró**, o sea que se quedó sin alimentación, o sea la pila;
  y que conteste con una hora buena pero absurda (2000-01-01 es el arranque de fábrica del PCF85063) es que
  **nunca se lo puso en hora**. Ahora cada uno tiene su renglón. Es el mismo defecto que el "el DNS no devolvió
  nada" contra "resuelve a red interna" de 1.5.92: dos causas con un solo mensaje no son un diagnóstico.
- **El PMIC no puede cargar la pila de respaldo: es una CR2032.** El AXP2101 tiene cargador para esa celda
  (0x18 bit2, apagado de fábrica) y la primera versión de este arreglo lo ENCENDÍA, dando por sentado que había
  algo recargable. Lo confirmó el dueño y es al revés: es primaria. Cargar una primaria la calienta, la hincha y
  la seca antes de tiempo — y una CR2032 seca ES el síntoma. Así que el arranque ahora **comprueba y apaga** ese
  bit, y si lo encontró encendido lo dice en el log: "de fábrica" acá es el OTP de un clon del PMIC y un gestor
  de arranque del vendor, y ninguno de los dos lo decidimos nosotros.

**El diagnóstico de la hora, entonces, se lee en una línea del arranque.** Si dice `el RTC se quedó sin
alimentación (oscilador parado)`, la pila está seca o no hace contacto y no hay software que lo arregle.

## El aparato pintaba dos veces: un bug tapaba al otro (1.5.95)

El usuario reportó que desde 1.5.93 la pantalla "se pinta dos veces" y quedan manchas que antes no
estaban: dos marcos de selección a la vez, renglones de la pestaña Lector encima de los de Sistema,
el "Editar" sobre el "Selecc.", **todo en negro nítido y no en gris**. Lo que lo resolvió no fue
leer código: fue **un número**.

    1.5.90 / 1.5.91          1.5.94
    FAST   582 ms      →      95 ms
    HALF  1793 ms      →     114 ms
    FULL  2191 ms      →     118 ms

El FULL dieciocho veces más rápido, y los tres modos midiendo casi lo mismo. Eso no es una mejora:
es **la onda que no se espera**, y lo que se mide es sólo la escritura del framebuffer. Confirmado
en el aparato en Ajustes → Sistema → Memoria → Panel, y confirmado a mano por el usuario: **tocando
un botón y esperando dos segundos la pantalla queda limpia**. O sea que el panel corre su onda
perfecta —la LUT, la temperatura, los rieles, los dos bancos de RAM, todo bien— y lo único roto es
que nadie la espera. Esa prueba de un minuto descartó cuatro hipótesis de golpe.

**La cadena, y es de manual de por qué un arreglo destapa otro defecto:**

1. `EpdBus::waitRefreshComplete()` toma el camino **por interrupción**, porque este firmware no
   instala el *slice hook* (no hay un solo llamador de `setBusyWaitSliceHook`). Ese camino arma un
   `attachInterrupt(CHANGE)` sobre BUSY y duerme la tarea **20 ms** esperando el flanco con el que
   el panel avisa que arrancó; sin flanco, `detachInterrupt` y `return` **sin esperar nada**.
2. El propio SDK avisa del peligro en el comentario de esa misma función: *"edge interrupts do not
   fire during light sleep, so a completion edge taken while the host is slept would be missed"*.
3. Hasta 1.5.92 eso no se veía **porque el aparato no reposaba nunca**: el usuario tenía un
   recordatorio vencido sin atender, `msUntilNextAlarm()` devuelve 1 ms con algo vencido, y con ese
   tope `IdleSleep::tick()` no entra (`MIN_REST_MS` son 500).
4. En 1.5.93 se arregló eso —`if (cap > 0 && cap < MIN_REST_MS && !alarmWouldRingHere()) cap = 0;`—
   y el aparato empezó a reposar de verdad. **Incluido en el medio de un refresco.**
5. Flanco perdido → vencen los 20 ms → la espera vuelve en el acto → el cuadro siguiente se escribe
   sobre una onda que el panel sigue dibujando.

**Un bug tapaba al otro, y el que quedó a la vista era el viejo.** El arreglo de 1.5.93 era correcto;
lo que faltaba es que **nada en todo el árbol le decía al reposo que había un refresco en curso**
(`grep -rn "isRendering\|renderBusy\|renderInFlight"` no devolvía una línea).

Dos candados, y hacen falta los dos:

- **`gfxPanelRefreshInFlight()`** (`lib/GfxRenderer`): un contador atómico que se levanta alrededor
  de cada llamada al panel —los tres caminos síncronos, el disparo asíncrono y el
  `waitRefreshComplete()` del lector— y que `main.cpp` consulta en la cadena de motivos del reposo.
  Ataca la causa: sin light sleep en el medio, el flanco llega y la espera funciona de verdad.
- **El PISO por modo**: si la espera vuelve antes de lo que la onda puede durar, se espera la
  diferencia. Los valores salen de lo MEDIDO en este panel en 1.5.80 (FAST 582, HALF 1793, FULL
  2191), recortados a 550 / 1700 / 2100 para no alargar jamás una espera sana: con BUSY andando,
  la espera real ya los supera y esto no hace absolutamente nada. Y cuando el piso SÍ entra, lo
  dice en el log (`la espera del panel volvió en N ms … se perdió el flanco de BUSY, van N`), que
  es la única forma de medir sin cable cuán seguido se pierde.

Ninguno de los dos toca una LUT, una secuencia 0x22 ni el registro de temperatura: la regla de no
tocar una onda sin hardware delante sigue intacta, y acá además habría sido el arreglo equivocado.

**Lo que queda anotado**: el camino bueno es instalar el *slice hook* que el SDK ofrece justo para
este anfitrión (deja light-sleepear DURANTE el refresco con despertar por GPIO, y de paso enruta
`waitRefreshComplete()` al camino polleado, que sí tiene ventana de gracia). Eso es más cirugía y
va con el aparato delante; el candado de arriba cuesta como mucho dos segundos de reposo por
refresco y no puede salir mal.

**Y la moraleja de fechar logs**: en esta misma investigación fechamos mal el firmware DOS veces
leyendo el log, porque hasta 1.5.92 el aparato remandaba sus 24 KB enteros en cada sincronización y
el servidor los apendaba: el blob tenía bloques de 1.5.85, 1.5.90 y 1.5.91 mezclados mientras el
aparato ya corría 1.5.94. Antes de deducir de un log, mirar el encabezado de arranque de CADA
bloque — y si hay dudas, vaciarlo desde `/board/log` y sincronizar una vez.

## El piso quedaba corto, y el log no avisaba de nada (1.5.96)

Dos cosas, y la segunda importa más que la primera.

**EL PISO DE 1.5.95 SE QUEDABA CORTO, por dos motivos a la vez y los dos míos.** Lo medido
(582/1793/2191) se cuenta desde ANTES de escribir el framebuffer, y la onda recién arranca DESPUÉS,
con `MASTER_ACTIVATION`: la escritura son unos 90 ms, así que un piso de 550 contado desde el mismo
lugar le deja a la onda 460 cuando necesita ~490. Y encima los había recortado por debajo de lo
medido "para no alargar una espera sana". Recorte sobre recorte: el cuadro siguiente seguía cayendo
sobre la cola de la onda y la mancha seguía. Ahora son **dos números distintos**: uno DETECTA
(550/1700/2100, justo por debajo de lo medido, así que un refresco sano nunca lo cruza y no paga
nada) y otro ESPERA (700/1950/2350, con margen de sobra, y sólo lo paga el refresco que ya venía
roto).

**Y el número que teníamos para diagnosticar estaba mintiendo.** `commit()` recibía el tiempo de
ANTES del piso, así que Ajustes → Memoria → Panel decía "FULL 116 ms" cuando el refresco de verdad
había durado dos segundos — mintiendo justo hacia el lado que hacía parecer que el arreglo no hacía
nada. Ahora se commitea el tiempo real.

**EL LOG TIENE QUE DETECTAR, NO EL USUARIO.** El dueño lo dijo con todas las letras: *"quisiera que
el log detecte estas cosas y no tener que estar diciéndote todo"*, y tenía razón — estaba haciendo
él el trabajo del aparato. El caso que lo disparó: apretó PWR en el hub y **el aparato se reinició**,
y en el log no había una sola línea al respecto. Motivo: la cabecera decía "arranque por boton",
que sale de `wakeReasonName()` — la causa de DESPERTAR del deep sleep. Un pánico, un watchdog o un
brownout quedaban anotados igual que si el usuario lo hubiera prendido a propósito. **Son dos
preguntas distintas y sólo se contestaba una.**

Ahora la cabecera lleva las dos (`arranque por … | reset: …`) con `esp_reset_reason()` en
castellano, y cuando el reset NO es uno de los tres normales —encendido en frío, sueño profundo,
reinicio pedido por software— sale una línea gritada:

    !!! OJO: el aparato NO se apagó solo — se cayó por CAÍDA DE TENSIÓN (brownout). Esto no es normal.

Regla que sale de acá, y vale para todo lo demás: **si el usuario tuvo que contarme un síntoma que
el aparato podía haber detectado solo, el bug no es sólo el síntoma — es también que el log no lo
dijo.** Las dos cosas se arreglan juntas.

**Sin resolver todavía**: por qué PWR reinicia en vez de mostrar la barrita. La línea nueva del
arranque lo va a nombrar la próxima vez que pase; hasta entonces no hay con qué, y no se adivina.
Lo que el dueño quiere de ese botón está escrito y no se negocia: apretar y soltar = suspender; la
barrita sólo carga mientras se mantiene; soltar con la barrita a medias = suspender; **reiniciar,
nunca**.

## La espera del panel, por NIVEL y no por flanco (1.5.97)

**El arreglo de verdad, y es una línea.** `HalDisplay::begin()` instala el *slice hook* que el SDK
ofrece justo para un anfitrión como éste y que nunca habíamos puesto:

```cpp
if (BoardConfig::isWS397()) {
  einkDisplay.setBusyWaitSliceHook([](int8_t, uint8_t) -> bool { return false; });
}
```

Con ese puntero no nulo, `EpdBus::waitRefreshComplete()` abandona el camino **por flanco**
(`attachInterrupt(BUSY, CHANGE)` + 20 ms para verlo; sin flanco, `detachInterrupt` y `return` sin
esperar nada) y pasa al **polleado**: 20 ms de gracia por NIVEL y después `waitBusy()`, que para
ActiveHigh es `while (digitalRead(busy) == HIGH)`. **Un nivel no se puede perder**, se pierda el
flanco por lo que se pierda. Cuesta el ~9 % de energía por refresco que el SDK documenta. Si BUSY
estuviera muerto, cae de largo igual que hoy —no queda peor— y el lazo corta a los 30 s.

**Por qué el piso de 1.5.95/96 no podía alcanzar, y esto es lo que yo no había visto.** El build es
`EINK_DISPLAY_SINGLE_BUFFER_MODE=1`, así que `Ssd1677Driver::displayImpl` hace, **adentro** de
`display.displayBuffer()** y apenas vuelve `refresh()`:

```cpp
if (prev == nullptr && !async) {
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_BW,  fb, _bufferSize);
  writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
}
```

O sea que **el daño ya está hecho antes de que GfxRenderer recupere el control**: cualquier piso
puesto más afuera llega tarde por diseño. El piso queda igual, pero como lo que de verdad es —el
instrumento que mide la falla sin cable, y una segunda línea de defensa para no encimar el cuadro
siguiente—, no como el arreglo.

**Y por qué la mancha es negra y nítida y son exactamente DOS cuadros**: el FAST sale diferencial
contra RED (`CTRL1_NORMAL`) y el HALF y el FULL salen absolutos (`CTRL1_BYPASS_RED`). La tinta que
quedó "coincide" con lo que dice RED, así que **no se vuelve a manejar nunca** hasta que cae una
limpieza. Una sola onda perdida envenena el vidrio hasta el próximo HALF.

**LA CORRECCIÓN QUE ME DEBO, y es la tercera de esta sesión.** En 1.5.95 dije con todas las letras
que la causa era mía, de la línea de 1.5.93 que destapó el reposo. **Ese mecanismo es real y está
cerrado, pero NO explica el log del usuario**, y dos verificaciones independientes lo mostraron con
aritmética: en ese log hay trece refrescos en 28,3 s, uno cada 2,2 s sostenido — el aparato EN USO.
El ocio nunca llega a los 30 s de `REST_AFTER_MS`, así que el reposo no entró ni una vez, y sin
embargo los refrescos salen todos cortos. Quedan dos candidatos y **el árbol no alcanza para
decidir**: (A) el flanco se lo come el light sleep, o (B) BUSY levanta más tarde que los 20 ms de la
ventana, despierto, en todos los refrescos. El hook arregla los dos. La línea del log los separa: si
aparece pegada a un `[REST] a reposar…` era A; si aparece sin reposo cerca, era B.
Y del diff de 1.5.91 a 1.5.94 no sale nada que toque GPIO, interrupciones, frecuencia de CPU ni
SPI, y el submódulo no se movió: **puede no haber sido un commit**.

**Dos huecos del candado de 1.5.95, encontrados en la misma revisión y cerrados**: `IdleSleep::tick()`
ahora repregunta `gfxPanelRefreshInFlight()` **pegado** a `esp_light_sleep_start()` (entre la
consulta de `main.cpp` y el sueño pasan varios ms, con dos lecturas I²C en el medio), y
`displayGrayBuffer()` —la onda de gris del lector, 366 ms— había quedado sin candado y sin piso.

**El piso ahora mira el pin, no el reloj.** Un número fijo es frágil en las dos direcciones: corto
deja pasar la cola, largo le cobra a un refresco sano. Se pollea BUSY hasta que baje, con el tope
por modo como cordura y 3 s de tope duro.

## El aparato se colgaba con un toque de PWR, y lo detectó el log solo (1.5.98)

**La línea nueva de 1.5.96 contestó en el primer intento, que era exactamente para lo que se puso:**

    === 1.5.97-ws397 | arranque por encendido | reset: WATCHDOG de interrupciones ===
    !!! OJO: el aparato NO se apagó solo — se cayó por WATCHDOG de interrupciones. Esto no es normal.

Siete arranques seguidos así, hasta que el dueño le sacó la batería. Un toque de PWR y el aparato
entraba en bucle de reinicios. **Y la causa era mía, de 1.5.97.**

`armWakeSources()` arma los botones con `gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL)`: una
interrupción **POR NIVEL**, no por flanco. Mientras el pin siga en bajo esa interrupción se vuelve a
disparar sola, para siempre. Normalmente no importa, porque `esp_light_sleep_start()` la consume y
al volver se desarma todo (el camino normal desarma **las dos** fuentes, timer Y GPIO).

Pero en 1.5.97 metí la guardia del panel (`gfxPanelRefreshInFlight()`) **pegada al sueño, después de
armar**, y su `return` desarmaba **sólo el timer**. O sea: pines armados con una interrupción por
nivel, sin nadie que la atienda y sin sueño que la consuma. Apretar un botón dejaba a la CPU sin
salir del vector de interrupción hasta el watchdog — y como al reiniciar pasaba lo mismo, bucle del
que sólo se sale sacando la batería.

**El arreglo es de ubicación, no de lógica**: la pregunta va ARRIBA DE TODO, junto a `blocked` y al
`idleMs < REST_AFTER_MS`, antes de armar nada. Así no hay nada que desarmar. Y además existe ahora
`disarmWakeSources()` —que apaga pin por pin y las dos fuentes— y la usan **todas** las salidas,
incluido el camino de fallo de `armWakeSources()`, que tenía el mismo agujero desde siempre.

**La regla, que es la que faltaba escrita**: en `IdleSleep::tick()`, todo `return` posterior a
`armWakeSources()` **tiene que desarmar**. Armar sin dormir no es un desperdicio, es un cuelgue.

**Y la moraleja del log**: esto es lo que el dueño había pedido dos versiones antes — *"quisiera que
el log detecte estas cosas y no tener que estar diciéndote todo"*. La cabecera con
`esp_reset_reason()` convirtió "se reinicia y se traba, no sé por qué" en una causa con nombre, a la
primera, sin cable y sin que él tuviera que reproducir nada. **Cuando un síntoma cuesta tres vueltas
de adivinanzas, el arreglo que hay que hacer primero es el que lo vuelve visible.**

## PWR colgaba el aparato desde el primer reposo, y 1.5.98 era la mitad (1.5.99)

Después de 1.5.98 el log siguió llenándose de `reset: WATCHDOG de interrupciones` y el dueño lo resumió:
*"pésimo comportamiento de los botones, mismo error, todo mal"*. Esta vez no se adivinó: se desensambló.

**La causa, verificada en el binario de IDF (`libesp_driver_gpio.a`)**: `gpio_wakeup_enable(pin, LOW_LEVEL)`
—lo que arma cada pin de despertar del reposo— **escribe el TIPO de interrupción del pin** (bits 7-9 de
`GPIO_PINn_REG`, `and 0xfffffc7f` / `or tipo<<7`), y `gpio_wakeup_disable` **sólo apaga el bit de despertar**
(`and 0xfffffbff`): el tipo queda en NIVEL para siempre. A los cuatro botones no les importa, no tienen ISR.
Pero **GPIO38 es la IRQ del PMIC y tiene la ISR de flanco de `PowerKey` con la interrupción habilitada**
(`attachInterrupt(FALLING)`). O sea que desde el PRIMER reposo, cada pulsación de PWR —y el propio despertar
por PWR— era una interrupción por nivel entrando sin parar: la ISR no puede levantar la línea (eso es I2C
desde el loop, que no llega nunca) y a los 300 ms salta el watchdog. Existía desde 1.5.93, que es cuando el
reposo empezó a entrar de verdad: **el dueño tenía razón con "antes de la 93 esto no pasaba"**, y el
arreglo de 1.5.93 estaba bien — destapó esto, igual que destapó lo del panel.

Lo de 1.5.98 (armar sin dormir) era cierto pero era la mitad: cerraba el caso "sin reposo" y dejaba abierto
el caso "después del reposo", que es el de todos los días.

- `PowerKey::pauseIrq()` (detach) **antes** de armar y `resumeIrq()` (attach, que restaura el flanco) en
  `disarmWakeSources()`, o sea en TODA salida del reposo. Además `armWakeSources()` **se niega a armar por nivel
  cualquier pin con `int_ena != 0`**, sea de quien sea, y lo dice: la próxima ISR sobre un botón no repite esto.
- **Dos redes de seguridad que se anotan en el log**: la propia ISR, si se encuentra el pin por nivel, lo pasa a
  flanco en el acto y cuenta (`GPIO38 estaba armado POR NIVEL con la ISR de PWR enganchada (van N)`), y
  `pump()` hace la misma comprobación desde el loop. Si alguna vez sale esa línea, alguien armó GPIO38 sin
  `pauseIrq()`.

**Y por qué el log no lo decía: el final del log no sobrevivía al cuelgue.** Se bajaba a la tarjeta cada 2 KB y
nada más, así que el watchdog se llevaba hasta 2 KB de líneas. "La última línea antes del reinicio" era la
última que había llegado a la tarjeta, con decenas de segundos y un reposo entero en el medio — por eso los
crashes parecían pegados a `Entering activity: Voice` o a un golpe del IMU y no a PWR, y por eso las siete
cabeceras seguidas de 1.5.97 no tenían nada adentro. Ahora `devlog::tick()` baja lo pendiente a los 250 ms de
quietud, cada 1,5 s si no para de escribir, y en el acto si la línea es `[ERR]`. **Antes de leer "qué pasó justo
antes" en un log, saber cuánto del final se perdió.**

**La regla de PWR, ahora sí como la escribió el dueño en 1.5.96**: apretar y soltar = suspender, dure lo que
dure; la barrita (desde 1,2 s) sólo dice cuánto falta para apagar; soltar con ella a medias suspende igual;
3 s = apagado. Hasta 1.5.98 un toque de menos de 1,2 s "no hacía nada", regla de 1.5.59 de cuando el toque
abría un menú que ya no existe. Se ignoran la pulsación que lo encendió (`BOOT_KEY_IGNORE_MS`) y la que viene con
ABAJO (captura de pantalla), y las dos lo dicen en el log.

**Sin explicar y anotado**: en un arranque de 1.5.98 hay un `refresh FAST hint=ui 30043ms` mientras se
conectaba el WiFi: BUSY quedó en alto 30 s (el tope de `waitBusy`). Una vez, sin cable. Si vuelve, mirar si
coincide con el WiFi levantando.

## Lo que quedó guardado mientras el loop estaba ocupado (1.5.100)

1.5.99 cerró el watchdog (cero `WATCHDOG` en su log, y el toque de PWR suspende), y el mismo log mostró
otra cosa: *"cuando le pregunto algo se queda pensando un montón de tiempo, cuando sale de eso, se traba"*.

- **El POST de Hablar tardó 90 s dos veces de tres**: `retry 1 after status -1` a los 91 s (el tope), y el
  reintento salió en 5 y en 30 s con el servidor tardando 1,4-2,6 s (`stt= llm= tts=`). El mismo día, en
  1.5.98, tres POST de hasta 158 KB salieron en 3-6 s. 1.5.99 no toca la red, y la fusión a `ws397` fue dos
  horas antes (no es el redeploy). **No se sabe si es el enlace o Railway**, y el log tampoco lo decía:
  ahora cada intento que falla o pasa de 15 s deja `intento N FALLÓ tras N ms (status, bytes, wifi, rssi,
  heap)`. Con eso, la próxima vez se distingue.
- **"Se traba" era la pulsación de PWR guardada.** El dueño apretó PWR durante los 90 s para destrabarlo; el
  PMIC guardó press+release+LONG+SHORT (`sts2=0F release held=1000 ms long`) y, cuando el loop volvió, la regla
  nueva de 1.5.99 hizo lo suyo: `PWR soltado a los 1000 ms: se suspende`, justo con la respuesta en pantalla.
  Un flanco de hace más de 5 s (`STALE_EDGE_MS`) ya no emite ni suelta ni toque; se decodifica y se dice.
- **"El doble toque quedó sensible de más" es lo mismo con el IMU**: el motor del chip deja el golpe latcheado
  en STATUS1 hasta que alguien lo lea, y la primera lectura después del POST traía el golpe dado al aparato
  "colgado" (`[168282] doble golpe` pegado a la respuesta) → Hablar abierto solo. Con más de 2 s sin sondear
  (`STALE_GAP_MS`) lo que traiga la primera lectura se descarta, y el log lo dice.

Regla: **lo que se latchea mientras el loop no corre es del pasado, no una orden.** Vale para el PMIC, para el
IMU y para cualquier otro periférico con estado pegajoso que se lea por sondeo.

## El router, el ahorro del WiFi y lo que quedaba de la tanda (1.5.101)

- **Las conexiones mudas coinciden con el ROUTER, no con el firmware.** Desde que el aparato pasó de
  "Esthetique" (BSSID `50:46:4a:…`, fallida a las 22:18) a "INFINITUM6902_2.4" (`c2:68:cc:…`), una de cada dos
  conexiones nuevas al servidor se quedaba muda hasta el tope: `POST /api/voice` 90 s, `GET /api/hub` 20 s,
  `GET /api/news/pack` 28 s, y el reintento salía en 3 s. Desde acá Railway contesta en 0,2-0,5 s doce veces
  seguidas. Lo que distingue a la OTA —5,7 MB por la misma red sin un solo traspié— es que pone
  `WIFI_PS_NONE`: el modem sleep del ESP32 con ciertos routers domésticos pierde paquetes hasta que TCP se
  rinde. `ServerClient::request()` apaga el modem sleep mientras dura la petición (como ya hacía
  `SpeechToText`) y lo devuelve al terminar. El tope de Hablar bajó de 90 a 40 s: el servidor tarda 1,5-5 s.
  La transferencia de archivos por esa misma red "se conecta pero tarda horrores": ahí el aparato es el
  servidor y ya tenía `setSleep(false)`; el log dice ahora la dirección con máscara y puerta de enlace, y
  cada vez que alguien llega a la página. Si con eso sigue lento, es el router.
- **Apoyar el aparato era un doble golpe.** Cada Hablar abierto solo tenía un evento de posición (inclinar,
  horizontal, boca arriba) 100-650 ms antes; los dobles a propósito llegan con el aparato ya quieto en la mano.
  `TAP_AFTER_MOVE_MS` (700): un golpe dentro de ese plazo después de un evento de posición no es un gesto y el
  log lo dice (`recién movido: se ignora`).
- **El doble Atrás tenía un rebote**: `doble Atrás (104 ms)` 170 ms después de un Atrás mantenido. Nadie toca dos
  veces en un décimo de segundo. Dos sueltas a menos de 150 ms son una, y medio segundo de cuarentena después
  de un mantenido.
- Noticias: la hora salía en UTC porque `getHours()` corre en Railway. Ahora va en la zona del lugar del clima de
  la cuenta (`hub-settings.timezone`) o `HUB_TZ`. Los paquetes ya armados se corrigen en la pasada de cada hora.
- Ajustes → Memoria: "quedan N h" en dos renglones; en uno, con el metadato a la derecha, se cortaba justo lo
  único que se viene a leer. Biblia: se fue el "En la tarjeta" de cada capítulo ("es irrelevante").

## La barrita de PWR y el "Apagando" encimados (1.5.102)

`drawPowerHoldBanner()` calculaba el alto del cuadro según el texto: "Suelta para suspender · 3 s para apagar"
son dos renglones y "Apagando..." uno, así que a los 2,3 s el cuadro nuevo salía más chico y más abajo, y como
el framebuffer conserva lo pintado, el borde y el texto del grande quedaban asomando alrededor del chico. El
cuadro reserva siempre dos renglones y el texto de uno se centra en el hueco: el segundo cuadro tapa
exactamente al primero.

## "Busca" es la única llave de internet, y la pregunta tardaba por otra cosa (1.5.103)

**REGLA DEL DUEÑO, reafirmada**: el servidor busca en internet **sólo si el usuario lo dice** ("busca…",
"fijate en internet"), en Hablar y en Preguntarle al libro. La había pedido en 1.5.41 y la volvió a pedir
después de 1.5.102, cuando por una tarde se cambió a "cuando haga falta": *"quiero que siga igual, solo cuando le
digo busca, que busque; esto tiene que estar también en el helper"*. No cambiarlo sin preguntar. La casilla de
`/board` decía "Buscar cuando haga falta" y ahora dice lo que hace. Lo que sí quedó del intento: la línea
`voice ms:` del servidor lleva `intent=` y `web=no|si|sin resultados|FALLO|apagada`, así "no buscó" y "buscó y
no encontró" dejan de ser el mismo síntoma.

**Y la pregunta que "tarda un montón, así sea la distancia a la luna" no era el modelo.** Los logs de 1.5.99 y
1.5.100 dicen lo mismo en cada pregunta: toma de 4-8 s, **4-5 s de WiFi DESPUÉS de terminar de hablar**
(`Take` → `upload`), y el servidor en 3-4 s — o **90 s** cuando la conexión se quedaba muda y el reintento
salía en 3,5 s. O sea 15 s en el mejor caso y un minuto y medio en el peor. Dos cosas, las dos en el aparato:

- **El WiFi se conecta MIENTRAS se habla.** `VoiceActivity::startRecording()` arranca `FriendlyWifi` antes de
  abrir el micrófono y le da cuerda cada 100 ms desde el loop de `RECORDING`; al terminar la toma, si ya está
  conectado, se manda en el acto. Estaba así desde 1.5.39 (grabar, DESPUÉS conectar), no era una regresión: era
  el diseño. Sin red guardada, el selector espera a que termine la toma, como siempre. Grabar con la radio
  encendida ya pasaba en la segunda vuelta (la hora del recordatorio) y en la Biblia, y transcribía bien.
  Y si se sale durante la toma (Atrás, toma demasiado corta) **no hay reinicio silencioso**: sin un POST no
  hubo TLS, no hay heap que recuperar, se apaga la radio y listo (`requestMade`).
- **Una conexión muda ya no se come el tope entero.** `SecureClient::setKeepAlive()` (parche 0025 del SDK,
  apagado por defecto; `ServerClient` lo enciende en 5 + 3·3 s): el router que deja de entregar la bajada
  dejaba la petición esperando 40-90 s sobre una conexión que el servidor **ya había contestado** (los
  `29776 bytes subidos` del log dicen que la subida fue confirmada). Con keepalive el socket se cae a los ~14 s
  y el reintento sale por una conexión nueva. Un servidor lento pero vivo contesta las sondas: no dispara.
  OJO: `performRequest()` ya ponía `WiFi.setSleep(false)` ANTES de 1.5.101 y el POST de Hablar se quedaba mudo
  igual, así que el modem sleep **no explica** ese caso; el keepalive lo acota sea cual sea la causa.
- **El aparato dice sus tiempos**: `tiempos: toma N ms, WiFi +N ms tras la toma, ida y vuelta N ms` (con
  `— LENTO` pasados los 15 s), al lado de la línea del servidor (`stt= llm= tts=`). Con esas dos líneas la
  próxima queja de "tarda" se lee sin deducir nada de los sellos.

## Las apps de Lua salen a la red, y la primera es el Librito (1.5.104)

Plan y contrato en **`docs/ws397/PLAN_APPS_VIAJES_EPUB.md`** (decisiones del dueño incluidas). Lo que entró:

- **Cinco puertas nuevas en `cp`** (`docs/ws397/APPS_LUA.md`, sección "Las puertas"): `listen` (micrófono →
  texto), `call` (servicio con nombre del servidor), `download` (archivo generado → carpeta de la app o
  `/Books/<app>/`), archivos propios (`files/read/write/remove/size`, sólo `/Apps/data/<app>/`), `view`
  (el visor paginado del sistema) y `open_book` (el lector). **Todo lo que espera es asíncrono**: la app
  encola y el resultado llega por `on_heard(texto)` / `on_reply(id, ok, tabla)`. Lo de red y micrófono lo
  hace `LuaAppsActivity` desde su loop (fases Listening/Connecting/Transcribing/Calling/Downloading/Viewing),
  **nunca el worker de Lua** (32 KB de stack; TLS no entra). WiFi arriba hasta cerrar la app, sin reinicio
  silencioso salvo `open_book` después de TLS (mismo camino que Preguntarle al libro). Con el micrófono
  abierto `on_tick` no corre (cada tick es una tarea de 32 KB al lado del DMA).
- **Harness de escritorio con `cp` falso completo** (`test/lua_sandbox/test_sandbox.cpp`, tabla `fake`:
  `heard`, `reply[servicio]`, `download`, `key`, `tick`, `step` —una cosa por vez—, `draw`, `advance/ms`,
  `opened`, `reload`). Descubre `examples/Apps/*.lua` solo y corre `test/lua_sandbox/scenarios/<app>.lua`
  si existe. **Una app nueva no está terminada sin su escenario.**
- **Servidor `/api/apps/*`** (`server/src/apps.ts`, `appsJobs.ts`, `appsLlm.ts`): `POST /api/apps/call`
  `{app, service, args}` siempre 200 con `{ok, …}`; servicios síncronos en < 25 s; lo largo son **trabajos**
  (`job.status`, `/data/apps-jobs.json`, archivos en `/data/apps-files/`, poda a 24 h) y `GET /api/apps/file/:id`.
  **Cliente de Anthropic aparte** con `config.apps = {key, model}` (`/board` → Ajustes → Avanzado → Apps de
  Lua; `claude-opus-5` u `claude-sonnet-5`; pensamiento adaptativo; streaming para la prosa). Se cuenta en
  `usage` como `apps_calls`, **fuera** del tope de Hablar (a propósito: `job.status` cada 5 s contaría como
  llamada al modelo). Sin clave: `{ok:false, error:"Carga la clave de las apps en la web…"}`.
- **Librito** (`examples/Apps/librito.lua`, `server/src/librito.ts`, `epub.ts`): dicta un tema → tres enfoques
  → índice de 5-8 capítulos que el usuario **edita** (OK desactiva, "Agregar o cambiar por voz", "Más
  temas") → trabajo que escribe capítulo por capítulo (índice cacheado con `cache_control`, resumen de lo ya
  escrito, línea `RESUMEN:` recortada) y arma el EPUB a mano con `fflate` (`mimetype` primero y sin comprimir;
  prueba en `./test/epub/run.sh`) → `/Books/librito/<slug>.epub` → "Abrir en el lector". **Mínimo 15 minutos**
  (200 palabras/min): si se sacan capítulos, el resto se alarga. El `jobId` se guarda con `cp.save` y al
  volver a entrar ofrece "Retomar".
- **Y el pedo de la 1.5.103, encontrado de paso**: la etiqueta nueva de la casilla de búsqueda llevaba
  comillas sin escapar dentro de un string de `app.js` → `SyntaxError` → **`/board` no cargaba** desde el
  despliegue de 1.5.103 hasta este arreglo. `node --check server/public/board/app.js` lo habría dicho; ahora
  es parte del chequeo de todo cambio en la web.

Pendiente de hardware: todo lo de red y micrófono desde una app (la lógica se probó de escritorio con el
escenario entero del Librito), y una escritura real con la clave cargada.

## "Se trabó" con una pregunta con búsqueda, la red sola al despertar y el Atrás raro (1.5.105)

Tres quejas del dueño sobre 1.5.104, y el log que mandó era de **1.5.98** (19:36 del 17, antes de 1.5.99 de las
20:35): los siete `WATCHDOG de interrupciones` y el `doble Atrás (104 ms)` que hay ahí son los bugs que cerraron
1.5.99 y 1.5.101. Fechar el log primero, siempre. Lo que sí seguía vigente en el árbol:

- **"Se trabó luego de que le pregunté algo que tenía que buscar."** Dos causas, las dos reales:
  1. El tope de Hablar eran **40 s** (1.5.101, por la conexión muda) y una pregunta con búsqueda en internet son
     20-60 s de modelo (el servidor le da 90 s). El aparato vencía, y `ServerClient` **reintentaba dos veces
     más**: el servidor repetía la búsqueda entera y el dueño miraba "Pensando" dos minutos con el aparato sordo.
     Ahora el tope vuelve a **90 s** —la conexión muda la corta el keepalive de 1.5.103 a los ~14 s, así que el
     tope ya no la protegía de nada— y **un -1 que llega al cumplirse el tope no se reintenta**: es un servidor
     vivo que sigue trabajando, no una conexión caída (ésa muere antes del tope y sí se reintenta).
  2. **La sincronización oportunista se metía adentro de Hablar.** Con la respuesta en pantalla o en "¿otra
     pregunta?" la radio sigue arriba y `preventAutoSleep()` ya es false, así que a los 3 s de quietud
     `devicesync::ifDue` bloqueaba el loop con la cola, las noticias y el hub. El dueño tocaba Atrás justo ahí
     y no pasaba nada: "la pantalla de si tienes otra pregunta no vuelve al hub". `Activity::allowsBackgroundSync()`
     (Hablar devuelve false) y `ActivityManager::allowsBackgroundSync()` mira **toda la pila**, porque el visor de
     la respuesta va encima de Hablar y el que sabe que hay una conversación abierta es el de abajo.
- **"Luego de una suspensión quería conectarse al wifi a huevo; eso solo cuando yo lo requiero, no en auto."**
  Decisión del dueño, dos caminos cerrados:
  1. **El hub ya no sincroniza solo al entrar.** Lo hacía con caché de más de 3 h —o sea en cada despertar de una
     noche— y reintentaba a la hora si el clima venía vacío. Queda sólo la vuelta de una OTA (paquete de contenido
     pendiente, una vez). Sincroniza quien lo pide: Atrás mantenido en el hub, Ajustes → Sincronizar hub, y la
     oportunista cuando la red ya está arriba por otra cosa. **Consecuencia**: el clima, los recordatorios cargados
     desde `/board` y la hora del RTC se refrescan cuando el dueño sincroniza o usa algo con red, no antes.
  2. **La tecla que despierta del reposo ya no es un gesto largo.** En el log: `despertó por pin`, Atrás seguía
     abajo porque la pantalla no reaccionaba, y a los 1,2 s el hub lo leyó como "Atrás mantenido = sincronizar" y
     levantó la red. `MappedInputManager::ignoreHeldLongPress()` marca sólo la pulsación larga como ya disparada;
     **la suelta sigue siendo un toque**, porque casi toda vuelta de página en el lector viene del reposo (leer una
     página tarda más que los 30 s de `REST_AFTER_MS`) y tragarse ese toque obligaría a apretar dos veces.
     Se aplica en la pasada siguiente a `Woke::Button` (`restWakeHeldPending`), que es cuando la tecla se lee.
- **"Comportamiento raro en el botón de atrás."** Cancelar Hablar con Atrás abría la ventana del atajo de voz,
  y el toque siguiente —"¿por qué no volvió?", toco otra vez— abría Hablar de nuevo. `checkVoiceShortcut` corre
  ANTES del loop de la Activity y Hablar sale con el flanco de bajada, así que se anota **en qué pantalla se
  apretó**: si no era una tranquila (Hablar, Noticias, la Biblia, una app de Lua), ese Atrás ya hizo lo suyo y no
  abre ninguna ventana. El rebote de 104 ms del log es el de 1.5.101 y ya estaba cerrado.

**Sin probar en hardware**: los cuatro. Lo que hay que ver es que una pregunta con "busca" conteste (el log dirá
`ida y vuelta N ms` sin `intento 2`), que después de la respuesta Atrás vuelva al hub en el acto, que al
despertar de una noche el hub NO levante la red, y que apretar Atrás para despertar y mantenerlo no sincronice.

## "Anoche se tragó el 9 % suspendido" (1.5.106)

Del log: 04:18 `antes de dormir: 82 % · 3975 mV` → 15:45 `bateria 73 %` (3908 mV). Once horas y media de sueño
profundo a **0,8 %/h**, que en una batería de 1500 mAh son unos **10-12 mA**. El S3 dormido son microamperios,
así que eso no es el ESP: es lo que queda prendido alrededor. La noche anterior (23:29 → 02:38, 86 → 86 %) parecía
gratis, pero por tensión (4020 → 3987 mV) fue ~1 %: el medidor del PMIC redondea y se queda.

**El mapa de rieles, sacado del esquemático oficial** (`files.waveshare.com/wiki/ESP32-S3-ePaper-3.97/ESP32-S3_e-Paper-3.97-schematic.pdf`,
una sola página): **DC1 = VCC3V3** (ESP32, tarjeta SD, SHTC3, QMI8658, pull-ups; no se puede cortar), **RTCLDO = VRTC**
(PCF85063, con la pila de respaldo; independiente), y **ALDO1-3 = EPD_VCC_AXP, Audio_VCC y AudioCTR_VCC** (en un
orden que el esquemático no deja leer): el panel (a través del P-MOSFET Q2), el ES8311 con el micrófono, y la zona
del NS4150B. ALDO4, BLDO y DLDO sin uso. El firmware NO toca los rieles (regla de siempre), así que los tres ALDO
quedan a 3,3 V toda la noche.

Con eso, dos cosas que dejaban chips **encendidos de verdad**, arregladas sin tocar el PMIC:

- **El ES8311 nunca se apagaba.** `AudioManager::powerDown()` sólo corta un riel por GPIO que en esta placa es
  `PIN_UNASSIGNED`, así que el códec pasaba la noche polarizado como lo dejó la última reproducción. Ahora
  `codecsleep::es8311Suspend()` (`src/util/CodecSleep.h`, la secuencia `es8311_suspend` de esp-adf) va en
  `sleepNow()`; al despertar el aparato se reinicia y `codecInit()` lo resetea.
- **El enable del amplificador quedaba al aire.** GPIO39 (PA_CTRL del NS4150B) no es RTC GPIO y en la placa **no
  tiene resistencia a masa** (R74 sin poblar). `silenceAmp()` lo pone en LOW, pero `PowerManager::deepSleep()`
  llama a `esp_sleep_config_gpio_isolate()` y suelta el pad: un clase D con el enable flotando puede quedar
  encendido con su corriente de reposo. Ahora se **retiene** (`gpio_hold_en`, el SDK ya hace
  `gpio_deep_sleep_hold_en`) y `setup()` lo libera antes de que el audio lo maneje; sin liberar, el parlante
  quedaría mudo hasta el próximo corte de energía.

Y dos cosas para que **el log lo mida solo** (regla del dueño: el aparato detecta, no él):

- Al despertar de un sueño profundo, `batterylog::reportAfterSleep()` compara la línea de "antes de dormir" con la
  lectura de ahora: `dormido 11.4 h: 82 -> 73 % (0.79 %/h, 3975 -> 3908 mV)`, y **arriba de 0,3 %/h** sale como
  `[ERR]` con "DEMASIADO para un sueño profundo: algo quedó encendido".
- El volcado del PMIC al arrancar suma los rieles: `AXP2101 rieles: DCDC(80)= LDO(90)= ALDO1-4= …` (0x1C = 3,3 V).
  Con eso se ve qué está prendido sin abrir el aparato.

**Y la palanca grande, hecha en 1.5.107 a pedido del dueño ("hacé lo de los rieles a ver qué onda"):**
`PowerKey::railsOffForSleep()` apaga ALDO1-3 (bits 0-2 de 0x90) como ÚLTIMO paso de `sleepNow()`, con el panel ya
en su deep sleep y el log cerrado; `PowerKey::begin()` los vuelve a encender **lo primero** al arrancar (pone 3,3 V
en 0x92-0x94 y los bits en 0x90, 20 ms de espera), y `begin()` corre antes de `setupDisplayAndFonts()` en los dos
caminos del setup. Un arranque sin sueño de por medio los encuentra prendidos y no escribe nada. Lo que queda
consumiendo dormido es sólo lo que cuelga de VCC3V3 (la tarjeta en reposo) y el PMIC mismo.
- **Vía de rescate**: si alguna vez despierta con el panel a oscuras y sin audio, es que el reencendido no llegó:
  **PWR 10 s** (corte duro del PMIC, que vuelve a los valores de fábrica) o sacar la batería.
- **Ojo con el códec sin riel**: su I2C es el bus compartido, con pull-ups de 4,7 K a VCC3V3 (R22/R28). Con
  Audio_VCC apagado, esas líneas alimentan al ES8311 por sus diodos de protección (unos cientos de µA). Si el
  número de "dormido" no baja lo esperado, la alternativa es dejar Audio_VCC encendido con el códec en suspensión
  y cortar sólo los otros dos; para eso hace falta saber cuál ALDO es cuál, y se puede averiguar en caliente
  (cortar uno y ver si el ES8311 deja de contestar en 0x18).
- La línea del arranque lo dice: `rieles ALDO1-3 re-encendidos al arrancar (estaban apagados: 07)`. Si no aparece
  después de un sueño profundo, el corte no se hizo.

## "Se trabó por completo": el panel no contestaba y el SDK esperaba 30 s por cada cosa (1.5.108)

Con 1.5.105 el aparato "se trabó por completo, no respondía ni a la palanca, a los años se conectó a la red".
Sacarle la batería no lo arregló. **No estaba colgado**: el log lo dice, y hay que saber leerlo.

- A las 15:56, primer refresco después de diez minutos de reposo: `refresh FULL hint=ui 30086ms`. Y después de
  los arranques en frío (con el cable puesto, `vbus=1`), el init de la pantalla tarda **90 s** y cada refresco
  **30 s**, siempre el mismo número redondo. Ése número es el tope del SDK: `EpdBus::waitBusy` esperaba hasta
  30 s a que BUSY del SSD1677 bajara, tres veces en el init y una por pintada. **BUSY quedó en alto** y el
  aparato pagaba el tope entero por cada comando: cada toque de la palanca "no hacía nada" porque la pintada
  anterior seguía esperando.
- **Por qué sacar la batería no lo arregló**: el USB estaba puesto y el PMIC no se resetea con el ESP, así que
  el riel del panel (ALDO, vía el P-MOSFET Q2) nunca se cortó. Un controlador trabado no sale de ahí con RST
  ni con un reinicio del ESP: hace falta un corte de corriente de verdad. Y 1.5.105 no toca nada del panel ni
  del PMIC (1.5.106/107 nunca corrieron en ese aparato): no fue un commit, fue el chip.
- **Parche 0026 del SDK**: `EpdBus::setBusyTimeoutMs()` y un contador `busyTimeouts()`. `HalDisplay` pone
  **5 s** en la ws397 (la onda más larga medida es el FULL, 2,2 s; hay margen para el frío). Con eso un panel
  mudo cuesta 5 s por pintada en vez de 30 y se llega a Ajustes y a la OTA.
- **El aparato se rescata solo, una vez por encendido** (`checkPanelAfterInit` en `main.cpp`): si el init
  venció alguna espera, `[ERR] EL PANEL NO CONTESTA`, `PowerKey::railsCycle(500)` (ALDO1-3 abajo medio
  segundo, que se lleva también códec y amplificador, y arriba) y reinicio limpio. `panelRescueMagic` en
  RTC_NOINIT evita el bucle: si al volver sigue mudo, arranca igual y el log dice qué probar (PWR 10 s, o
  batería **y** cable). En uso, `checkPanelHealth()` en el loop anota cada espera vencida en el acto y a la
  tercera de la sesión hace el mismo ciclo con reinicio silencioso al hub.
- **Lo que hay que ver en el log del aparato afectado** cuando tome 1.5.108: si `el panel volvió a contestar
  después del ciclo de corriente` aparece, era el controlador trabado y ya está; si aparece `ya se le dio un
  ciclo de corriente y sigue mudo`, mirar la línea `AXP2101 rieles:` (LDO(90) sin los bits 0-2 = riel apagado)
  y, si el riel está bien, es hardware: el panel o su cable plano.

## Hablar sin segunda pantalla, palabras de orden y lo que sobraba (1.5.109)

Pedidos del dueño sobre 1.5.108, todos hechos:

- **Se fue la pantalla "¿quieres preguntar algo más?"**: respuesta, Atrás, y al hub (o a la pantalla que abrió
  Hablar). **El contexto vive en el servidor, por cuenta, 24 h** (`voice-context` en `fsjson`, últimos 8 turnos
  de PREGUNTAS; sobrevive al redeploy). Antes era un `Map` en memoria atado a un `conversationId` que sólo
  servía mientras el aparato se quedaba en esa pantalla. El `conversation=` de los aparatos viejos se ignora.
- **Palabras de orden estrictas** (`CMD` en `voice.ts`, una tabla por idioma que viaja en el system prompt):
  `recuérdame` → recordatorio, `memoriza` → memoria, `busca` → internet, `compra` → compras, `tarea` → tarea,
  `nota` → nota, `pon N minutos` → temporizador, `alarma` → alarma, `traduce` → traducción; sin palabra de orden
  es pregunta. **Un recuérdame NUNCA es memoria**, y hay guardia del servidor además del prompt: una acción
  `memory` sin la raíz "memoriz" en lo dicho (`saysMemorize`) se descarta y se anota en el log. La pantalla de
  Hablar muestra los ejemplos con esas palabras (`STR_VOICE_SAY_*`, nueva fila Buscar).
- **Respuesta vacía del servidor**: el log del aparato ahora dice qué vino (`respuesta vacía: text=… saved=…`), y
  el servidor ya no manda una reply vacía: "Listo, guardado." si guardó algo, "No entendí" si no.
- **Las sugerencias de Mi día se fueron** (eran de los viajes): `suggest.ts`, `/api/suggest`, el documento
  `suggest`, 8 claves × 7 idiomas y todo el código en `CalendarActivity`. En Hoy, **OK dicta el día** (antes OK
  pedía sugerencias y dictar sólo se podía con Atrás mantenido).
- **Apps de Lua con nombre de verdad**: la lista y el cabezal muestran el título y la descripción del comentario
  que abre el archivo (`-- Reloj: la hora grande, la fecha debajo.`, `LuaApp::titleFromHeader`), y nunca la
  ruta. Sin comentario, el nombre del archivo con mayúscula. Documentado en `docs/ws397/APPS_LUA.md`.
- **Pedo mío en los rieles, visto en el log de 1.5.108**: `ALDO1-4=1C 1C 19 0D`, o sea que **ALDO3 va a 3,0 V de
  fábrica** (0x19), no a 3,3, y ALDO4 a 1,8. `PowerKey::begin()` de 1.5.107 escribía 0x1C a los tres al volver
  del sueño: le habría subido 0,3 V a un chip que no sabemos cuál es. Ahora **sólo se tocan los bits de
  encendido** de 0x90; las tensiones quedan como las dejó la fábrica (el PMIC no se resetea con el ESP).
- **Sin red**: no se traba. `FriendlyWifi` prueba las guardadas (9 s cada una, cinco como mucho) y escanea; si
  ninguna está, cae al selector de redes ("No hay redes disponibles" o la lista con la clave por el teléfono) y
  Atrás ahí muestra "No se pudo conectar al WiFi" y vuelve. Sin ninguna red guardada, directo al selector.

## El Librito murió en la línea 53, y la línea 53 no tenía la culpa (1.5.110)

`librito:53: attempt to compare nil with number`, en `if cp.textw(corte, tam) <= ancho`. El nil es el
resultado de `cp.textw`, y `cpTextWidth` **siempre** hace `lua_pushinteger`: no hay camino en C que devuelva
nil. El archivo de la tarjeta es byte a byte el del repo (19465 B), y el harness con letra ancha recorre ese
mismo bucle sin fallar. O sea que el nil no lo produjo el script ni la función: lo produjo **la VM corrupta**.

- **`on_draw` corre en la tarea de render** (`ActivityManager::renderTaskLoop`, núcleo 1) y **`on_tick` y
  `on_key` en el loop de Arduino**, y cada uno abre su propio worker de `runBounded` sobre el MISMO
  `lua_State`. `ActivityManager::loop()` llama a `currentActivity->loop()` sin ningún candado, a propósito
  ("do not hold a lock here"). Con un tick cada 120 ms y un `on_draw` del índice de 275 ms (`dibujo lento` en
  el log, por las decenas de `cp.textw` que miden cada título), el solapamiento era cuestión de tiempo.
- **Un mutex recursivo alrededor de toda entrada a la VM** (`VmGuard` en `LuaApp.cpp`): `open`, `close`,
  `callbackWith`, `onHeard`, `onReply*`, `cancelQueued`, `takeRequest`, `hasRequests`, `setBusy`. Recursivo
  porque `cancelQueued` llama a `onHeard`/`onReplyError`. La cola de pedidos también va adentro: la llena el
  worker de un callback y la vacía el loop.
- Existía desde 1.5.48 y no se veía porque los `on_draw` de las apps de fábrica tardan milisegundos. El harness
  de escritorio no lo puede encontrar: es de un solo hilo por diseño. **Regla**: el `lua_State` de una app lo
  toca un hilo por vez, siempre, y la puerta es `VmGuard`.

## Viajes, la app de Lua, y `cp.say` (1.5.111)

Diseño en `docs/ws397/VIAJES_APP.md`; **la verdad sobre nombres y formas en `docs/ws397/VIAJES_CONTRATO.md`**
(decisiones del dueño: sin Diario ni libro del viaje; la guía se guarda directo en la tarjeta sin confirmar;
antes de generarla la app pregunta POR VOZ lo que le falta al itinerario; Preguntar por voz con el viaje entero en
el contexto y respuesta hablada). Tres paquetes en paralelo, cada uno con su prueba:

- **Servidor y web** (`server/src/viajes.ts`, ~1400 líneas): modelo `Trip` resucitado de `d13923b^` SIN adjuntos
  (los papeles son TEXTO pegado con campos), con `hotel`, `code`, `guide` y un viaje `active` por cuenta; doc
  `trips` en `fsjson`. Rutas web `/api/trips` y `/api/trip*` y la **sexta pestaña Viajes** de `/board` (agenda por
  día con editor en hoja, papeles, lista, guía con las preguntas como campos, buscador de destino). Doce servicios
  bajo `POST /api/apps/call` (`VIAJES_SERVICES`): `viajes.lista/activar/viaje/papel/llevar/sugerir/
  sugerir.agregar/guia.preguntas/guia.generar/guia.seccion/preguntar/recordar`. La guía es un trabajo con
  **búsqueda web** (`appsProseSearch` en `appsLlm.ts`, misma herramienta que `chatSearch`); `guia.preguntas` decide
  SIN modelo mirando el itinerario (¿hay hotel? ¿llegada? ¿salida?) más una de intereses; `preguntar` busca sólo
  con "busca" (regla 1.5.103) y devuelve `spoken` ≤ 220 para el parlante; `recordar` crea el recordatorio 2 h
  antes en la zona del viaje. **El espejo al calendario vuelve** (`syncCalendar`, eventos con `tripId`;
  `normalizeCalendar` dejó de descartarlos): sólo ítems con hora. Probado con curl y Playwright a 360 px; lo que
  llama al modelo no se pudo ejecutar acá (sin clave) y queda con `{ok:false, error}` limpio.
- **`examples/Apps/viajes.lua`** (49 KB de los 64 del tope): pantallas 0-11 menos Diario; JSON propio (no hay
  `json` en el cajón) **sin `pcall`**, porque un `pcall` ahí se traga la guardia de instrucciones y el segundo
  disparo condena la VM. Medido con `armStepLimit`: `on_open` ≈ 110 k instrucciones con un `viaje.json` de 35 KB
  (tope 400 k). Archivos: `viaje.json` (vista compacta normalizada), `papel-<id>.txt`, `guia-N.txt`,
  `pendientes.json` (tildes sin red, se reproducen en Actualizar), `respuesta.txt`. `cp.save`: `trip`, `viajes`,
  `updE/updM`, `job`. Escenario entero en `test/lua_sandbox/scenarios/viajes.lua` (Lisboa 9 días + Cusco: las
  once pantallas, la guía con preguntas contestadas y salteadas, retomar el trabajo, cambiar de viaje, sin reloj).
  **La app no levanta la red sola**: la levantan Actualizar, la guía, Sugerir, Preguntar, Agregar por voz y Recordar.
- **`cp.say(texto)`** (firmware): `Request::Kind::Say`, `GET /api/tts?…&max=45` desde el host y `SpeechOut` en
  `LuaAppsActivity`; la fase vuelve a `Idle` apenas arranca el audio, así la app sigue y `cp.view` se abre encima.
  Atrás corta la voz (y ese Atrás no llega a la app); `cp.listen` la corta antes de abrir el micrófono (I2S
  único); `preventAutoSleep()` mientras habla. En el harness: `fake.said`.

**Instalar**: copiar `examples/Apps/viajes.lua` a `/Apps` de la tarjeta (modo memoria USB). Cargar el viaje en
`/board` → Viajes y en el aparato Juegos → Viajes → Actualizar. **Pendiente de hardware**: red, micrófono y voz
desde la app, y una guía generada de verdad con la clave de las apps cargada.

## Viajes v2: cada día su lugar, y la guía por día y a pedido (1.5.112)

Probando 1.5.111 el dueño dijo lo obvio: *"es un viaje de 13 días, obvio es de varios lugares… de ahí toma un
crucero, visita islas, vuelve a Roma, de ahí a Madrid, boletos de tren, cada día en un hotel distinto"*, y *"la
guía obviamente es por día, no para el viaje en general… sólo por petición del usuario por día"*. La v1 tenía UN
destino, UN hotel, UN clima y UNA guía de diez secciones para todo el viaje. Contrato v2 en
`docs/ws397/VIAJES_CONTRATO.md` (la sección 7 de `VIAJES_APP.md` queda como historia).

- **Datos**: cada día lleva `place`, `hotel` y `guide {at, answers, text}`; el viaje pierde `hotel`, `guide`,
  `weather`, `lat/lon` (queda `timezone`). `normalizeTrip` migra `/data/trips.json` al leer: el hotel del viaje
  se copia a cada día si ninguno tenía, y la guía general se tira (no tiene día). El `place` del viaje es un
  resumen ("Roma · crucero · Madrid"); vacío, se arma con los lugares distintos de los días.
- **Servicios** (`VIAJES_SERVICES`): `viajes.guia.preguntas {id,date}` decide SIN modelo qué falta para ESE día
  (`hotel` si no hay hotel, `llegada` si cambia de lugar y no hay vuelo ni tren cargado, `intereses` siempre);
  `viajes.guia.generar {id,date,answers}` es un trabajo de UNA `appsProseSearch` (600-1000 palabras: qué hay
  cerca de lo agendado —Casa Batlló → qué hay alrededor—, qué se está perdiendo uno, cómo moverse, dónde comer,
  lo práctico de esa fecha; progreso "Buscando cerca de Casa Batlló…"); `viajes.guia.dia {id,date}` devuelve el
  texto (≤ 24 KB) o `sin guía`. `viajes.guia.seccion` **no existe más**. `viajes.preguntar` lleva en el contexto
  el lugar y el hotel de cada día y la guía del día preguntado. Rutas web nuevas
  `GET|POST /api/trip/:id/day/:date/guide` y `…/guide/questions`; `POST /api/trip/day` acepta `place` y `hotel` y
  sólo toca lo que viene en el cuerpo.
- **Web**: la hoja del viaje ya no tiene buscador de destino ni hotel ni guía; cada tarjeta de día muestra
  `📍 lugar · 🏨 hotel` y abre la hoja del día; debajo de los ítems, "Generar la guía de este día" / "Leer la
  guía" / "Rehacer", con las preguntas del día como campos y la barra del trabajo (varios días a la vez).
- **`viajes.lua`** (54 KB): se fue la guía general entera (y con ella el **error de la línea 763**: `k .. ". " ..
  guia.titulos[k]` con `titulos` nil leído de la tarjeta). En Día y Hoy, fila "Guía de este día": en la tarjeta →
  `cp.view`; en el servidor (`guide.ready`, hecha desde la web) → se baja directo; si no → preguntas por
  `cp.listen` (Atrás salta) → trabajo → `job.status` cada 5 s → `guia-<date>.txt`. Con guía, "Rehacer la guía
  del día" sin confirmar. Guía (7) es la lista de días con "guía lista" / "en el servidor" / "generando…";
  Inicio dice "Guía · 3 de 13 días" y "Retomar la guía del día N" si quedó un trabajo (`cp.save` `job`,
  `jobDate`, `jobTrip`). Hoy/Día muestran "Roma · Hotel Artemide"; Agenda, el lugar de cada día.
  **Regla que sale del 763**: todo lo que viene de `cp.read`/`cp.load`/`cp.files`/servidor pasa por `s()` o
  `n()` antes de concatenar, comparar o indexar. Los 60 `..` del archivo están auditados. El escenario nuevo
  recorre un viaje de 13 días con días sin lugar y sin hotel, el flujo entero de la guía, Retomar tras
  `fake.reload`, sin reloj, errores y un `viaje.json` deliberadamente incompleto (la clase del 763).
  `on_open` ≈ 210 k instrucciones con un `viaje.json` de 38 KB (v1: 202 k; tope 400 k).
- **La web ya no pide el token del aparato** ("eso nada que ver"): la tarjeta "Token del aparato" de Ajustes →
  Avanzado sale sólo en un servidor sin cuentas (`!multi()`), y si `/auth/me` no contesta la entrada dice "No se
  pudo conectar" en la pantalla de cuenta en vez de caer a la puerta del token. Con cuentas, el token lo acuña
  el aparato y se vincula con el código; la web no lo toca.

**Instalar**: copiar `examples/Apps/viajes.lua` a `/Apps` (modo memoria USB); las guías viejas (`guia-N.txt`)
las ignora. **Pendiente de hardware**: el flujo de la guía con la clave de las apps cargada, y una guía
generada de verdad desde la web.

## Libros: el aparato le pide libros a un bot de Telegram (servidor, después de 1.5.112)

Pedido del dueño con dos capturas de su Telegram: *"Digo un nombre, me llega esa lista, la selecciono, luego
aprieto el botón epub y se baja"*, y *"obvio todo LUA"*. Contrato en `docs/ws397/LIBROS_CONTRATO.md`. **Sin cambio
de firmware**: la app usa las puertas que ya existen.

- **Un bot no puede hablarle a otro bot**, así que el servidor le escribe al bot **como la cuenta de Telegram del
  dueño** (MTProto con `@mtcute/bun`, `server/src/telegram.ts`): una sesión por cuenta de `/board`, archivo
  `/data/telegram/<accountId>.session` (`TELEGRAM_DIR`), config en el doc `telegram` de `fsjson` (el api hash es
  secreto: nunca vuelve, sólo `hasHash`). Se abre desde **`/board` → Ajustes → Avanzado → Telegram (app Libros)**:
  api id y api hash de https://my.telegram.org (API development tools), teléfono, `@bot`, Enviar código, Entrar
  (contraseña de dos pasos si la pide), Probar, Cerrar sesión. Rutas `/api/board/telegram*` con la sesión de la
  web, nunca con el Bearer de un aparato vinculado (403).
- **Cómo se le habla al bot** (`askBot`): `sendText`, y después **sondeo** de `getHistory(bot, {limit:6})` cada
  700 ms hasta ver un mensaje entrante con id mayor (20 s para texto, 120 s para el archivo), con una vuelta de
  gracia por si la lista llega en dos mensajes. Nada de `on('new_message')`: el cliente va con
  `disableUpdates`. Apretar un botón en línea = `getCallbackAnswer` con el `data` del botón cuyo texto es el
  formato (`b.type.data` en esta capa TL, no `b.data`); si el bot no contesta el callback se ignora y manda el
  historial. `connect()` resuelve al toque y reintenta solo para siempre, así que sin red inundaba el log: se
  sondea con `help.getNearestDc` bajo 15 s y, si falla, se destruye el cliente. Un pedido por cuenta a la vez
  (`telegram_busy`); 401 → `session_lost`.
- **Servicios** (`LIBROS_SERVICES`, `server/src/libros.ts`): `libros.estado`, `libros.buscar {q}` →
  `{results:[{title, code}]}`, `libros.ficha {code}` → `{title, author, year, pages, genre, desc, formats}`,
  `libros.bajar {code, format}` → trabajo (tope 40 MB; `saveFile` acepta ahora `maxBytes`, el default sigue en
  8). Lo que dice el bot se lee con funciones puras (`librosParse.ts`: `Título /comando` por línea; ficha
  `Título - Autor`, año, páginas, género, descripción; botones que parecen formato) probadas con los textos
  exactos de las capturas en **`./test/libros/run.sh`** (también en CI). Sin sesión: `code:"no_telegram"`.
- **`examples/Apps/libros.lua`** (28 KB, `on_open` ≈ 20 k instrucciones): Inicio con "Buscar por voz" y la lista
  de **Bajados** (`bajados.json` propio; OK abre en el lector) → `cp.listen(10)` → Resultados → Ficha (título,
  autor, "1967 · 345 páginas · Novela Drama", filas "Bajar EPUB"/"Bajar PDF", "Leer la descripción", "Otra
  búsqueda") → `job.status` cada 3 s con `label` → `cp.download(id, nombre, "books")` a `/Books/libros/` →
  "Abrir en el lector". **El nombre de archivo lo sanea la app**: `cp.download` sólo acepta `[A-Za-z0-9._-]`,
  así que "Cien años de soledad.epub" se guarda como `Cien-anos-de-soledad.epub` (el título original queda en
  `bajados.json`). Escenario en `test/lua_sandbox/scenarios/libros.lua`, incluido un `bajados.json` roto.
- **Hueco conocido**: el firmware no le manda "Atrás largo" a las apps (`LuaAppsActivity` sólo pasa
  arriba/abajo/OK/Atrás y el mantenido sale de la app), así que no hay tecla para sacar un libro de Bajados; la
  app ya atiende `on_key("backlong")` para cuando exista. El libro que ya no está en la tarjeta se saca solo.
- El puente es genérico (cualquier bot que conteste `Título /comando` y entregue el archivo con un botón). Qué
  bot se carga es del dueño.

**Instalar**: copiar `examples/Apps/libros.lua` a `/Apps`; conectar Telegram en la web; en el aparato Juegos →
Libros. **Pendiente de hardware y de Railway**: la sesión real de Telegram (código, contraseña), la cadencia
del bot de verdad (si manda "Buscando…" antes de la lista, la ventana de gracia es lo que hay que tocar) y una
bajada entera.

## Deletreo, nombres fresones, la mascota y el sudoku (1.5.113)

Pedidos del dueño después de probar LIBRARY en vivo ("anduvo bien!"):

- **"Le dije Ángeles Mastretta y entendió angeles mastretas."** Dos arreglos en `libros.buscar`: (1) si el
  bot no encuentra nada, el servidor le pide al modelo barato (`chatText`, mismo proveedor que Hablar) que
  corrija el dictado con lo que sabe de autores y libros y busca de nuevo; devuelve `corrected` y la app dice
  "Buscando «Ángeles Mastretta»". Sin clave o con error, se queda con el vacío: el corrector nunca hace fallar
  la búsqueda. (2) **Deletrear**: en la pantalla sin resultados, fila "Deletrear el nombre" → `cp.listen(20)` →
  `libros.buscar {q, spelled:true}`; `lettersToWord()` (`librosParse.ts`, pura, probada) pasa los nombres de
  las letras en español e inglés a palabra ("a, ene, ge, e, ele, e, ese, espacio, eme…" → `angeles m…`) y
  después pasa por el corrector para acentos y mayúsculas. Las dos búsquedas van adentro del mismo candado.
- **Nombres**: la app de libros se llama **LIBRARY**, el Librito **GHOSTWRITER** y la guía de viaje
  **CONCIERGE**. Es sólo la primera línea de cada `.lua` (`titleFromHeader` la toma tal cual, mayúsculas
  incluidas); los archivos siguen siendo `libros.lua`, `librito.lua`, `viajes.lua`, y con ellos las carpetas
  `/Books/libros/` y `/Apps/data/<app>/`.
- **Puerta nueva `cp.image(x, y, w, h, bits [, escala])`** (firmware, la única razón del release): bitmap de
  1 bit empaquetado por filas (MSB primero, 1 = tinta, fila redondeada a byte), hasta 256 × 256, escala 1..8.
  Pinta píxel a píxel con `drawPixel`/`fillRect` a propósito: `GfxRenderer::drawImage` no rota los bits
  (tiene un `TODO`) y así la orientación del panel se aplica sola. En el harness queda en `fake.images`.
- **Mascota** (`examples/Apps/mascota.lua`, 45 KB; créditos y reglas en `docs/ws397/MASCOTA.md`): tamagotchi
  con **dibujos ajenos, como pidió el dueño** ("no las generes tú"): la gatita es "Tiny Kitten Game Sprite" de
  Segel (OpenGameArt, **CC0**) y los iconos y el huevo son de OpenCritter de SuperMechaCow (**MIT**); pasados a
  1 bit con Pillow (umbral, sólo contorno) a 88 × 120 y pintados a escala 2. Cuatro barras (hambre, ánimo,
  energía, higiene) que bajan con el reloj de verdad (`cp.time().epoch`, tope de 12 h por ausencia, guardado
  en `cp.save`); Alimentar, Jugar (mayor o menor, 3 rondas), Dormir, Limpiar, Info; nombre por voz al nacer;
  sacudir la despierta, boca abajo la duerme; 36 h de descuido y "se fue" (huevo nuevo). Repinta sólo cuando
  cambia algo (cuadro cada 4 s). Descartados por licencia: picotamachibi (sin licencia), Matagotchi (GPL),
  ToffeeCraft (sin redistribución).
- **Sudoku** (`examples/Apps/sudoku.lua`, 30 KB): con sólo palanca, OK y Atrás, el cursor recorre las celdas
  vacías, OK cicla 1…9…vacío y Atrás abre el menú (Dictar, Verificar, Pista, Nuevo, Salir). **Dictar** entiende
  "fila tres, columna cinco, siete", "3 5 7", "borra fila 2 columna 4" y "pon un siete" en la celda del cursor.
  Generar con unicidad en Lua no entra en 400 k instrucciones, así que van **36 tableros base verificados en
  Python** (12 por nivel, únicos) y cada partida se deriva por simetrías (renombrar dígitos, barajar filas y
  columnas dentro de sus bandas, bandas, pilas, transponer): miles de tableros distintos por base. OJO: el
  primer generador de azar usaba `% 2^31`, que con `LUA_32BITS` pasa a float y colapsaba a 12 tableros de 20;
  el escenario lo atrapó. Ahora es xorshift32 entero. La partida se guarda con `cp.save` (Continuar al volver).
- Los tres escenarios nuevos corren en `./test/lua_sandbox/run.sh`; `on_draw` de la mascota ≈ 4 k
  instrucciones, del sudoku ≈ 4 k.

**Instalar**: copiar `libros.lua`, `mascota.lua` y `sudoku.lua` (y `librito.lua`/`viajes.lua` por el nombre
nuevo) a `/Apps`, y actualizar a 1.5.113 por OTA antes de abrir la mascota (sin `cp.image` no dibuja).
**Pendiente de hardware**: el tamaño de la gatita en el vidrio (escala 2; si queda chica, escala 3 en
`dibujarCasa`), el micrófono para el nombre, los gestos, y una corrección y un deletreo reales en LIBRARY.

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
