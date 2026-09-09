# Rediseño ws397 — brief de arquitectura

Objetivo: sumar lo mejor de **folloup-sticky** (misma placa, ESP-IDF C++17) y **rustmix-wave** (misma placa, Rust) a
nuestro firmware **sin que sea un Frankenstein**, con una **enorme mejora visual**. Base: siete lecturas exhaustivas
del código nuestro y de los dos proyectos (clonados en `scratchpad/{folloup,rustmix}`), 2026-09-09.

Hechos confirmados por el usuario: la placa tiene un **botón PWR físico** cableado al PWRKEY del AXP2101 (no es un
GPIO), y la "rueda" de rustmix-wave es **nuestra misma palanca ARRIBA/ABAJO + botón** (no hay encoder). El IMU tiene
que quedar **implementado y muy funcional** en esta etapa.

---

## 1. Estado actual, por subsistema (lo que hay que arreglar)

### 1.1 Pantalla y refresco
- Único cuello de botella hacia el panel: `GfxRenderer::displayBuffer/displayBufferAsync` (lib/GfxRenderer/GfxRenderer.cpp:1707/1714)
  → `HalDisplay` → `FreeInkDisplay::displayBuffer` → `Ssd1677Driver::displayImpl`. Nadie en `src/` llama al display directo.
- **No hay buffer sombra ni comparación con el frame anterior**: cada render paga 144 KB de SPI (FAST) o 192 KB
  (HALF/FULL) + la onda + el ciclo de rieles, aunque el frame sea idéntico (tick del hub con el mismo minuto, popup quieto).
- **La regla del panel (limpio cada 10-15 parciales) NO se cumple a nivel aparato**: hay ~27 contadores
  independientes (`PARTIALS_BEFORE_CLEAN` en 25 Activities + lector + `GrayText`), todos miembros por instancia que
  `ActivityManager` recrea en 0 en cada cambio de pantalla, y decenas de `displayBuffer()` FAST sin contar (OptionPopup,
  `GUI.drawPopup`, `drawPowerHoldBanner`, todas las pantallas upstream, Voice/HubSync/HubLocation/Crash/ReminderAlert/
  AudioTest/ServerTest/AskBook). Hub(11) → Ajustes(N) → Hub(11) → popup(N) no limpia nunca.
- `HubActivity::render` con "Próximamente" hace **dos refrescos** (`GUI.drawPopup` refresca adentro y el hub vuelve a refrescar).
- Driver: `_needsInitialFull` (primer paint tras `begin()` = HALF) **se saltea con `fadingFix`** (`if (!turnOff)`), y
  `displayImpl` promueve cualquier primer paint a HALF aunque se haya pedido FULL. Silent restart mete un destello 0xD7
  después de cada Activity de red.
- `TimerActivity` pone el contador en 0 sin limpiar; `DictionaryDefinitionActivity` usa el contador como bandera.
- El SSD1677 **no puede refrescar por ventana** (0x44/0x45 sólo acotan escrituras; 0x20 maneja todo el panel). Confirmado
  en los tres códigos. `displayWindow` sólo ahorraría SPI, no onda: **no se usa**.

### 1.2 Energía y botones
- OK (GPIO5) es confirm+power (`InputStyle::DigitalConfirmPowerHold`, 400 ms). El PWR físico del AXP2101 está
  **totalmente desconectado del firmware**: `BatteryMonitor.cpp` sólo toca 0x03/0x68/0x30/0xA4/0x34/0x35/0x01. GPIO38
  (IRQ del PMIC, confirmado por folloup) está libre en el perfil WS397.
- **GPIO38 no es RTC GPIO** (S3: 0-21) → la IRQ del PMIC **no puede despertar del deep sleep**. Sólo sirve despierto.
- `handlePowerHold()` con cartel en pantalla hace `return` antes de `MUSIC.pump()` y `checkTimeAlarms()`: mantener OK
  0,6-3 s congela la música y las alarmas.
