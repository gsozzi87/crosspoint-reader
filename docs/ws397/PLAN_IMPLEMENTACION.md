# Plan de implementación paso a paso — ws397 para vender

Sale de `docs/ws397/PLAN_VENTA.md` (el qué y el por qué) más las decisiones que tomó el usuario el
12/09/2026 y un reconocimiento de siete lectores sobre el código real. Esto es el **cómo**, en orden.

Punto de partida: **1.5.70**. Maquetas en `docs/ws397/maquetas/`, generadores en `tools/maquetas/`.

## Decisiones cerradas

| Tema | Decisión |
|---|---|
| Tema visual | **Diario v2 por defecto** |
| Opciones de interfaz | Quedan **Lyra** (el de siempre) y **los nuevos**. Clásico, Lyra Extendido y RoundedRaff se van |
| Deep sleep | Lo **mínimo** posible de consumo |
| Despertar | **Siempre un botón.** Nunca el IMU |
| Pantalla dormido | Dice **SUSPENDIDO** o **APAGADO** según el caso, con información y **titulares** |
| Modo de energía | **No vuelve** el selector. Lo fijo yo |
| Fotos | Fuera del producto |
| Apps de Lua | A la **etapa final** |
| Noticias | El servidor mastica; el aparato baja el paquete entero cuando se conecta; refresco horario |

---

## Dos hallazgos que cambian el plan

### 1. El tema NO manda en nuestra interfaz

Verificado: existe `UITheme` (macro `GUI`) con cuatro temas registrados y el default es Lyra — de ahí
el `[UI] Using Lyra theme` del log. Pero **las pantallas nuestras no lo usan**. HubActivity,
AgendaActivity, NotesActivity, Noticias, Biblia, Calendario, Viajes y los juegos dibujan con
`src/activities/ListStyle.h` (`listui`, constantes fijas: margen 24, filas de 48/72) y con
`src/components/Selection.h`. Del tema consumen tres cosas y nada más: `GUI.drawHeader`,
`GUI.drawButtonHints` y cuatro números (`topPadding`, `headerHeight`, `buttonHintsHeight`,
`verticalSpacing`).

O sea: **poner Diario v2 como tema default cambiaría el lector y la home clásica, y dejaría el hub
exactamente igual.** Para que Diario v2 sea de verdad el aspecto del aparato hay que hacer dos cosas,
y la segunda es la grande:

- **(a)** Registrar Diario como tema (barato: una tabla de métricas y el registro en cinco lugares;
  un tema nuevo cuesta entre 1,6 y 4,4 KB de flash).
- **(b)** Hacer que `listui` y `Selection` **salgan del tema** en vez de ser constantes. Eso es lo que
  convierte un tema en el aspecto del aparato entero.

### 2. El botón que despierta no se puede elegir: sólo hay uno posible

En el ESP32-S3 los RTC GPIO son **0..21**. GPIO38 (la IRQ del PMIC, o sea PWR) y GPIO45 (el INT del
RTC) **quedan afuera**, así que ninguno de los dos puede despertar del deep sleep. (El comentario
"38 is an RTC GPIO" de `BoardConfig.h:1280` es del perfil M5PAPER_V11, otra placa; el perfil ws397
dice lo contrario en la línea 1847.)

Lo que sí son RTC GPIO: **BOOT (0), ARRIBA (4), OK (5), ABAJO (6)**. BOOT no se usa porque es strap
de arranque. Decisión:

- **SUSPENDIDO → despierta OK.** Sólo OK: si despertara la palanca, el aparato en la mochila se
  encendería solo toda la noche. Es deliberado y la propia pantalla lo dice.
- **APAGADO → enciende PWR mantenido 1 s** (PressOn del AXP2101, que es el único que funciona sin
  ESP).

Cada pantalla dice cuál es su botón, así que no hay nada que adivinar.

---

## Ola 1 — 1.5.71 · Los bugs que ya están reportados

Sin dependencias. Es lo primero porque son todas molestias visibles.

