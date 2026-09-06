# CrossPoint port — Waveshare ESP32-S3-ePaper-3.97 ("ws397")

Fork de [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) con soporte para la placa
Waveshare ESP32-S3-ePaper-3.97 (SSD1677 800x480, ESP32-S3-WROOM-1-N16R8, ES8311 + NS4150B, PMIC AXP2101-compatible,
RTC PCF85063, IMU QMI8658, SD 4-bit). El submódulo `freeink-sdk/` apunta al fork propio con el perfil de placa.

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
  de un release de GitHub). Comparación estricta major.minor.patch. Servidor: repo `ws397-server` (Bun + Hono,
  Railway, volumen en `/data`).
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
- Mosaicos 2x3 (Leer, Preguntar, Recordatorios, Biblia, Música, Ajustes) con íconos Lucide de 48 px generados en
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
- Widgets: clima, próximo recordatorio, agenda de hoy (o la frase si no hay eventos), contador de mensajes en la
  barra. Íconos de 24 px en `src/components/icons/hubWidgetIcons.h`. Pendiente: temperatura interior (SHTC3).

## Roadmap acordado

La lista completa de funciones, con fase, estado y contrato del servidor, está en `docs/ws397/FUNCIONES.md`
(fusión de lo planeado con lo que hacen el reTerminal Sticky y el ZecTrix Note 4). Resumen:

0. Hardware: volumen, trackball/botones PCF8574, wake por alarma del RTC, driver SHTC3, deep sleep medido, IMU
   por polling (boca abajo = silenciar, doble golpe = PTT, sacudir = cancelar; modo atril horizontal para el hub).
1. Hub + preguntarle al libro + cliente HTTP + sincronización con `GET /api/hub` y widgets (hecho). Falta:
   pizarra de mensajes (1.8), mosaico "Hablar" (PTT), ajustes del hub en la web UI.
2. Voz: el servidor clasifica la intención de una sola grabación (pregunta, tarea, recordatorio, compras,
   nota, mensaje, temporizador, traducción, alarma); recordatorios con repetición y alarma del RTC; varias
   listas de tareas (Entrada, Casa, Trabajo, Administrativo, Compras, proyectos) con vista por semana ISO;
   TTS por el parlante; traductor; temporizador/Pomodoro; agenda; memoria del asistente.
3. Contenido: Biblia con índice en SD, versículo/frase del día, MP3 desde SD, RSS/lectura web, álbum de
   imágenes en 4 grises, clima detallado.
4. Juegos: damas, cartas (rummy, solitario, blackjack), retos mentales (sudoku, acertijos, cálculo), memoria
   (parejas, Simón), Tetris experimental, ajedrez opcional.

Descartado: radio por streaming, Casa Cerebro, lectura en voz alta de libros, Spotify, auto-rotación por IMU.

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