- `verifyPowerButtonWakeup()` exige GPIO5 bajo en dos muestras; con `input.power = PIN_UNASSIGNED` devolvería true siempre.
- `powerDownRailsForSleep()` es no-op en WS397: en deep sleep DC1/ALDO1-3 siguen a 3,3 V (panel, códec, SD, sensores).
- `armReminderWake()` corre dos veces por `enterDeepSleep()`.

### 1.3 Captura de voz
- `VoiceRecorder::start()`: `audio.begin()` → **`blip()` BLOQUEANTE (~350 ms de puerto)** → recién `beginCapture()`.
  Todo lo que se dice al oír el pitido se pierde: **es la causa directa de "se come la primera palabra"**.
- El SDK **ya soporta** reproducir encima de una captura abierta (`ensureI2s` crea TX+RX full duplex; `play()` no corta
  la captura; `taskLoop` deja TX si `capturing_`). El único orden prohibido es play → beginCapture. El pitido tiene que
  ser de 16 kHz y salir por el MISMO `AudioManager` de la grabadora.
- DMA de RX = 90 ms: nada bloqueante > 80 ms con el mic abierto (ni un HALF, ni un `blip()` que no drene).
- Notas de voz se escriben a la SD en el acto; no hay revisión. `AudioTestActivity` ya reproduce desde PSRAM con el
  mismo `AudioManager` (sin `end()` en el medio): es la pieza que falta.
- `blip()` usa volumen 80 fijo (ignora el volumen único del aparato).

### 1.4 Capa visual (tres sistemas superpuestos)
- `ThemeMetrics` (Lyra: header 84 + topPadding 5 = **89 px**, hints 60: 18 % de la pantalla en cada lista),
  `fui::ThemeTokens` (2/4/8/16, sólo pantallas upstream) y **nuestras ~35 Activities que no leen tokens**: siete
  márgenes laterales distintos (10/12/16/18/20/22/24), nueve altos de fila (34…74), N = 0…40 px entre encabezado y
  contenido, cuatro radios y cinco recortes de alto para el "único" resalte, tres encabezados (Lyra 84, hub 44, música 30),
  cuatro barras de progreso, tres sistemas de números grandes.
- Fuentes UI: SMALL = NotoSans 8 **sólo regular** (se pide BOLD en 4 archivos y no existe), UI_10 y UI_12 Ubuntu
  regular+bold. **No hay fuente de display** ni de título. Pasos de línea a mano que pisan glifos (Translator 26 < 29,
  News 18 < 24, Games 22 < 29).
- Tramados: sólo 25 % y damero 50 % (con `TODO`). No hay 75 % ni Bayer: no existe rampa de 4 grises barata.
- Quedan **bloques negros macizos con texto blanco**: pane del traductor, `drawPopup` en Lyra (todo "Próximamente" es un
  manchón), barra de título de Música (skin deliberada).
- `freeink::Icon` no tiene primitiva de dibujo: el bucle está copiado en 4 archivos. Hub usa iconos de 48 px con
  mosaicos de 82-100 px y los de 64 px generados se tiran; etiquetas caen a SMALL regular cuando no entran.
- `FreeInkApp::refreshHint()` del SDK no lo lee nadie.

### 1.5 IMU y sensores
- `freeink::Imu` (SDK): CTRL7 fijo accel+gyro a 28 Hz, sin modo solo-accel, sin ODR, sin tap/WoM/any-motion, sin
  STATUS1/TAP_STATUS/FIFO. `HalTiltSensor` sólo sondea el giro en el lector; con tilt activo deja el chip encendido
  (~1 mA) en todas las pantallas. Ejes/signo de montaje en la ws397 **desconocidos** (mapeo del X3).
- **GPIO39 = amp enable = INT1 del IMU**: NUNCA habilitar INT1_EN. Todo evento del chip por polling de registros. INT2 en
  GPIO40 según folloup (no cableado en nuestro perfil; tampoco es RTC GPIO).
- SHTC3: driver en `src/util/Shtc3`, se lee **dentro del render** (13 ms de `delay`), descarta la humedad, y sólo se
  muestra si hay clima del servidor.