1. **La barrita del PWR carga parejo.** Hoy se dibuja dos veces (1200 ms al 40 %, 2300 ms al 77 %) y
   nunca llega al 100 %. Repintar **sólo el rectángulo de la barra** cada ~200 ms con un parcial de
   la región (la caja y el texto ya están en pantalla), de 1200 a 3000 ms = 9 parciales chicos.
   `src/main.cpp` → `drawPowerHoldBanner()` y `handlePowerHold()`.
   *Listo cuando:* la barra se llena de 0 a 100 sin escalones y soltar a mitad suspende.
2. **Viajes deja de reconocer un viaje borrado.** Tres arreglos:
   - `server/src/trips.ts` → `getTrip()`: no devolver `trips[0]` cuando el llamador pidió un id.
     `suggest.ts` línea 358 cae hoy en ese fallback.
   - `server/src/trips.ts` → `POST /api/trip/delete`: purgar las entradas `trip:<id>:*` de
     `/data/suggest.json`, igual que ya purga calendario y adjuntos.
   - `src/activities/home/TripActivity.cpp` → `fetchTrips()`: si `tripId` o `suggestTripId` no están
     en la lista nueva, limpiarlos y guardar la caché.
   *Listo cuando:* se borra un viaje en la web, se entra a Viajes y no aparece ni en la lista ni en
   las sugerencias, con y sin WiFi.
3. **El OTA repinta cada 10 %, no cada 2 %.** 50 refrescos → 10.
4. **Documentación al día.** `CLAUDE.md`: el modo de energía dejó de ser tres opciones en 1.5.61 (hoy
   está forzado y escondido), y por eso la red de seguridad de 30 minutos de `main.cpp` —que pregunta
   por `sleepTimeoutMs == 0`— **está muerta en la ws397**. `docs/ws397/FUNCIONES.md`: sacar los doce
   juegos compilados (se fueron en 1.5.63) y marcar los gestos del IMU como hechos.

---

## Ola 2 — 1.5.72 · Energía: el reposo que hoy no reposa

Depende de nada. Es la ola que decide si el producto se puede vender.

5. **Fuera el despertar por movimiento.** El usuario decidió que siempre se aprieta un botón. Sacar
   `motionMoved()`, `WAKE_DELTA_G`, `haveSample_`, `lastX_/Y_/N_` y `Woke::Motion` de
   `src/util/IdleSleep.{h,cpp}`, y el manejo de `Woke::Motion` en `main.cpp`.
   Efecto inmediato: **desaparece el ciclo de 2 s**. El light sleep pasa a ser *indefinido* —
   sin timer— y despierta por los cuatro botones, la IRQ del PMIC (GPIO38) y el INT del RTC (GPIO45),
   que en light sleep **sí sirven** aunque no sean RTC GPIO. Es el reposo más profundo que se puede
   tener, y de paso se muere el bucle `despertó por movimiento tras 2000 ms` del log.
   *Listo cuando:* el log muestra `a reposar` y la línea siguiente es de cuando se apretó un botón,
   minutos u horas después.
6. **La escalera de energía, fijada.** Sin selector. Los números los fijo así:
   - **30 s** quieto → light sleep indefinido (antes 45 s; se puede bajar porque ya no cuesta nada
     entrar y salir: vuelve en menos de 10 ms y la pantalla no se toca).
   - **10 min** quieto → deep sleep. Diez y no cinco a propósito: del light sleep se vuelve
     instantáneo y del deep sleep se vuelve con un arranque entero, así que conviene ser perezoso
     para bajar. El ahorro marginal entre light y deep es chico; la molestia de arrancar, no.
   - Se saca el ajuste de la web y del aparato; el número crudo queda como constante.
