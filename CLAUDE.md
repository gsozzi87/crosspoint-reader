# CrossPoint port — Waveshare ESP32-S3-ePaper-3.97 ("ws397")

Fork de [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) con soporte para la placa
Waveshare ESP32-S3-ePaper-3.97 (SSD1677 800x480, ESP32-S3-WROOM-1-N16R8, ES8311 + NS4150B, PMIC AXP2101-compatible,
RTC PCF85063, IMU QMI8658, SD 4-bit). El submódulo `freeink-sdk/` apunta al fork propio con el perfil de placa.

Idioma con el usuario: español rioplatense/mexicano, informal y directo. Respuestas cortas. Él prueba en hardware
y devuelve correcciones puntuales; no pedir que especifique todo de antemano.

## Estado (2026-09-04)

Funciona: boot, pantalla (orientación y polaridad correctas), botones, SD, WiFi, web UI, deep sleep,
batería vía PMIC, RTC, OTA desde servidor propio.

Pendiente de verificar en hardware: refresco periódico de un solo destello (parche `halfrefresh`), porcentaje de
batería real, hora tras apagado sin WiFi.

Pendiente de implementar (Fase 0): validar audio (ES8311 mic + parlante), trackball + 2 botones vía PCF8574 en I²C
(SDA 41 / SCL 42, INT GPIO44) cuando llegue el hardware.

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
- Audio: ES8311 en 0x18 (I²S bclk 14, ws 47, dout 48, mclk 13), amp enable GPIO39 (compartido con IMU INT1;
  IMU va por polling). Mic es I²S vía ES8311, no PDM.
- Sensores: SHTC3 en 0x70 (sin driver aún), QMI8658 en 0x6B.

## Roadmap acordado

0. Hardware: audio, trackball/botones, deep sleep medido.
1. Hub de apps (base CrossMux, MIT) + "preguntarle al libro" (texto del EPUB → servidor → Claude) + cliente HTTP
   común con token, reintentos y cola offline.
2. Asistente por voz PTT (mic → Whisper → Claude → TTS → parlante), recordatorios y listas (RTC + servidor),
   traductor, lectura en voz alta.
3. Biblia con índice invertido pregenerado en SD (búsqueda en ms, sin WiFi), reproductor MP3 desde SD y radio por
   streaming (`ESP32-audioI2S`, tarea propia en core 1), dashboard Casa Cerebro, RSS/lectura web.
4. Juegos (cartas, sudoku). Spotify descartado.

Principios: CrossPoint sigue siendo el lector; lo nuestro entra como Activities en el hub. Todo lo pesado (STT,
LLM, TTS, render) en el servidor. Audio y red en tareas FreeRTOS separadas de la UI. El aparato nunca guarda
claves de Anthropic; habla con el Hono con un token propio.

## Convenciones

- Commits: prefijo `ws397:`. Cambios al SDK en el submódulo, con su propio commit.
- Los commits ws397 del SDK (perfil, waveform, battery, rtc, wake, halfrefresh) están exportados como `.patch` en
  `docs/ws397/` (`git format-patch`); si al submódulo le falta alguno, `git am docs/ws397/NNNN-*.patch` dentro de
  `freeink-sdk/`. Regenerarlos cuando se agregue un commit al SDK.
- No tocar la lógica upstream fuera de lo necesario para la placa; preferir `case Board::WS397` sobre `#if`.
- Antes de un release: `pio run -e ws397` limpio y probar en hardware.