### 1.6 Framework
- Render en tarea propia (prio 1, core 1) bajo `RenderLock`; sólo se renderiza la Activity de arriba; al hacer pop la de
  abajo se repinta ENTERA. `ActivityManager` admite **una sola acción pendiente por pase**: los push de
  `checkTimeAlarms`/`checkVoiceShortcut` compiten con la transición de la Activity y pueden perderse.
- `OptionPopup` no es Activity: se dibuja sobre el framebuffer; cada movimiento del cursor = refresco de panel entero.
- Sin onResume: el hub no resincroniza reloj/contador cuando se le cierra algo encima.

---

## 2. Mecanismos de referencia que adoptamos (valores concretos)

### 2.1 folloup-sticky (misma placa)
- **PWRKEY del AXP2101**: reg **0x27** = PressOnTime bits[1:0] (2 = 1 s), PressOffTime bits[3:2] (1 = 6 s; 3 = 10 s),
  IrqLevelTime bits[5:4] (0 = 1 s). **0x10** COMMON_CONFIG: bit2 = apagado por PWRKEY habilitado, bit0 = shutdown por
  software. **INTEN2 0x41 / INTSTS2 0x49** (write-1-to-clear): bit0 PKEY_POSITIVE (soltar), bit1 PKEY_NEGATIVE
  (apretar), bit2 PKEY_LONG, bit3 PKEY_SHORT, bit6 VBUS_REMOVE, bit7 VBUS_INSERT. IRQ open-drain activa en bajo en
  **GPIO38**, con pull-up; queda baja mientras haya status sin limpiar. Largo gana sobre corto (un hold latchea los dos).
  VBUS IRQs apagadas a propósito.
- **Panel**: framebuffer + `previous_framebuffer` (sombra) en RAM interna; `RefreshChangedRegion()` = memcmp → si igual
  no toca el panel; si no, parcial de pantalla completa (0x26 ← sombra, 0x24 ← frame, 0x22=0xFF, 0x20). Full = 0xF7 con
  reset HW; fast = 0xD7 con 0x1A=0x6A. **Tope 8 parciales → full 0xF7** ("el fast destella pero no restaura contraste").
  Nunca escribir 0x21. Cola de UN slot que fusiona (full gana, nunca se pierde un full pendiente).
- **`ui_refresh_runtime`**: 15 superficies, "el último gana" por superficie, un solo RefreshRequest por lote; con overlay
  que captura input → sólo se refresca el overlay, el fondo se descarta (el estado se aplica igual); cerrar un overlay
  grande → full. **Sin parciales antes de `s_startup_complete`** (primer paint = full).
- **Grabación**: 16 kHz, pre-roll de 1000 ms en anillo PSRAM (32 KB) mientras "armado"; **la captura arranca ANTES del
  pitido** ("esperar el tono se come la primera palabra; el tono se superpone al arranque de la toma"); toma ≤ 10 s;
  al soltar: pitido de fin → **reproducción desde PSRAM** → etiqueta / **Discard sin tocar la SD**. Validación antes de
  transcribir: ≥ 500 ms y pico ≥ 700 en ≥ 3 ventanas de 240 muestras (filtro de silencio).
- **design_tokens** (para copiar la ESTRUCTURA, no los tamaños: ellos tienen fuentes de 22-165 px y touch):
  spacing 2/4/8/12/14/16/20/24/32/40/48/56/64; roles tipográficos {tamaño, peso} (Label S/M/L, Heading 1-3, Display,
  Body, Detail); rampa **0x00 / 0x55 / 0xAA / 0xFF** con roles: SurfaceBase blanco, SurfaceRaised gris claro,
  SurfaceEmphasis gris oscuro, SurfaceInverse negro; TextPrimary/Secondary/Inverse; BorderSubtle (gris claro) /
  BorderStrong (negro); FocusRing; sombra = gris claro desplazada 8 px; status bar 44 px; botón 56 px; modal inset 16,
  padding 14, borde 2, sombra 8; toast igual; list_item padding 12/16; menu_item 72; badge 32; progress 8.