7. **Deep sleep al mínimo.** Nada de esto se está haciendo hoy (verificado: no hay un solo
   `esp_sleep_pd_config()` en todo el árbol):
   - **El IMU.** Los tres `sleepNow()` del `setup()` (líneas 1057, 1082, 1131) duermen con el QMI8658
     muestreando a **250 Hz**. Mandarlo a suspend en todos los caminos de sueño. Es el ahorro más
     grande de la lista.
   - **El códec.** `AudioManager::powerDown()` existe y **no lo llama nadie**. ES8311 a standby y el
     enable del amplificador (GPIO39) a nivel bajo antes de dormir — hoy queda flotando.
   - **Dominios de energía**: `esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, OFF)` (el PSRAM no hace
     falta retenerlo) y probar `RTC_PERIPH` en OFF. *Ojo:* ext1 puede necesitar RTC_PERIPH; si al
     apagarlo OK deja de despertar, se deja prendido y se anota el costo.
   - **Pines sin aislar**: `esp_sleep_config_gpio_isolate()` del SDK sólo toca 0..21, así que
     38, 39, 41, 42, 45, 47 y 48 quedan sueltos. Dejarlos en un estado definido antes de dormir.
   - **Rieles del PMIC**: apagar los que no hagan falta. Esto **contradice la regla de CLAUDE.md**
     de no tocar rieles, así que va con cuidado, sólo para el sueño y con vuelta atrás al arrancar:
     apagar el riel equivocado deja el aparato muerto hasta un PWR largo. Va último y se mide antes
     y después.
   *Listo cuando:* se mide el antes y el después y el número está escrito en `docs/ws397/`.
8. **Medir.** Con `BatteryLog` ya hecho, correr las cuatro pruebas: reposo puro 48 h, lectura
   continua, uso normal 3 días, y **suspendido una semana** (que nunca se midió). Son los números
   que van en la caja.

---

## Ola 3 — 1.5.73 · Fondo de pantalla con información, y las fotos afuera

Depende de la Ola 2 (el camino de sueño se toca en las dos). Maqueta:
`docs/ws397/maquetas/fondo.png`, generador `tools/maquetas/fondo.py`.

9. **Rescatar lo que las fotos se llevan puesto.** `PhotosActivity::drawFullScreenPhoto()` es estática
   y la usan **TripActivity** (la página del adjunto) y **main.cpp** (`paintWallpaperForSleep()`).
   Mudarla a `src/util/FullScreenBmp.{h,cpp}` ANTES de borrar nada. En el servidor pasa lo mismo:
   `toDeviceBmp()` y `bmpToPng()` viven en `photos.ts` pero los importan `attachments.ts` y
   `board.ts` → mudarlos a `server/src/deviceBmp.ts`.
   *Honestidad sobre el ahorro:* sacar las fotos son ~12-13 KB de flash sobre 5,71 MB (dos décimas de
   punto), cero RAM estática, y `sharp` **no** se puede sacar del Docker porque lo usan los adjuntos y
   el paquete de contenido. Se hace porque simplifica el producto, no porque ahorre.
10. **Borrar las fotos.** `PhotosActivity.{h,cpp}`, la entrada Ajustes → Sistema → Fondo de pantalla
    (`SettingAction::Wallpaper`), `server/src/photos.ts` (los routes), la pantalla Fotos de
    `/board`, los `STR_PHOTO*` de los siete yaml y el ícono `hub_photos` (que ya es código muerto:
    cero apariciones en `firmware.map`). **El enum `Tile` no se toca**: las fotos dejaron de ser
    mosaico en 1.5.39, el `static_assert(== 13)` sigue bien y no se corre ningún índice.
11. **El renderizador del fondo.** Nuevo `src/activities/home/SleepScreen.{h,cpp}`, llamado desde
    `paintWallpaperForSleep()`. Reglas:
    - **Una sola pintura.** Hoy, con foto puesta, se pagan DOS pantallas completas antes de dormir
      (la `SleepActivity` del SDK y encima la foto). El fondo nuevo reemplaza a las dos: se saca el
      `goToSleep()` → `SleepActivity` del camino de la ws397.
    - **La hora es un sello, no un reloj.** Dice cuándo se pintó. Lo que va en dígitos grandes es la
      **próxima alarma**, que es el único dato que sigue siendo cierto con el sistema muerto.
    - **Cuatro estados**: suspendido, apagado, suspendido leyendo, y sin sincronizar.
    - **Los titulares entran los que entren**: se mide de arriba a abajo y se corta limpio, nunca a
      mitad de renglón. El primero lleva bajada, los demás van pelados, y al final "y N titulares más
      en Noticias".
    - **Leer antes de desmontar.** `Storage.prepareForDeepSleep()` desmonta la tarjeta; los titulares
      salen de `/.crosspoint/news/`, así que se leen ANTES. Todo lo demás (clima, recordatorios,
      libro, batería, RTC, SHTC3) ya está en RAM en ese momento.