- **Auto-sleep por IMU**: sondeo 200 ms, movimiento si Σ|Δ| ≥ 60 mg o eje ≥ 25 mg; quieto si Σ ≤ 20 y eje ≤ 8 durante 2 s.
  Bloqueadores: grabando, guardando, reproduciendo, apagando, refrescando, escribiendo SD, AP WiFi, sync de hora.
- **task_config** centralizado: core 1 = app/UI/audio, core 0 = red; prioridades RecordCapture 5, UiRefresh 4, Display 3,
  SleepMotion 3, Wifi 3, Storage 2, Gemini 2, SensorPoll 2.
- Botones (iot_button): tap 180 ms, largo 500 ms, hold-repeat 350 ms; suspender el timer de botones alrededor del sleep.

### 2.2 rustmix-wave (misma placa)
- **`PanelRefreshCoordinator`**: UN contador para menús, lector y juegos; tope **24** parciales → base 0xF7; AfterWake /
  ManualGhostCleanup / SafetyFallback fuerzan base. Parcial = sólo 0x24 (el panel promueve 0x24→0x26 solo).
- **PWRKEY sin IRQ**: polling de INTSTS2 cada 100 ms; INTEN2 |= 0x0C; limpia sólo los bits de tecla. Corto → menú de
  mantenimiento ("Limpiar fantasma ahora"); largo → imagen de sueño. `SleepWakeGuard` 900 ms tras dormir.
- **Eventos IMU** (`imu_events.rs`), muestreo 80 ms sólo cuando alguien los necesita, ±8 g: **TILT** eje planar (x,y)
  ≥ 550 mg durante 3 muestras, latch hasta ≤ 320 mg; **SHAKE** |‖a‖−1000| ≥ 420 mg, cooldown 900 ms; **ROTATE** gyro
  ≥ 120 dps, latch hasta ≤ 45 dps, cooldown 650; **LEVEL** |x|,|y| ≤ 150 y |z−1000| ≤ 150 durante 3 muestras, flanco;
  debounce global 350 ms. Juegos: +X→Abajo, −X→Arriba, +Y→Izq, −Y→Der. Umbrales ciclables en la pantalla "Motion".
- **`PHYSICAL_SMOKE_TEST.md`**: 9 secciones (build y boot; tecla power y refresco; lector; diccionario; calendario;
  notas de voz; red/alarmas/ajustes; juegos y sensores; layout del editor) con pasos verificables.

---

## 3. Decisiones de arquitectura (ya tomadas; la fase de diseño las revisa adversarialmente)

### D1. Coordinador de refresco + sombra — en `lib/GfxRenderer` (nuevo `PanelRefreshCoordinator`)
- Vive como miembro **mutable** de `GfxRenderer` (o singleton propio en la lib), enganchado en
  `GfxRenderer::displayBuffer/displayBufferAsync`: atrapa popups, banners, upstream, juegos, todo.
- Política: cuenta sólo los FAST **efectivos**; HALF/FULL/grises resetean; al llegar al **umbral (12, configurable)**
  promueve FAST → HALF; cada N limpiezas HALF (p. ej. 4) hace un **FULL 0xF7** (folloup: el 0xD7 no restaura contraste;
  verificar en hardware). `promoteNextRefresh` ya existe y queda como el hint de las pantallas.
- **Sombra de 48 KB en PSRAM** (la DRAM compite con WiFi/TLS). Antes de un FAST: `memcmp` (≈1 ms): si idéntico →
  **no se toca el panel** (ni SPI, ni onda, ni rieles) y se loguea. Tras cada display bloqueante exitoso → `memcpy`.
  Invalidación (`shadowValid=false`) en: `HalDisplay::begin`, `setInverted`, pipeline de grises (`displayGrayBuffer`,
  strips, `storeBwBuffer/restore`), `lendBuildStorage`, deep sleep, async (lector: se deja como está, sólo invalida).
  HALF/FULL nunca se saltean.
- **Primer paint tras `begin()` = HALF forzado por el coordinador** (además del driver), y se arregla en el SDK el
  `if (!turnOff)` que lo saltea con `fadingFix`.
- **Overlays**: `OptionPopup` guarda el rectángulo de fondo al abrir (`readFramebufferRegion`) y lo restaura al cerrar;
  los hosts hacen `if (popup.active()) { popup.processRender(); return; }` ANTES de `clearScreen` (regla única).
  `GUI.drawPopup` deja de refrescar por dentro: compone y devuelve el rect; el caller refresca. Cerrar un overlay grande
  → `promoteNextRefresh(HALF)`.
- Los ~27 contadores locales **se eliminan** en la ola B (rediseño), junto con `GrayText::nextRefreshMode` (pasa a
  consultar al coordinador y a resetearlo tras una página en grises).
- El lector conserva su cadencia configurable; el coordinador sólo le pone techo.
- Diagnóstico: contador global y "saltos por sombra" en el log del aparato (`[GFX] refresh fast n=…`).

### D2. Botón PWR real por el AXP2101 — `src/util/PowerKey.{h,cpp}` + perfil del SDK
- **Detección sin ISR**: la línea IRQ queda en bajo mientras haya status pendiente → en cada pasada del loop
  `digitalRead(38) == LOW` (gratis) → leer INTSTS2 (0x49) por I2C, decodificar, limpiar escribiendo 1 en los bits de
  tecla. Cero tráfico I2C en reposo. (rustmix sondea I2C cada 100 ms; folloup usa ISR+tarea: el nuestro es el mejor de los dos.)
- Init (una vez, en `setup()`): INTEN2 |= 0x0F (apretar, soltar, corto, largo); **0x27**: PressOffTime = **10 s** (3)
  para que el corte duro del PMIC nunca gane a nuestra barra de 3 s, IrqLevelTime = 1 s, PressOnTime = 1 s;
  0x10 bit2 = 1 (apagado por PWRKEY como escape de emergencia a los 10 s). Limpiar 0x48-0x4A al arrancar. Loguear
  0x20/0x21 (qué encendió/apagó). NO tocar rieles ni carga (regla de CLAUDE.md).
- Alimenta al SDK por `InputManager::setButtonHook()` devolviendo `(1<<BTN_POWER)` entre PKEY_NEGATIVE y PKEY_POSITIVE:
  `handlePowerHold()` y `getPowerButtonHeldTime()` siguen funcionando sin cambios.
- **Política**: PWR mantenido ≥ 600 ms → barra; 3 s → `enterDeepSleep()` (deep sleep, alarmas vivas). PWR **corto** →
  **"Limpiar pantalla"** (FULL 0xF7 vía el coordinador), como rustmix — útil y sin riesgo. **OK pasa a ser sólo
  confirm** (habilita OK largo a futuro; no se le da uso ahora).
- Perfil WS397 (SDK, commit propio + .patch): `InputStyle::DigitalButtons`, `input.power = PIN_UNASSIGNED`, campo nuevo
  `wakePin = 5` (OK sigue siendo el que **despierta**: ext1 en GPIO5; PWR no puede despertar porque GPIO38 no es RTC), y
  `pmicIrq = 38`. `PowerManager::armPowerButtonWakeup` arma `wakePin`; `verifyPowerButtonWakeup` verifica `wakePin`;
  `armWakeOnPins` chequea el retorno de `esp_sleep_enable_ext1_wakeup`.
- `handlePowerHold()` no debe cortar `MUSIC.pump()`/alarmas mientras muestra la barra. Sacar la duplicación de
  `armReminderWake`. Combo captura de pantalla POWER+ABAJO → PWR corto + ABAJO (o se saca).
- Wake por PWR queda para el futuro sólo vía apagado real del PMIC (mata las alarmas): **no en esta etapa**.