12. **El apagado pinta.** Hoy `powerOffNow()` llama a `paintWallpaperForSleep()` y esa función
    **vuelve temprano si no hay foto elegida**, así que apagar deja en pantalla lo que hubiera con la
    barrita encima. Con el fondo nuevo siempre hay algo que pintar: queda la pantalla APAGADO.

---

## Ola 4 — 1.5.74 · Noticias masticadas y una sola sincronización

Las dos cosas van juntas porque el paquete de noticias viaja en la sincronización.

13. **El masticador, en el servidor.** `server/src/news.ts` nuevo, al lado de `rss.ts` (que hoy es
    100 % bajo demanda y sin LLM, con memoización de 30 min en un `Map` de proceso).
    - Un **temporizador en el proceso Bun** (`setInterval` horario + una pasada al arrancar) recorre
      las cuentas con feeds y arma el paquete. Sobrevive al redeploy porque el paquete se **persiste**
      en `/data/news-<cuenta>.json` y al arrancar se refresca si está vencido.
    - Por artículo: se baja con `extractArticle()` (el limpiador que ya existe, sin LLM) y **se
      mastica con el modelo** sólo los ~12 más nuevos de la hora: titular, tres a cinco frases de
      resumen y el cuerpo recortado. El resto quedan titular + extracto, gratis.
    - **Tope de gasto**: entrada propia en `METERED` y `addUsage` a mano, como hace `suggest.ts`.
      Ninguna ruta de noticias figura hoy en `METERED`.
14. **El paquete, en el aparato.** `GET /api/news/pack` devuelve un **manifiesto** (~4 KB: por
    artículo id, medio, hora, titular y sha) y el aparato baja los que le faltan uno por uno a
    `/.crosspoint/news/<id>.txt`. **No** se baja un JSON de 100 KB de una: `ServerClient` no tiene
    streaming, copia el cuerpo dos veces, y `SDCardManager::readFile()` **corta a 50 KB** — un
    archivo más grande que eso no se vuelve a leer nunca. El manifiesto también alimenta los
    titulares del fondo de pantalla.
15. **Subir todo / bajar todo lo cambiado.** Hoy `HubSyncActivity` es la única sincronización de
    verdad y sólo se abre desde el hub. La subida está medio resuelta (`ServerClient::request()`
    llama a `flushOnConnect()` en la primera petición de cada sesión de red, así que cualquier
    pantalla que levante WiFi vacía la cola); lo que **ninguna** hace es bajar. Trece o catorce
    Activities levantan WiFi y sólo dos tocan el hub.
    - `SERVER_CLIENT.syncIfDue()` nuevo, llamado desde cualquier Activity que levante WiFi: vacía la
      cola, pide `GET /api/hub`, pide el manifiesto de noticias y baja lo que falte.
    - **Lo del aparato sube.** Hoy `settings.rev` va en un solo sentido: lo que se cambia en el
      aparato (volumen, gestos, lugar de la Biblia, modo de voz) **nunca sube**. Agregar el camino de
      vuelta con la misma regla de `rev`.
    - **Atómico.** Hoy son cuatro pasos independientes y cortarse a la mitad deja media cuenta vieja.
      Escribir a `.tmp` y reemplazar al final, con la maquinaria de F16 que ya existe.
    - Y de paso: **subir el tope de 50 KB de `readFile`** y barrer los `.tmp` al arrancar (era A5 del
      plan de venta).

---

## Ola 5 — 1.5.75 · Diario v2 de verdad

Depende de nada técnico, pero va después porque es la más larga y la que más se nota si se hace a
medias.

16. **Que el tema mande.** Sacar de `src/activities/ListStyle.h` y `src/components/Selection.h` las
    constantes fijas y hacer que salgan de `ThemeMetrics`. Sin esto, cambiar el tema cambia el lector
    y deja el hub igual (ver el hallazgo 1).
17. **Registrar Diario y los nuevos.** `DiarioTheme` con su tabla de métricas (el patrón de
    `Lyra3CoversMetrics` copia la de Lyra con un lambda constexpr y pisa dos campos: siete líneas), y
    el registro en los cinco lugares de siempre: enum, switch de `UITheme::setTheme`, lista de
    ajustes, clave de I18n y los yaml de los seis idiomas.
18. **La lista de opciones, podada.** Quedan **Diario** (por defecto), **Lyra** (el de siempre) y los
    otros que diseñé. Se van Clásico, Lyra Extendido y RoundedRaff. Migración: si el
    `settings.json` guardado apunta a un índice que ya no existe, cae en Diario.
    **Ojo con el nombre:** mi maqueta se llamaba "Lyra" y choca con el tema de upstream, que se
    queda. La mía pasa a llamarse **Riel** (por el rail de 56 px sobre el que está armada).
19. **Aplicar Diario a las pantallas nuestras**, con las maquetas de `docs/ws397/maquetas/` como
    referencia: masthead con doble regla, versalita espaciada para los antetítulos, serif para el
    contenido, puntos guía. Regla número uno de `DISENO.md` intacta: **nunca letras sobre trama**, y
    nada de pastillas negras macizas.

---

## Ola 6 — 1.5.76 · La web sin el cajón "Más"

20. **Seis pestañas**: Hoy · Agenda · Listas · Notas · Viajes · Ajustes. Viajes sube al primer nivel
    porque es lo único con contenido propio que se usa seguido; Fotos ya no existe.
21. **Ajustes deja de ser un cajón**: pantalla con secciones (Aparato, Voz y sonido, Clima, Noticias,
    Memoria, Cuenta) y un bloque **Avanzado** (IA, Contenido, Log) sólo para admin.
    `POST /api/board/settings` mergea campo por campo, así que partir el formulario **no toca el
    servidor**: es 95 % `app.js` + `index.html`.
22. **Desarmar la rama `mas`.** Todo el segundo nivel vive hoy adentro de `p[0] === "mas"`
    (app.js:1253-1271), el botón Atrás tiene `"#mas"` de default (1530) y el `+` se apaga con un
    `fab = false` suelto en cada rama. Hay que rehacer el despacho de `render()` (1236-1287) y la
    barra de `index.html:52-58`.
23. **Cerrar las excepciones a "un solo estado".** Hoy la cumplen Hoy, Agenda, Listas y Notas; Viajes
    (`tripCache`) y el Log (`logText`) tienen caché propia al margen de `S`, y de ahí salen las
    inconsistencias que ya se habían quejado.

---

## Ola 7 — 1.5.77 · Seguridad y reconstruibilidad

Es lo que no se ve y sin lo cual no se puede vender. Está detallado en `PLAN_VENTA.md` (A1, A2, A6).

24. **TLS de verdad (F15)**: raíces embebidas, `wolfSSL_check_domain_name` en `SecureClient` (hoy no
    se llama en ningún lado) y **el reloj en hora ANTES del primer TLS** — hoy nadie llama a
    `settimeofday` y un certificado se valida contra la fecha. Orden: RTC → SNTP → header `Date`.
25. **El hueco de permisos**: reproducir el registro público en una instancia limpia y ver qué rol
    queda. Si el primer usuario se hace admin, que sea explícito (`BOOTSTRAP_ADMIN_EMAIL`).
26. **Reconstruible**: publicar el commit del SDK en el fork, reapuntar el submódulo, regenerar
    **todos** los `.patch` de `docs/ws397/` (faltan tres) y un `build.sh` que compile desde cero.

---

## Ola 8 — 1.5.78 · Primer arranque y la tarjeta de fábrica

27. **La tarjeta sale armada**: `/fonts`, `/dictionaries` (español), `/Apps`, `/Music`, `/Books` y un
    libro de muestra. Hoy el log dice `Fonts directory not found` y `No /dictionaries directory`.
    Además, que el aparato **cree las carpetas que falten** en vez de loguear que no están.