### D3. Pre-roll y revisión — `src/voice/VoiceRecorder` (sin tocar el SDK)
- Orden nuevo en `start()`: `audio.begin()` → **`beginCapture(16000)` PRIMERO** → descartar ~20 ms de asentamiento del
  ADC → `playBuffer(tono 16 kHz)` **no bloqueante por el mismo AudioManager**, drenando RX (`pump()`) mientras suena →
  marcar `spokenStart = recorded` en (playBuffer + 64 + 90 ms) → **silenciar (poner a 0) las muestras del tramo del
  tono** (se sabe exactamente cuáles) para que Whisper no lo oiga. `tooShort()` mide desde `spokenStart`; se agrega
  `spokenSamples()`. Pitido con el volumen único del aparato (no 80 fijo). Sin anillo permanente (choca con
  `anyRecording()`, alarmas, PTT y consumo): **el pre-roll es "no perder nada desde que se aprieta OK"**, que es lo que
  el usuario nota.
- Filtro de silencio antes de subir (como folloup): ≥ 500 ms y pico ≥ 700 en ≥ 3 ventanas → si no, "no se escuchó nada"
  sin gastar una llamada.
- **Revisión sólo en notas de voz** (los dictados al servidor siguen "apretar y listo"): `stop()` se separa en
  `stop()` (cierra el mic, deja el códec vivo) y `closeAudio()`; estado REVIEW en `NotesActivity`: se reproduce solo desde
  PSRAM (`playBuffer` sobre el PCM que ya está), **OK = guardar, Atrás = descartar, Arriba = escuchar de nuevo**. Recién al
  guardar se escribe a la SD. Los siete callers actuales no cambian de API.

### D4. IMU como capa de entrada — SDK (`freeink::Imu` ampliado, aditivo) + `src/input/MotionInput.{h,cpp}`
- SDK (commit propio + .patch): `setPower(accelOnly | accelGyro | off)`, `setAccelOdr(code)`, `readAccel()` (una
  transacción de 6 bytes), motor de **tap del chip** (CTRL9 `CTRL_CMD_CONFIGURE_TAP` 0x0C con CAL1-4, `Tap_EN` en
  CTRL8 0x09), `readStatus1()` (0x2F) y `readTapStatus()` (0x59) — **todo por polling, INT1_EN siempre en 0**.
  Registros del datasheet QMI8658A / SensorLib: verificar y loguear WHO_AM_I + revisión al arrancar.
- `MotionInput` (src): sondeo desde `loop()` cada **80 ms** (nunca desde render), sólo accel salvo que un consumidor
  pida ROTATE; eventos **TILT / SHAKE / ROTATE / LEVEL / FACE_DOWN / DOUBLE_TAP** con los umbrales de rustmix
  (550/420/120/150 mg, 350 ms, latch/cooldown) + boca abajo = eje normal ≈ −1 g con los otros < 300 mg durante 500 ms,
  histéresis 600 mg. Eje normal y signo: **desconocidos** → se calibran en hardware con la pantalla de diagnóstico.
- Usos globales (main.cpp, mismas guardas que `checkVoiceShortcut`): **boca abajo** = posponer alarma / callar
  temporizador / pausar música; **sacudir** = cancelar grabación / cerrar alerta; **doble golpe** = abrir Hablar (PTT).
  Usos por pantalla (API `MOTION.events()`): inclinar = pasar página en visores largos (respuesta, noticia, Biblia) y
  mover selección en el hub; juegos nuevos por movimiento: **Laberinto por inclinación** y **2048 por movimiento**.
- Pantalla **Ajustes → Movimiento**: ax/ay/az/gx/gy/gz en vivo, magnitud, último evento, umbrales ciclables con la
  palanca (como rustmix), estado del motor de tap. Es lo que permite calibrar ejes y signo sin cable.
- Consumo: accel-only en ODR baja en pantallas tranquilas; giro sólo cuando alguien pide ROTATE o en el lector con tilt;
  power-down en deep sleep y en el lector sin tilt. `HalTiltSensor` deja de encender el giro fuera del lector.
- **SHTC3**: caché de T y HR, lectura desde el `loop()` del hub (tick de 15 s), widget "Interior 23° · 45 %" **aunque no
  haya clima del servidor**, y bloque "ahora" de `WeatherActivity`.

### D5. Sistema de diseño y rediseño visual — lo decide la fase de diseño (§4); restricciones fijas
- Un solo módulo de tokens (`src/components/DesignTokens.h`) **y** un tema `Ws397Theme` (deriva de Lyra) que alimente
  `ThemeMetrics` → `uiThemeTokens()` para que Settings/FileBrowser/OPDS **se vean igual** que lo nuestro. Los tokens del
  tema y los nuestros salen de las mismas constantes.
- Nada negro macizo con texto blanco salvo skins deliberadas (barra de título de Música). Popups/modales: tarjeta
  blanca con borde 2 px y sombra tramada, como folloup.
- Rampa de grises barata en 1 bit: agregar **75 %** y **Bayer 4x4 ordenado** en `fillRectImpl` (hoy 25 % y damero 50 %).
- Primitiva `GfxRenderer::drawIcon(const freeink::Icon&, x, y, ink)` (borra 4 copias) y helpers de layout compartidos.
- Encabezado compacto propio (44-56 px, no 89) para todas nuestras pantallas; el hub y Música ya lo hacen a su manera:
  se unifican. Barra de botones: se mantiene la de 60 px (la UX de 4 celdas con iconos es buena y el usuario la pidió).
- Tipografía: evaluar agregar **una cara de título/display** (Ubuntu 14-16 bold o similar) y **SMALL bold**; flash al
  85 %, cada cara ~20-40 KB; regenerar con `lib/EpdFont/scripts`. Si no entra, sevenseg + UI_12 bold.
- Alto de fila = función de `advanceY` de las fuentes (nunca literales); márgenes = `contentSidePadding` único.
- Hub: iconos de **64 px** (ya generados), etiquetas siempre UI_10 bold, widgets como tarjetas.

### D6. Menores
- `docs/ws397/PHYSICAL_SMOKE_TEST.md` con la estructura de rustmix, adaptada a lo nuestro.
- `src/TaskConfig.h`: prioridades/stacks/cores de todas las tareas en un lugar + instantánea de memoria en el log.
- Conversor de unidades offline en punto fijo (longitud, peso, temperatura, volumen, velocidad), palanca/OK/Atrás; la
  fase de diseño decide dónde vive (mosaico propio vs. "Herramientas").
- `ActivityManager`: cola de acciones pendientes (o mover `checkTimeAlarms`/`checkVoiceShortcut` después de
  `activityManager.loop()`) para que un push global no se pierda.

---

## 4. Preguntas para la fase de diseño

1. **Lenguaje visual completo** (lo más importante para el usuario): sistema de tokens con valores concretos para esta
   pantalla (480x800, 1 bit + tramas 25/50/75 %, fuentes reales SMALL 23/18, UI_10 24/20, UI_12 29/24), encabezado
   compacto, barra de estado, tarjetas, listas (1 y 2 renglones), resalte único, diálogos, chips, barras, tableros de
   juego, visores de texto, y **maquetas** (PIL) de: hub, agenda, notas, visor de respuesta, música (mantener la skin),
   diálogo, ajustes (fui), un juego de tablero, la pantalla de Movimiento. Criterios: legibilidad en tinta, coherencia,
   nada negro macizo, sin tramas pegadas al texto, palanca/OK/Atrás, belleza, costo de implementación.
2. ¿Agregar fuentes (título/display + SMALL bold) o no? Con costo en flash medido.
3. Umbral del coordinador (8 folloup / 24 rustmix / 12 nuestro) y cada cuántas limpiezas HALF va un FULL 0xF7.
4. Dónde vive el conversor y cómo se reparte la grilla del hub (3x4 + "Mi día" ancho hoy; 13 mosaicos + Movimiento
   + Herramientas no entran sin repensar).