28. **Asistente de primer arranque**: idioma → WiFi por el teléfono (ya está, 1.5.70) → vincular (ya
    está) → lugar del clima por voz (ya está) → **una pantalla que enseñe los tres gestos que no se
    adivinan**: doble Atrás = hablar, Atrás mantenido = sincronizar, PWR mantenido = suspender.
29. **Velocidad de lectura**: hoy 2,5 a 4,8 s por página con `display=2227ms` adentro. Medir qué
    forma de refresco elige el coordinador al pasar de página y por qué. Objetivo: menos de 1 s.

---

## Ola 9 — 1.6.0 · Apps de Lua y candidata de venta

30. **Apps de Lua** (el usuario las puso explícitamente al final): `cp.time()` para que una app sepa
    la hora, tres o cuatro apps de fábrica en `/Apps`, y la pantalla de Juegos abriendo directo a la
    lista.
31. **Las cuatro mediciones de batería** escritas, y el checklist de hardware de `PLAN_VENTA.md`
    completo.

---

## El IMU: para qué sirve de verdad

Lo que averiguamos de los aparatos que compartiste:

- **ZecTrix Note 4: no tiene IMU.** No es que no esté documentado — el wiki publica el mapa de pines
  completo y la tabla de I2C, y en el bus hay dos chips: el RTC PCF8563 (0x51) y el NFC NT3H (0x55).
  Los 24 GPIO están todos asignados. Confirmado además contra tres firmwares comunitarios. Así que
  **no hay nada que copiarle** por ese lado.
- **reTerminal Sticky: sí tiene** (LSM6DS3TR-C de 6 ejes en 0x6A) pero el firmware oficial casi no lo
  usa: sólo pone la pantalla en vertical u horizontal según cómo la apoyes. Sin despertar por
  movimiento, sin sacudir, sin doble golpe. Lo interesante está en firmwares de terceros:
  **tilt-to-turn-page** (apagado de fábrica), **auto-reposo por movimiento** con umbrales en mg, y
  sacudida sostenida como selector.
- Los hermanos reTerminal E1001/E1002 **no tienen IMU**: sólo temperatura y humedad.

Conclusión: ya tenemos más gestos que los dos aparatos de referencia juntos. Lo que falta no es
agregar gestos, es que los que hay no molesten. Propuestas, todas **sin despertar el aparato**:

1. **Inclinar para pasar de página** en el lector, **apagado de fábrica**. Es el único gesto que los
   demás aparatos implementan y la gente usa.
2. **Levantarlo alarga la vigilia**: si está despierto y se mueve, no entra al reposo todavía. No
   despierta nada; sólo evita que se duerma en la mano. Es lo contrario del auto-reposo de Followup y
   es gratis, porque el IMU ya se consulta mientras está despierto.
3. **Inclinar a los costados = saltar de a diez** en listas largas (Biblia, Noticias, Música).
4. Se quedan los tres globales de siempre: boca abajo calla, sacudir cancela, doble golpe habla.
5. **Mientras duerme el IMU está apagado** (Ola 2, paso 7). Es a la vez la decisión de energía y la
   garantía de que nunca se enciende solo en la mochila.

---

## Riesgos anotados

- **Apagar rieles del PMIC** (paso 7) puede dejar el aparato muerto hasta un PWR largo. Va último,
  se mide antes y después, y contradice una regla de `CLAUDE.md` a propósito y sólo para el sueño.
- **`RTC_PERIPH` en OFF** puede romper el despertar por ext1. Si OK deja de despertar, se deja
  prendido.
- **El tope de 50 KB de `readFile`** es un techo duro para el paquete de noticias y para la caché del
  hub: si no se sube en la Ola 4, la caché se pierde entera en silencio.
- **Diario v2 a medias** (registrar el tema sin hacer el paso 16) deja el lector con un aspecto y el
  hub con otro. Es peor que no hacer nada.
- El masticador de noticias es el primer trabajo de fondo del servidor que gasta modelo **sin que
  nadie lo pida**. Sin el tope de `METERED` puede comerse el presupuesto de un mes en una noche.