5. Revisión adversarial de D1-D4: qué se rompe, qué falta, qué es más simple.

## 5. Preguntas para el usuario con el hardware (no bloquean)
- Ejes y signo del IMU (pantalla Movimiento). Tiempos reales de FAST/HALF/FULL (`SSD1677_PROBE_DEBUG`). Si 0xD7 alcanza
  para el fantasma o hace falta 0xF7 cada N. Qué dejó el vendor en 0x27/0x22 (se loguea al arrancar).

---

## 6. Paquetes de trabajo, dependencias y archivos (para repartir sin pisarse)

| WP | Qué | Archivos | Depende de |
|----|-----|----------|------------|
| A1 | PWR por AXP2101 | SDK: `BoardConfig.h` (perfil WS397, campos `wakePin`, `pmicIrq`), `PowerManager.cpp`, `HalGPIO.cpp` (verify); src: `src/util/PowerKey.{h,cpp}` (nuevo), `src/main.cpp` (sección power) | — |
| A2 | Coordinador + sombra + overlays | `lib/GfxRenderer/GfxRenderer.{h,cpp}`, `lib/GfxRenderer/PanelRefreshCoordinator.{h,cpp}` (nuevo), `lib/hal/HalDisplay.cpp`, SDK `Ssd1677Driver.cpp` (fix `!turnOff`), `src/components/OptionPopup.h`, `src/components/themes/BaseTheme.cpp` (drawPopup), `src/util/GrayText.h`, `src/activities/ActivityManager.cpp` (cola de pendientes) | — |
| A3 | Pre-roll + revisión | `src/voice/VoiceRecorder.{h,cpp}`, `src/voice/VoiceNotes.{h,cpp}`, `src/activities/home/NotesActivity.{h,cpp}` (sólo el flujo REVIEW) | — |
| A4 | IMU + SHTC3 | SDK: `libs/hardware/Imu/*`; src: `src/input/MotionInput.{h,cpp}` (nuevo), `src/util/Shtc3.{h,cpp}`, `src/activities/settings/MotionActivity.{h,cpp}` (nuevo), `src/main.cpp` (sección gestos: **coordinar con A1**, misma persona o secuencial), `lib/hal/HalTiltSensor.cpp` | — |
| A5 | Smoke test + TaskConfig | `docs/ws397/PHYSICAL_SMOKE_TEST.md`, `src/TaskConfig.h` (nuevo) | — |
| B0 | Tokens + tema + primitivas | `src/components/DesignTokens.h` (nuevo), `src/components/themes/ws397/Ws397Theme.{h,cpp}` (nuevo), `UITheme.cpp`, `lib/GfxRenderer` (drawIcon, 75 %, Bayer), fuentes nuevas | diseño |
| B1 | Hub + widgets + barra de estado | `HubActivity.*`, `hubIcons.*`, `hubWidgetIcons.*` | B0, A2 |
| B2 | Listas: Agenda, Notas, Calendario, Viajes, Noticias(lista), Biblia(listas), Fotos, Juegos(lista) | esos archivos | B0, A2 |
| B3 | Visores: DictionaryDefinition, Noticias(artículo), Biblia(capítulo), Clima, Traductor, Voz, AskBook, ReminderAlert, Timer | esos archivos | B0, A2 |
| B4 | Juegos (9) + Laberinto + 2048 por movimiento | `src/activities/games/*` | B0, A2, A4 |
| B5 | Ajustes propios, DevicePair, AudioTest, HubSync/Location/AssetSync, Música (retoque a tokens) | esos archivos | B0, A2 |
| B6 | Conversor de unidades | nuevo + mosaico | B0, B1 |
| C | Verificación: build, revisión adversarial, simulaciones, release, docs | — | todo |

Regla de convivencia: un archivo, un dueño por ola. `main.cpp` lo tocan A1 y A4 → **secuencial** (A1 primero). Los
yaml de i18n sólo por `scripts/add_i18n.py`. Build sólo por `/tmp/claude-0/pio-build.sh` (flock).
