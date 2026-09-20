# AI Review & Execution Protocol — WS397

This file is the shared, persistent working memory between AI agents reviewing and improving the WS397 product.

It is intentionally stored in the repository so that separate AI sessions can coordinate without relying on chat memory.

## Roles

### Human owner
Gastón is the product owner and final authority. He decides product behavior, priorities, releases and whether a disputed theory should be pursued further.

### Reviewer agent
Primary role: independent reviewer, adversarial auditor and regression hunter.

Responsibilities:
- inspect the current HEAD of branch ws397 before every review;
- identify possible bugs, regressions, race conditions, unsafe assumptions, memory risks and UX mismatches;
- produce evidence-based hypotheses with exact file paths and current line references when possible;
- distinguish confirmed bugs from probable bugs and hardware-only risks;
- review the executor's implementation after changes;
- challenge fixes that only mask symptoms;
- never silently convert a hypothesis into a fact;
- never publish an OTA.

The reviewer SHOULD NOT be the main implementation agent unless Gastón explicitly asks it to implement a specific change.

### Executor agent
Primary role: reproduce, validate/refute and implement.

Responsibilities:
- read this file and AGENTS.md before touching code;
- inspect the current HEAD of ws397 and never work from stale assumptions;
- take reviewer hypotheses one by one;
- either CONFIRM, REFUTE or mark NEEDS_HARDWARE with technical evidence;
- implement confirmed fixes with the smallest safe change;
- add or update regression tests whenever practical;
- run the relevant build/tests;
- write its result back into this file;
- never “fix” an item merely to agree with the reviewer;
- if the reviewer is wrong, explicitly refute the theory and explain why.

The executor is expected to disagree when the code proves a review theory wrong.

## Core rule: this file is the durable memory

Every agent working on ws397 must treat this file as the persistent inter-agent memory.

At session start:

1. Read AGENTS.md completely.
2. Read AI_REVIEW_PROTOCOL.md completely.
3. Read the current HEAD SHA of ws397.
4. Read recent commits that may overlap the issue being worked on.
5. Re-check the actual source before trusting old line numbers or old conclusions.
6. Update the Session Log below when meaningful work is completed.

Chat memory is secondary. Repository state and this file are authoritative.

## Concurrency protocol

Several agents may modify ws397 at the same time.

- Never assume the branch remained unchanged while you were working.
- Before editing a file, re-fetch the current version.
- Before writing this protocol file, re-fetch it first.
- Do not erase another agent's findings, refutations or verification notes.
- Prefer appending a dated note to rewriting history.
- If a finding has become obsolete because another commit changed the code, mark it SUPERSEDED and cite the commit.
- Never revert another agent's recent fix just because it differs from the implementation you expected.
- If two changes conflict, stop and report the conflict instead of force-pushing.
- Do not publish OTA automatically.

## Finding lifecycle

Each finding should use one of these states:

- OPEN: reviewer hypothesis, not yet validated.
- CLAIMED: executor is actively investigating it.
- CONFIRMED: reproduced or proven directly from code.
- REFUTED: theory was tested/read and shown not to be a bug.
- NEEDS_HARDWARE: code review is insufficient; requires device measurement.
- FIXED_PENDING_REVIEW: executor changed code and tests passed; reviewer has not re-reviewed.
- VERIFIED: reviewer inspected the fix and/or regression test and accepts it.
- DEFERRED: real issue intentionally postponed by Gastón.
- SUPERSEDED: no longer applicable because another commit changed the relevant code.

A finding is NOT closed merely because code was changed. It is closed only as VERIFIED, REFUTED, DEFERRED or SUPERSEDED.

## Required format for every finding

ID: REV-xxx
State:
Severity: P0 / P1 / P2 / P3
Subsystem:
Reviewer theory:
Evidence:
Reproduction:
Expected behavior:
Executor response:
Fix:
Tests:
Reviewer final check:
Commit(s):

Severity:
- P0: corruption, crash/reset, brick/OTA, data loss, security or cross-account contamination.
- P1: main feature broken, deadlock, device stuck or important alert silently lost.
- P2: incorrect behavior or serious UX regression.
- P3: cosmetic, logging, maintainability or low-impact optimization.

## Disagreement protocol

A REFUTATION is a valid successful outcome.

If the executor disputes a review theory, it must write:
- what assumption was wrong;
- what the code actually does;
- evidence from source/tests/hardware;
- whether any smaller nearby issue remains.

The reviewer then re-checks the refutation. If correct, mark VERIFIED/REFUTED. If not, reopen with new evidence.

No agent should “win” an argument. The repository and reproducible behavior decide.

## Release gate

No OTA should be considered ready until:
- ws397 builds successfully;
- relevant unit/desktop tests pass;
- server typecheck passes when server code changed;
- CI is green or a documented infrastructure-only failure is understood;
- version number and binary served by /firmware/latest agree;
- high-severity findings changed by the release are VERIFIED;
- human hardware checks required by NEEDS_HARDWARE items are explicitly listed for Gastón.

---

# Initial reviewer audit queue

The following queue captures the independent review performed against branch ws397 around firmware 1.5.118-ws397. Line numbers may move; always inspect current HEAD.

## REV-001 — CI on ws397 is red
State: VERIFIED
Severity: P0 release gate
Subsystem: CI / release

Recent ws397 pushes were observed with failing GitHub Actions runs. Determine exactly which jobs fail and why. The workflow is .github/workflows/ci.yml and includes clang-format, cppcheck, multi-board builds including ws397, unit tests, ws397 desktop tests and server TypeScript typecheck.

Do not publish OTA while ws397 CI is red.

Minimum validation:
- pio run -e ws397
- relevant test scripts under test/
- test/news_pack
- test/lua_sandbox
- test/battery_drain
- SSRF tests
- libros tests
- server tsc --noEmit
- confirm firmware version in source equals firmware actually served by the deployment.

Executor response: CONFIRMED — y la causa no es ningún test: **el workflow no parsea**.

Evidencia (HEAD 2a47b1c):
- Los ocho runs más recientes de ci.yml en ws397 terminan en `failure`, incluido el de un commit
  que sólo toca documentación (`docs: add AI review and execution protocol`). Un commit de docs que
  falla ya dice que no es una regresión de código.
- `GET /actions/runs/35474683436/jobs` devuelve `total_count: 0` con `conclusion: failure` y
  `created_at == run_started_at == updated_at` (mismo segundo): el run murió antes de crear un solo job.
- `python3 -c "yaml.safe_load(open('.github/workflows/ci.yml'))"` ->
  `mapping values are not allowed here, line 245, column 21`.
- Línea 245: `      - name: Libros: lo que dice el bot`. Un escalar YAML sin comillas no puede llevar
  `": "`; el parser lo lee como clave. Es el único `name:` del repo con dos puntos.
- Introducido en `c9f2f3d` ("ws397: Libros — app de Lua que le pide libros a un bot de Telegram",
  2026-09-18). **Desde entonces hay 65 commits en ws397 y CI no corrió ni una vez.** Todo lo que los
  mensajes de commit de ese tramo dicen haber probado, lo probó el ejecutor a mano, nunca CI.

Fix:
- Comillas en esa línea. `yaml.safe_load` pasa y los siete jobs aparecen
  (clang-format, cppcheck, build, unit-tests, ws397-tests, server, test-status).
- De paso, `ws397-tests` corría 6 de las 11 suites de escritorio que existen. Se agregaron las cinco
  que faltaban (xtc_geometry, idempotency, device_log, net_lookup, epub), porque ahora CI va a correr
  de verdad y esas suites existían sin que las mirara nadie.

Tests: las once suites corren en verde localmente (`lua_sandbox, battery_drain, news_pack,
register_admin, ssrf, libros, xtc_geometry, idempotency, device_log, net_lookup, epub`), `pio run -e
ws397` SUCCESS (RAM 28,6 %, Flash 87,6 %), `bunx tsc --noEmit` limpio.

OJO para el revisor: que el YAML ahora parsee NO quiere decir que CI vaya a dar verde. Es la primera
corrida real en 65 commits; clang-format y cppcheck pueden tener deuda acumulada. Eso se ve recién
cuando esto llegue a ws397 y hay que mirarlo antes de considerar el release gate abierto.
Reviewer final check:
VERIFIED por el revisor contra HEAD 727e620.
- El diagnóstico del ejecutor era correcto: el YAML roto impedía crear jobs; no era un fallo del firmware.
- Corrida 35478957781: doce jobs en verde, incluidos Build ws397, Build default, cppcheck,
  clang-format, unit-tests, Server typecheck y las once suites de escritorio.
- La corrida posterior del HEAD actual (35479537753) también terminó en success.
- Se acepta además ampliar ws397-tests con las suites que ya existían y no corrían.

Impacto de producto: esto no cambia una pantalla del aparato por sí solo. El fallo era que durante
65 commits no había red de seguridad automática: una regresión podía entrar aunque GitHub mostrara
simplemente un run muerto. Al arreglarlo se destaparon REV-042/043 inmediatamente.

## REV-002 — BatteryLog parses bool through int*
State: VERIFIED
Severity: P2
Subsystem: battery / memory safety

Inspect src/util/BatteryLog.cpp readAll().

Observed pattern parses CSV using sscanf and casts the address of Sample::charging, a bool, to int*. Writing %d through int* to bool storage is undefined behavior and may overwrite adjacent bytes. The time_t-to-long cast must also not be assumed safe on every target.

Parse into correctly typed temporaries and then assign to Sample.

Tests should include charging=0, charging=1, modern timestamps and post-2038 timestamps.

Executor response: CONFIRMED el problema, **REFUTADO el mecanismo** que describe la teoría.

Lo que la teoría dice y no es cierto: "may overwrite adjacent bytes". En este target no puede pasar.
Medido con el compilador del aparato (`xtensa-esp32s3-elf-g++`, static_assert):
`long`=4, `time_t`=8, `sizeof(Sample)`=24, `offsetof(charging)`=16. `charging` es el ÚLTIMO miembro y
lo sigue el relleno de la estructura, así que el `%d` de 4 bytes cae en 16..19, dentro del objeto.
No hay corrupción de memoria vecina.

Lo que SÍ es cierto, y es peor que teórico (reproductor de escritorio con el mismo `sscanf`):

    "1735689600,80,4000,2"    -> charging(bool)=2   byte=0x02
    "1735689600,80,4000,256"  -> charging(bool)=0   byte=0x00

1. Un `bool` que contiene 2 es una representación inválida: `r.charging ? 1 : 0` imprimió **2**, que
   para un bool es imposible. Todo `if (r.charging)` posterior es UB y el compilador puede plegarlo.
2. `charging=256` (byte bajo 0) se lee como **false**: una muestra CARGANDO entra a la ventana de
   descarga. Eso es exactamente lo único que `analyze()` no puede permitir — su contrato entero es
   "sin carga en el medio" — y el resultado es un número de autonomía inventado, que es el defecto
   que `test_drain.cpp` existe para prevenir.
3. El `%ld` sobre `time_t`: `long` son 4 bytes y `time_t` 8, así que llenaba MEDIO campo. Andaba de
   casualidad (NSDMI `epoch = 0` + little-endian) y no soporta fechas ≥ 2^31.
4. `readAll()` no tenía NINGUNA prueba: `test_drain.cpp` construye `Sample` a mano y nunca tocó el parseo.

Severidad: la teoría dice P0; el ejecutor propone **P2**. Sin corrupción de memoria posible, lo que
queda es un número de batería equivocado con un archivo dañado. El arreglo es trivial igual.

Fix: `batterylog::parseLine()` como función pura en `src/util/BatteryLog.h` (mismo patrón que
`analyze()`, para poder probarla sin placa): temporarios con el tipo que `sscanf` espera (`long long`,
`int`) y recién después se copian al `Sample`; `charging` es "distinto de cero". Los dos `snprintf` de
escritura pasan a `%lld` para no truncar después de 2038. `readAll()` llama a `parseLine()`.

Tests: 17 casos nuevos en `./test/battery_drain/run.sh` (25 -> 42 comprobaciones): charging 0/1/2/256/-1,
campo ausente, fecha de 2100 sin truncar, línea vacía/basura/a medias/epoch 0/negativo/puntero nulo, y
el caso que cierra el círculo — una muestra cargando bien parseada corta la ventana. TODO BIEN.
Reviewer final check:
VERIFIED. Se acepta la refutación parcial del ejecutor.
- En el layout real del S3, escribir cuatro bytes sobre `charging` no alcanzaba memoria vecina porque
  el bool es el último miembro y hay padding; mi mecanismo inicial de "pisar el objeto siguiente" fue
  demasiado fuerte.
- Seguía siendo UB real: representación inválida de bool, `256` convertido efectivamente en false y
  `time_t` de 64 bits leído mediante `long*` de 32 bits.
- `parseLine()` ahora usa temporarios compatibles con sscanf, normaliza charging y la escritura usa
  `%lld`. La suite battery_drain cubre 0/1/2/256/-1, entradas rotas y año 2100, y CI está verde.

Impacto de producto: en uso normal, el CSV generado por el propio aparato escribe 0/1, por lo que era
probable que el usuario no viera nada. Con un archivo dañado, editado o una fecha fuera de 32 bits,
Ajustes → Memoria podía calcular una descarga/autonomía falsa —por ejemplo incluir como descarga un
tramo en el que el aparato estaba cargando—. No era un crash P0 en este hardware.

## REV-003 — Unbounded file-size-driven allocations from SD
State: OPEN
Severity: P0/P1
Subsystem: SD / memory

Audit firmware-wide patterns such as:

    std::string raw;
    raw.resize(f.size());
    f.read(...);

Examples exist in News, Bible, Weather, Calendar and caches.

A corrupt FAT/file can report a pathological size. A large std::string/vector resize under -fno-exceptions can terminate the firmware or exhaust memory.

Every persisted/cache file should have a reasonable maximum before allocation. Regenerable oversized caches should be rejected and regenerated.

Also inspect Arduino String and vectors whose sizes originate from SD metadata.

Executor response:
Reviewer final check:

## REV-004 — News “Refresh” may not actually rebuild news
State: OPEN
Severity: P1
Subsystem: News

Inspect:
- src/activities/home/NewsActivity.cpp
- src/news/NewsPack.cpp
- server/src/news.ts

Verify exactly what manual Refresh does.

Correct semantics should be explicit:
1. force server rebuild through the intended refresh endpoint;
2. wait/poll if rebuild is asynchronous;
3. fetch the new pack;
4. update local headlines and article bodies.

If the button merely re-downloads an already stale pack, the UI is misleading.

Test adding a feed, removing a feed, enabling/disabling PubMed and then pressing Refresh.

Also confirm removed sources purge their old items from the pack.

Executor response:
Reviewer final check:

## REV-005 — PubMed/medical feed resilience
State: VERIFIED
Severity: P1
Subsystem: server news / medical

This area was changing concurrently around the initial audit. Re-read current HEAD.

Inspect:
- server/src/medical.ts
- server/src/news.ts
- server/src/rss.ts
- board web settings

Verify:
- PubMed is truly optional according to current configuration;
- medical feeds are not duplicated;
- RCT/guideline/meta-analysis/top-journal prioritization behaves as intended;
- PubMed failure cannot kill the whole refresh;
- failure handling cannot itself throw an unhandled rejection;
- NEWS_* environment limits reject NaN, negatives and absurd values;
- “Prepare now” cannot be spammed into uncontrolled AI rebuilds;
- web-visible pack and device pack agree.

Executor response: mixto. Se re-leyó el HEAD actual como pide la teoría.

REFUTADO (ya estaba bien):
- "PubMed is truly optional": sí. Desde `e6d7e17` es un feed más con la URL centinela `pubmed:`
  (`isMedicalFeed`); el interruptor es la lista de fuentes, no una variable de entorno. Verificado por
  curl: alta, rechazo del duplicado (`esa fuente ya está cargada`), `/api/board/state.medical.added` y borrado.
- "medical feeds are not duplicated": `POST /api/board/feed` deduplica la centinela.
- "PubMed failure cannot kill the whole refresh" y "failure handling cannot itself throw an unhandled
  rejection": ése ERA el defecto (`fetchPinned` escuchaba con `request.once("error")` y un segundo
  `error` sin oyente volteaba el proceso), y se arregló en `2e79156`. Verificado con el reproductor real:
  el refresco loguea `news feed: pubmed: Error…` y la pasada termina sola con el servidor vivo.
- "'Prepare now' cannot be spammed": ya hay guardia. `running: Map<accountId, Promise>` en news.ts:558;
  un segundo `refreshPack` de la misma cuenta devuelve la promesa en curso y no arranca otro rebuild.
  El Map se limpia en `.finally()`, así que tampoco crece (toca también REV-036).

CONFIRMADO:
- "NEWS_* environment limits reject NaN, negatives and absurd values": **no lo hacían**. `medical.ts`
  sí (`clampItems()` valida finito y recorta 1..MAX), pero `news.ts` tenía siete
  `Number(process.env.X ?? def)` crudos. Medido: `""`->0, `"abc"`->NaN, `"-5"`->-5, `"1e9"`->1e9.
  Con NaN toda comparación da false, así que una variable mal tipeada en Railway apagaba **en
  silencio y a la vez** el cupo por medio, el tope de bajadas y el de masticado; con 0 o negativo el
  paquete queda vacío; con 1e9 la pasada se queda sin freno de descargas.
  Severidad honesta: P2/P3 — es mala configuración del operador, no algo que alcance un usuario.

Fix: `envInt(name, def, min, max)` en news.ts (valida finito, trunca, recorta al rango y lo dice en el
log) aplicado a los siete topes.

Tests: caso nuevo en `./test/news_pack/run.sh` (pack.test.ts, 24 pruebas) con vacío, espacios, `abc`,
`NaN`, `Infinity`, negativo, cero, `1e9` y decimal.

Pendiente de esta teoría, NO investigado en esta tanda: "RCT/guideline/meta-analysis/top-journal
prioritization behaves as intended" (hay cobertura parcial en medical.test.ts) y "web-visible pack and
device pack agree".
Reviewer final check:
VERIFIED para los puntos originales.
- PubMed sólo se consulta si el feed `pubmed:` está en la lista y la web evita duplicarlo.
- La caída de PubMed queda contenida por feed; el error doble de socket ya fue corregido antes de esta
  tanda y el servidor no debe caer con la fuente.
- Los siete NEWS_* pasan ahora por `envInt()` con default y límites.
- La priorización clínica tiene prueba explícita: un NEJM RCT se ordena por encima de un RCT menor y
  una guía conserva su prioridad; el mecanismo de score coincide con el diseño declarado.
- La web y el aparato leen el MISMO objeto persistido: `/api/news/preview` y `/api/news/pack`
  llaman a `loadPack(account)`; difiere sólo qué campos exponen.
- `running` evita reconstrucciones simultáneas por cuenta.

Impacto de producto: una variable NEWS_* mal escrita en Railway podía dejar Noticias vacías, impedir
el masticado o quitar el freno de descargas sin una explicación visible. Eso queda acotado. PubMed
puede agregarse/quitarse como una fuente normal y los papers llegan traducidos con lenguaje técnico.

Queda un detalle cosmético separado en REV-045: la web todavía llama "resumidas" a todas las entradas
`chewed`, aunque en Medicina ese estado significa "traducida técnicamente", no resumida.

## REV-006 — Dictionary word navigation semantics
State: OPEN
Severity: P2
Subsystem: EPUB dictionary

Inspect src/activities/reader/DictionaryWordSelectActivity.cpp.

Current directional mapping observed during audit appeared to reduce ScreenLeft/ScreenUp to selected-- and ScreenRight/ScreenDown to selected++, while the activity already stores row and has closestInRow().

Validate whether this matches intended UX.

Potential desired behavior:
- left/right: previous/next word on same line;
- up/down: previous/next row while preserving approximate X;
- two-direction physical lever: optionally retain linear reading-order traversal.

Test:
- normal EPUB;
- multiple columns;
- tables;
- ruby;
- different font sizes;
- all orientations;
- touch hitboxes;
- curly punctuation, em dash, accents, combining marks and non-Latin scripts.

Executor response:
Reviewer final check:

## REV-007 — Large StarDict behavior
State: OPEN
Severity: P1/P2
Subsystem: dictionary

Inspect:
- src/util/Dictionary.cpp
- src/util/DictionaryLookup.cpp
- src/util/DictZip.cpp
- src/util/DictionaryRegistry.cpp

Test:
- .idx + .dict
- .idx + .dict.dz
- .syn
- .qidx/.sidx
- corrupt sidecar
- interrupted index build
- slow SD
- low-memory conditions

Index build yields periodically, but validate whether the Activity remains cancelable and responsive for a very large dictionary.

Executor response:
Reviewer final check:

## REV-008 — Possible UTF-8 split in TXT reader
State: OPEN
Severity: P1
Subsystem: TXT reader

Inspect src/activities/reader/TxtReaderActivity.cpp line-wrapping fallback.

During audit, if no character fits, breakPos can reach zero and then be forced to 1. If the first codepoint is multibyte, that can split UTF-8.

Create a test with a narrow viewport or huge font and a first character using 2-, 3- and 4-byte UTF-8, including CJK/emoji.

Fallback must advance by one complete codepoint, never one byte.

Executor response:
Reviewer final check:

## REV-009 — Potential XTC page-buffer OOB for odd dimensions
State: VERIFIED
Severity: P0/P1
Subsystem: XTC reader

Inspect src/activities/reader/XtcReaderActivity.cpp.

For bitDepth 2, allocation was observed to use approximately ceil(width*height/8) per plane while pixel access appears column-major using:

    colBytes = ceil(height/8)
    offset = column * colBytes + byteInColumn

For height not divisible by 8, width * ceil(height/8) can exceed ceil(width*height/8).

Normal device dimensions may hide this, but malformed or unusual XTC dimensions could read out of bounds.

Validate dimensions and fuzz odd widths/heights.

Executor response: CONFIRMED. La aritmética de la teoría es correcta y se verificó exacto.

Evidencia (HEAD 2a47b1c):
- `src/activities/reader/XtcReaderActivity.cpp` reservaba `((w*h+7)/8)*2` y accedía column-major con
  `colBytes = (h+7)/8`, `byteOffset = colIndex*colBytes + y/8`, con `plane2 = pageBuffer + planeSize`.
- Byte más alto tocado = `planeSize + (w-1)*colBytes + (h-1)/8`. Calculado:

      800x480 -> malloc 96000, último byte 95999   ok  (480 % 8 == 0: por eso no se veía)
      800x479 -> malloc 95800, último byte 95899   OOB +100
      800x477 -> malloc 95400, último byte 95699   OOB +300
      600x300 -> malloc 45000, último byte 45299   OOB +300
      100x4   -> malloc   100, último byte   149   OOB +50

- Las dimensiones salen SIN validar del archivo: `XtcParser::validatePageTable()` copia
  `entry.width/height` del primer registro de la tabla de páginas a `m_defaultWidth/Height` y no
  comprueba nada. O sea que un `.xtc` corrupto o hecho a mano las controla.
- El formato y el render no coinciden: el parser declara el plano como `ceil(w*h/8)` (XtcParser.cpp:446)
  y el render lo indexa con columnas alineadas a byte. Sólo dan lo mismo con `h % 8 == 0`.

Por qué NO se cambió el indexado: con `h % 8 == 0` las dos fórmulas coinciden EXACTAMENTE, así que
ningún archivo que hoy se dibuja bien permite distinguir cuál de las dos es el formato de verdad. XTC
es formato de upstream (X4) y no hay un XTH con altura impar para decidirlo. Reinterpretar los bits a
ciegas rompería todos los archivos que hoy andan.

Fix (el más chico y seguro): validar y negarse, en vez de adivinar el formato o leer fuera.
`xtc::planeBytes()`, `xtc::planeBytesIndexed()` y `xtc::canRender2Bit()` en `lib/Xtc/Xtc/XtcTypes.h`
(puras, comparan los dos tamaños); `renderPage()` rechaza la página con `STR_PAGE_LOAD_ERROR` y una
línea de log si no se puede indexar. No hay OOB, no hay basura dibujada en silencio, y lo que hoy se
ve bien se sigue viendo igual.

Tests: `./test/xtc_geometry/run.sh` nuevo. Incluye el barrido exhaustivo (w 1..96, h 1..520) que
comprueba el invariante de verdad: la guardia acepta exactamente lo que no se sale del buffer.
**Ese barrido corrigió al propio ejecutor**: la regla NO es "alto múltiplo de 8" — con anchos de 1 o 2
píxeles los dos redondeos coinciden y la página es segura igual. Por eso la guardia compara los dos
tamaños y no mira el resto.
Reviewer final check:
VERIFIED.
- La guardia corre ANTES de reservar/leer la página 2-bit y compara el tamaño que declara el formato
  con el tamaño que realmente puede indexar el renderer.
- No intenta adivinar una variante del formato; rechaza sólo geometrías cuya indexación saldría del
  buffer. Es la decisión conservadora correcta.
- `test/xtc_geometry` barre combinaciones y compara la guardia contra el byte máximo que tocaría el
  renderer; CI lo ejecuta y está verde.

Impacto de producto: con los XTC normales de 800x480 probablemente nunca se notaba. Un XTC corrupto o
creado con una altura problemática podía leer cientos de bytes fuera del buffer: página basura,
crash/reset o comportamiento impredecible. Ahora esa página muestra error de carga en vez de arriesgar
memoria.

## REV-010 — EPUB incremental build loops may still monopolize main loop
State: OPEN
Severity: P1
Subsystem: EPUB

Inspect src/activities/reader/EpubReaderActivity.cpp.

There are outer while loops repeatedly calling section->buildSomeMore() until a distant page/anchor/offset becomes available.

Although work is chunked internally, an outer loop may still remain inside one Activity call for a long time.

Test huge chapters and pathological EPUBs while pressing Back/PWR. Verify watchdog, cancellation and visible progress.

Also audit grayscale paths:
- two PSRAM planes;
- strip fallback;
- OOM path;
- cleanupGrayscaleWithFrameBuffer() on every relevant branch;
- orientation/font change while pagination is incomplete;
- ghosting after error fallback.

Executor response:
Reviewer final check:

## REV-011 — Finished-book move/rename collision handling
State: OPEN
Severity: P2
Subsystem: EPUB storage

Review logic that moves completed books and renames associated cache.

Test:
- duplicate book names;
- maximum suffix collision count;
- 100+ collisions;
- power loss between moving book and moving cache.

Never overwrite an existing book/cache accidentally or bind one book to another book's cache.

Executor response:
Reviewer final check:

## REV-012 — Timer/alarm/reminder full state matrix
State: OPEN
Severity: P1
Subsystem: timers / wake / alerts

Inspect:
- TimerActivity
- ReminderAlertActivity
- AlertBeep
- HubStore timer persistence
- main.cpp wake logic

Test matrix:
- timer while awake;
- timer while reading;
- timer in Notes/Agenda/Settings;
- timer during music;
- timer during TTS;
- timer expiration in deep sleep;
- reminder expiration in deep sleep;
- two nearby events;
- overdue event plus future event;
- invalid RTC;
- power loss;
- pause/resume;
- stale alarm >2 h.

In TimerActivity::resumeStored(), inspect what happens when a stored timer is marked running but RTC cannot be read. A stale hidden timer must not survive indefinitely while UI acts as if no timer exists.

AlertBeep uses a WAV allocation in PSRAM. Test heavily fragmented PSRAM; an important alert should have a fallback rather than silently disappearing.

Executor response:
Reviewer final check:

## REV-013 — I2S/audio torture test
State: OPEN
Severity: P1
Subsystem: audio

The single audio path is shared by music, microphone, SpeechOut/Piper, beeps, timer/alarm, Translator and Lua.

Repeatedly run:

music -> Talk -> music -> Translator -> timer -> spoken News -> voice note -> Lua cp.say -> Lua cp.listen

Repeat without reboot.

Look for:
- audio.begin failures;
- I2S left owned;
- codec not closed;
- amplifier enable wrong;
- audio task surviving Activity exit;
- PSRAM/internal heap not recovering.

Inspect SpeechOut::playFile peak memory because encoded clip and decoded WAV can coexist.

Executor response:
Reviewer final check:

## REV-014 — VoiceRecorder memory/cleanup soak
State: OPEN
Severity: P1
Subsystem: voice capture

Inspect src/voice/VoiceRecorder.cpp.

Measure internal heap, PSRAM, largest blocks and task counts before/during/after repeated recordings.

hasSpeech() allocates a temporary buffer using malloc. In OOM it currently appears to assume speech exists and continues. Validate whether this fallback is appropriate.

Inspect close-blip blocking wait and ensure system events remain safe.

Run at least 50 recordings without reboot.

Executor response:
Reviewer final check:

## REV-015 — Italian translator support boundaries
State: OPEN
Severity: P2
Subsystem: Translator / STT / TTS

Italian should be available only as a Translator language, not a general UI language.

Verify:
- TranslatorActivity supports it/Italiano;
- server/src/translate.ts accepts it;
- transcribe.ts can STT Italian;
- tts.ts has Italian voice;
- deployed Docker image contains the Piper model;
- board UI only exposes Italian in Translator language choice;
- server general Lang type does NOT accidentally turn Italian into a whole-device UI language.

Test es->it, it->es, en->it, silence, long phrase and TTS failure.

Review Whisper silence hallucination filters for common Italian hallucinations such as subtitle/thank-you boilerplate.

Test firmware-old/server-new and firmware-new/server-old compatibility.

Executor response:
Reviewer final check:

## REV-016 — POST retry idempotency
State: FIXED_PENDING_REVIEW (tanda 2) — id continuo hasta la cola; la parte C++ es NEEDS_HARDWARE
Severity: P0
Subsystem: network / server

Audit lib/ServerClient/ServerClient.cpp end to end.

Critical scenario:
1. server receives a POST;
2. commits a reminder/task/note/memory/calendar action;
3. connection drops before ESP32 receives 200;
4. ServerClient retries.

Determine whether the action can be created twice.

Affected endpoints include /api/voice and any state-changing postOrQueue path.

Build a fault-injection test where the server commits and then deliberately drops the connection.

If necessary, introduce a persisted request/idempotency key and server-side deduplication.

Executor response (tanda 2): CONFIRMADO el hueco. El revisor leyó bien el código.

Verificado en el árbol antes de tocar nada: `enqueue()` hacía `item["id"] = newRequestId()` sin
condición, y `postOrQueue()` llamaba a `enqueue(path, json)` sin pasarle nada. O sea que el id del
intento en línea se perdía y la entrada de la cola nacía con uno nuevo. La secuencia que describe el
revisor —commit remoto + pérdida total de la respuesta + paso a cola— duplicaba de verdad.

Fix (mínimo, tres líneas de lógica):
- `request()` guarda el id de la petición en `lastRequestId_`. Se asigna DESPUÉS de `flushOnConnect()`
  (que también hace peticiones), así que el que queda es el de ESTA petición.
- `postOrQueue()` encola con ese id: `enqueue(path, json, lastRequestId_)`.
- `enqueue(path, json, requestId)` lo usa y sólo inventa uno si viene vacío (los llamadores directos
  de `enqueue`, que no pasaron por un intento en línea).

Así la operación lógica tiene UN id desde el primer intento hasta el replay de la cola, que es
exactamente lo que pide el revisor.

Tests: `./test/idempotency/run.sh` pasa de 7 a 18 pruebas en tres archivos.
- **`journey.test.ts` es el recorrido completo que faltaba**, contra los middlewares de VERDAD
  (no una maqueta): se aplica, se pierde la respuesta, se reproduce con el mismo id -> **una** nota, y
  le vuelve la misma respuesta que se había perdido (`{ok:true,id:1}`). Con un id nuevo —o sea como
  estaba— la misma prueba deja **dos** notas: el defecto queda escrito como prueba, no sólo el
  arreglo. Más: dos operaciones distintas del usuario siguen siendo dos, y una cola de tres
  operaciones reproducida entera no duplica ninguna.
- Y una prueba que fija el ORDEN de los middlewares en api.ts, que es contrato y no se ve leyendo
  ninguno de los dos archivos por separado.

**Lo que NO está automatizado, y hay que decirlo**: que `postOrQueue()` efectivamente le pase el id a
`enqueue()` es C++ y no hay arnés de escritorio para `ServerClient`. Se verificó por lectura y por
compilación (`pio run -e ws397` SUCCESS). La prueba automática cubre el otro lado, que es donde
ocurría la duplicación: el servidor reconociendo el id repetido.

**NEEDS_HARDWARE**: dictar una nota con el WiFi a punto de caerse hasta forzar el encolado, y
comprobar en `/board` que queda UNA sola después de la sincronización.

Commit(s): en esta tanda. CONFIRMED, y demostrado en los dos sentidos contra un servidor real.

Evidencia (HEAD 2a47b1c):
- El aparato YA hace su parte: `ServerClient::request()` genera UN `X-Request-Id` por petición y lo
  mantiene en los tres intentos (ServerClient.cpp:214, con el comentario que lo dice).
- El servidor NO hace la suya: `grep -rn "x-request-id" server/src/` devuelve **una sola línea**,
  `api.ts:115`, que sólo lo devuelve en `/api/ping`. El comentario de al lado lo admitía:
  *"X-Request-Id viene en cada pedido del aparato (estable entre reintentos); por ahora solo lo
  devolvemos"*. No había deduplicación en ningún lado.
- Camino de reintento real: `retryable(status)` es true para -1 y 5xx. El -1 que llega AL CUMPLIRSE el
  tope no se reintenta (1.5.105), pero el que llega ANTES sí — y ése es justamente el socket que se
  cae con la respuesta ya emitida (el keepalive lo corta a los ~14 s desde 1.5.103).
- Reproducido contra un servidor local, DOS POST `/api/notes` idénticos sin `X-Request-Id`
  (o sea: el comportamiento de antes) -> `{"ok":true,"id":3}` y `{"ok":true,"id":4}`: **dos notas**.
  El handler no es idempotente por sí mismo.

Alcance real (más chico que "todo", porque parte ya estaba cubierto): `/api/hub/done` es idempotente
por el `at` de la ocurrencia (F01), `store.addListItem` no repite por texto (1.5.70) y un borrado
repetido deja el mismo estado. Los huecos son los que CREAN: `/api/notes`, `/api/hub/reminder`,
`/api/calendar/event`, `/api/calendar/dictate` y `/api/voice` (que además vuelve a pagar STT + modelo + TTS).

Fix: `server/src/idempotency.ts` (`ReplayCache` pura + `cacheable()`) y un middleware en `api.ts`.
Regla: **cualquier POST bajo /api que traiga X-Request-Id**, no una lista de rutas a mano (una lista
hay que acordarse de actualizarla y se congela). La web no manda ese header, así que no la toca.
Clave `(cuenta, ruta, request id)` — la cuenta adentro a propósito, dos aparatos pueden generar el
mismo id. Un 5xx NO se guarda: el reintento existe para pasar por encima de un fallo temporal, y
guardarlo convertiría una caída de un segundo en diez minutos de la misma caída. Topes: TTL 10 min,
256 entradas, 512 KB por respuesta y 8 MB en total con desalojo del más viejo (el contenedor de Railway
comparte 512 MB con Piper; el tope que manda es el de bytes, no el de entradas).

Verificación de punta a punta, mismo servidor:
- dos POST `/api/notes` con el MISMO `X-Request-Id` -> las dos veces `{"ok":true,"id":1}`, **una** nota,
  y el log del servidor dice `replay /api/notes (reintento-abc): se devuelve la respuesta guardada, no
  se aplica de nuevo`;
- con un id DISTINTO -> `{"ok":true,"id":2}`, nota nueva de verdad.

Tests: `./test/idempotency/run.sh` (7 pruebas): la clave separa cuenta/ruta/id, se devuelve lo guardado,
vencimiento, tope de entradas Y de bytes, respuesta demasiado grande que no se guarda, 5xx que no se
cachea y 4xx que sí, y el mismo id en otra cuenta que no devuelve la respuesta ajena.
Reviewer final check:
NO VERIFICADO como cierre completo. La parte servidor está bien, pero encontré un hueco que conserva
el escenario de duplicación en una transición concreta.

Lo aceptado:
- `ReplayCache` separa cuenta/ruta/id, tiene TTL/topes y no guarda 5xx.
- El middleware devuelve una respuesta ya aplicada cuando un REINTENTO EN LÍNEA llega con el mismo
  `X-Request-Id`.
- `ServerClient::request()` sí conserva un id entre sus intentos.

Hueco actual:
- `postOrQueue()` primero llama a `postJson()`; ese request genera un id interno.
- Si el servidor ALCANZÓ a aplicar la operación pero todas las respuestas posteriores se pierden y
  `postOrQueue()` decide encolarla, llama a `enqueue(path,json)`.
- `enqueue()` genera OTRO `newRequestId()`. Cuando la cola salga más tarde, el servidor ve un id
  nuevo y legítimamente aplica la operación una segunda vez.
- Por tanto, la ventana "commit remoto + pérdida total de respuesta + paso a cola" aún puede duplicar
  nota/recordatorio/evento.
- La suite `test/idempotency` prueba la caché pura; el comentario habla de un escenario de servidor
  real, pero ese fault-injection no está automatizado en la suite.

Arreglo que debe evaluar el ejecutor: el id lógico de una operación tiene que nacer una sola vez y
acompañar tanto los intentos online como la entrada offline. `postOrQueue` debería poder encolar EL
MISMO request id que acaba de usar, y hace falta una prueba automática que simule "se aplicó, se perdió
la respuesta, se encoló y luego se vació".

Impacto de producto: en una caída muy precisa de red, algo que el usuario hizo una vez todavía puede
aparecer dos veces. En /api/voice además puede repetirse una acción clasificada por la IA.

Hallazgo adicional separado en REV-044: un replay correcto puede volver a sumar uso/cuota porque el
middleware de medición envuelve al de idempotencia.

## REV-017 — Offline queue + account reassignment
State: FIXED_PENDING_REVIEW (tanda 2) — política global en ServerClient; 4 comprobaciones NEEDS_HARDWARE
Severity: P0
Subsystem: multi-account sync

HubSyncActivity attempts account check before flushing offline queue. Test this case:

1. actions queued offline under account A;
2. device reassigned to account B;
3. /api/pair/status fails;
4. queue flush proceeds.

If account identity cannot be confirmed, old queued IDs may apply to account B.

Consider refusing to flush until account identity is known.

Add a regression test.

Executor response (tanda 2): CONFIRMADO lo que quedaba abierto. Arreglado con política GLOBAL.

El revisor tiene razón en los dos caminos. Verificado en el árbol:
- `ServerClient::request()` llama a `flushOnConnect()` como primera cosa (antes de mandar nada), así
  que la PRIMERA petición de cualquier sesión de red vaciaba la cola: Hablar, Noticias, la Biblia, el
  Traductor, Vincular.
- `devicesync::ifDue()` (src/sync/Sync.cpp) llama a `SERVER_CLIENT.flushQueue()` directo.
- `ServerTestActivity` es el cuarto camino, pero ahí el usuario lo pide a propósito.

**La guardia se movió de la pantalla al cliente HTTP**, que es donde se juntan los tres caminos:

1. `ServerClient` tiene identidad de SESIÓN DE RED (`Identity::Unknown|Confirmed`), que se reinicia
   donde ya se reiniciaban `flushedThisSession_` y `clockCheckedThisSession_` (o sea cuando se cae la
   red).
2. `ServerClient::confirmAccount()` pregunta `/api/pair/status`, compara con la cuenta que el aparato
   cree tener, y si cambió TIRA la cola y avisa al firmware (`AccountChangedFn`, puntero a función y
   no `std::function`: esto vive en el camino de red). Se protege de reentrar con `inConfirm_` y pone
   `holdFlush_` mientras pregunta, porque la pregunta ES una petición y si no dispararía el vaciado
   que está por autorizar.
3. **`flushQueue()` no sale sin identidad confirmada**, y eso vale para los tres caminos por
   construcción. Si no se puede confirmar, la cola espera a la próxima sesión, igual que sin red.
4. **Sello por entrada**: `enqueue()` graba `acct` con la cuenta del momento y `flushQueue()` descarta
   la entrada cuyo sello no coincide. Es la segunda línea de defensa para el caso en que
   `clearQueue()` no pueda ESCRIBIR la tarjeta — una tarjeta que no escribe es un caso real en este
   aparato (ver "La tarjeta dañada" en CLAUDE.md) y ahí el portón solo no alcanzaría.

**El modo single-account no se rompe y no queda retenido**: en un servidor sin cuentas
`/api/pair/status` contesta `{paired:true, account:null, single:true}`, o sea que la identidad SÍ
queda confirmada (con cuenta vacía) y la cola sale normalmente. Un aparato sin vincular contesta
`account:null` igual. Lo único que retiene es no poder hablar con el servidor, y ahí tampoco se podría
subir nada.

**Costo**: una petición de ~200 bytes por sesión de red, y **sólo si hay algo encolado** — la
comprobación va después de leer la cola y salir temprano si está vacía, que es el caso normal.

`HubSyncActivity::checkAccount()` pasa a ser `return SERVER_CLIENT.confirmAccount();` (35 líneas
menos, y deja de haber dos copias de la misma regla). `main.cpp` siembra la cuenta desde
`HUB_STORE.account` al arrancar y registra el handler que limpia la caché.

Tests: **no hay prueba automática y hay que decirlo.** Es firmware con red, SD y multiusuario, y no
existe arnés de escritorio para `ServerClient` (no hay ninguno en `test/`; `PersistableStoreBase` tira
de `HalStorage` -> SdFat). Lo verificado acá es que compila (`pio run -e ws397` SUCCESS, RAM 28,6 %,
Flash 87,6 %) y el contrato de `/api/pair/status` leído del servidor.

**NEEDS_HARDWARE, para que el dueño lo pruebe** (los cuatro con el aparato delante):
1. Tildar algo sin WiFi (queda en cola) -> mudar el aparato a otra cuenta desde `/board` -> abrir
   **Hablar** (no el hub) -> en el log tiene que salir `el aparato cambió de cuenta: se descarta lo de
   la anterior` y NO se le tiene que tocar nada a la cuenta nueva.
2. Lo mismo dejando que dispare la sincronización oportunista en vez de Hablar.
3. Servidor de una sola cuenta: tildar sin WiFi y reconectar -> la cola tiene que SALIR igual
   (sin esto el arreglo sería una regresión).
4. Con el servidor caído (502 en `/api/pair/status`): la cola tiene que QUEDARSE, con la línea
   `no se pudo saber de qué cuenta es el aparato`, y salir en la sincronización siguiente.

Commit(s): en esta tanda. CONFIRMED, y **la teoría se queda corta**: el agujero es más grande y la guardia
F11 estaba anulada por su propia implementación.

Lo que dice la teoría (cierto): `HubSyncActivity::checkAccount()` tenía tres `return` mudos — fallo de
transporte, JSON ilegible y `account` vacío — y `runSync()` llamaba a `flushQueue()` igual. Con la cola
cargada en la cuenta A, el aparato mudado a B desde `/board` (el token no cambia) y un 502 en
`/api/pair/status`, la cola vieja se reproduce contra B. Los ids del store se numeran desde 1 en CADA
cuenta, así que tilda o borra lo que le tocó el mismo número. Eso es contaminación entre cuentas.

Lo que la teoría NO vio, y es peor:
1. **La pregunta de identidad vaciaba la cola antes de contestar.** `checkAccount()` pregunta con
   `SERVER_CLIENT.get("/api/pair/status")`, y `ServerClient::request()` llama a `flushOnConnect()`
   como primera cosa (ServerClient.cpp:200), ANTES de mandar nada. O sea que en el camino normal la
   cola ya se había subido cuando se llegaba a comparar la cuenta. La guardia de F11 era, en la
   práctica, código muerto.
2. **Hay tres caminos que vacían la cola y sólo uno miraba la cuenta**: `HubSyncActivity::runSync()`
   (el que la miraba), `ServerClient::flushOnConnect()` (se dispara en la PRIMERA petición de cada
   sesión de red: Hablar, Noticias, la Biblia, el Traductor, Vincular…) y `devicesync::ifDue`
   (Sync.cpp:62, la sincronización oportunista). `ServerTestActivity` es el cuarto, pero ahí el usuario
   lo pide a propósito.

Fix (esta tanda, lo chico y seguro):
- `checkAccount()` devuelve `bool` ("¿se pudo AVERIGUAR de quién es?") y cada camino de fallo lo dice en
  el log; `runSync()` no vacía la cola si es false. La cola se queda para la próxima sincronización,
  igual que cuando no hay red: perder una vuelta no cuesta nada, aplicarla contra la cuenta equivocada
  borra lo de otro.
- `ServerClient::setFlushHold(bool)` + guardia en `flushOnConnect()` y en `flushQueue()`.
  `checkAccount()` lo pone antes de preguntar y lo saca después, así la pregunta no dispara la subida
  que está por autorizar. Esto es lo que hace que la guardia F11 exista de verdad.

NO arreglado a propósito, y va al revisor y al dueño: cerrar `flushOnConnect()` y `devicesync::ifDue`
pide una política de "la cola no sale hasta que esta sesión confirmó la cuenta". Si el default fuera
"retenida", un aparato que nunca abre el hub (o un servidor de una sola cuenta) se quedaría con la cola
sin subir para siempre. Eso no es pérdida de datos pero sí un cambio de comportamiento para TODOS los
aparatos, y no me parece que lo decida el ejecutor solo. Propuesta concreta para revisar:
identidad por sesión de red en `ServerClient` (`Unknown`/`Confirmed`), `flushOnConnect()` y `flushQueue()`
retenidos mientras sea `Unknown`, y `devicesync::ifDue` confirmando igual que el hub.

Tests: no hay prueba automática de este camino — es firmware con red y multiusuario, y no hay arnés.
Lo que se verificó es que compila (`pio run -e ws397` SUCCESS) y el contrato de `/api/pair/status`
leído del servidor: `{paired, account, single}` con `account: null` en modo de una sola cuenta (por eso
"vacío" se trata como identidad CONOCIDA y sigue vaciando: ahí hay una sola cuenta y la cola es de ésa).
Queda como NEEDS_HARDWARE la comprobación de punta a punta.
Reviewer final check:
NO VERIFICADO. El ejecutor encontró correctamente que la guardia de HubSync era ineficaz y el
`setFlushHold(true)` hace que ESA consulta de identidad ya no vacíe la cola antes de comparar.
También es correcta la decisión de retener la cola si /api/pair/status no pudo contestar.

Pero el P0 original sigue abierto en otros caminos:
- `ServerClient::request()` llama automáticamente a `flushOnConnect()` en la primera petición de
  una sesión cuando `holdFlush_` es false.
- `devicesync::ifDue()` llama directamente a `SERVER_CLIENT.flushQueue()` sin confirmar cuenta.
- Por tanto, después de mover un aparato de cuenta A a B, si antes de entrar a HubSync se abre Hablar,
  Noticias, Traductor, Biblia u otra función que haga la primera petición, la cola de A puede salir
  contra B. La sincronización oportunista puede hacer lo mismo.
- Los ids del store son locales a cada cuenta, así que no es sólo duplicación: una operación vieja
  puede marcar/borrar/modificar el objeto de B que casualmente tenga el mismo id.

Se necesita una política global, no sólo una guarda local: por ejemplo identidad de sesión
`Unknown/Confirmed` dentro de ServerClient, con toda salida de la cola retenida hasta confirmar la
cuenta; o ligar cada entrada de cola a la identidad de cuenta que la creó. El modo single-account debe
seguir funcionando sin quedarse retenido para siempre.

Impacto de producto: sólo afecta instalaciones multiusuario/reasignación de aparato, pero ahí puede
modificar datos de la cuenta equivocada. Se mantiene P0 y requiere prueba de punta a punta antes de
cerrarlo.

## REV-018 — Timezone isolation and DST
State: OPEN
Severity: P0/P1
Subsystem: server multi-user time

Audit server-wide uses of HUB_TZ and timezone helpers that lack accountId.

Pay particular attention to:
- server/src/agenda.ts
- server/src/store.ts
- server/src/voice.ts
- server/src/hub.ts
- Calendar/ICS

Defaults around America/Argentina/Buenos_Aires must not override per-account location.

Test two simultaneous accounts with different timezones.

Test DST:
- nonexistent local time during spring-forward;
- duplicated local time during fall-back;
- recurring reminders across DST.

Changing location should invalidate timezone caches immediately, not leave old location semantics for an arbitrary cache TTL.

Executor response:
Reviewer final check:

## REV-019 — silentRestart() may now return
State: OPEN
Severity: P1
Subsystem: lifecycle / Wi-Fi

Audit every call to silentRestart() and silentRestartToReader().

On WS397, silentRestart() may now turn Wi-Fi off and return instead of ESP.restart() when the largest heap block is considered healthy.

Any caller written when restart was effectively terminal may contain incorrect control flow after the call.

Also validate empirically whether the current largest-block threshold is sufficient for subsequent EPUB/font/TLS workloads.

Executor response:
Reviewer final check:

## REV-020 — Deep-sleep hardware current
State: NEEDS_HARDWARE
Severity: P1
Subsystem: power

WS397 has custom sleep logic involving QMI8658, ES8311, amplifier, PMIC rails, GPIO hold and GPIO5 wake.

Hardware-test:
- real deep-sleep current;
- 8–12 h battery loss;
- gpio_hold release on wake;
- codec recovery;
- SD recovery;
- OK wake;
- PWR behavior;
- timer wake;
- reminder wake;
- spurious wake.

Prefer physical current measurement rather than battery percentage alone.

Executor response:
Reviewer final check:

## REV-021 — Asset sync partial-state semantics
State: OPEN
Severity: P1
Subsystem: assets

Inspect src/activities/home/AssetSyncActivity.cpp.

Questions:

A. assetsVersion is updated even if some files failed while assetsPending remains true. Find every consumer of assetsVersion and verify nobody treats it as “complete package installed”.

B. Retired apps are removed before the new package finishes downloading. A network failure may remove a working old app without installing replacement. Consider cleanup after successful commit.

C. Local manifest is written directly to canonical path. Evaluate temp + rename for power-loss safety.

D. Each asset is downloaded completely into ServerClient::Response before SD write. Measure internal largest block with maximum asset plus TLS.

Executor response:
Reviewer final check:

## REV-022 — Factory Lua app build reproducibility
State: OPEN
Severity: P1
Subsystem: server deployment

Inspect server/Dockerfile and factory-app acquisition.

If the Docker build fetches apps from moving branch ws397, deployment can begin at commit X and fetch Lua from commit Y after branch moves.

That can produce server binary/API from one revision and Lua apps from another.

Pin factory apps to the exact deployment SHA or otherwise make build inputs immutable/reproducible.

Executor response:
Reviewer final check:

## REV-023 — Lua global state/concurrency
State: OPEN
Severity: P0/P1
Subsystem: Lua

Audit:
- src/lua/LuaApp.cpp
- src/lua/LuaSandbox.cpp
- src/activities/games/LuaAppsActivity.cpp

There is already a lock because on_draw can run in render task while on_tick/on_key run in main loop. Stress it.

Specifically test:
- g_requests;
- g_hostBusy;
- allocator globals;
- closing app with a network response pending;
- immediately opening another app;
- stale response from old app reaching new app;
- cancelQueued();
- long-Back emergency exit;
- hostile __gc;
- pcall swallowing step-limit errors;
- memory exhaustion.

No worker may access a lua_State after destruction.

Executor response:
Reviewer final check:

## REV-024 — Lua viewer UTF-8 truncation
State: OPEN
Severity: P2
Subsystem: Lua viewer

LuaAppsActivity::openViewer() reads up to VIEW_CAP.

If truncation lands inside a UTF-8 codepoint, the system viewer can receive invalid UTF-8.

Clamp truncation to a valid UTF-8 boundary.

Test huge files with emoji/CJK/accented characters exactly at VIEW_CAP.

Executor response:
Reviewer final check:

## REV-025 — Lua app visual regression
State: CONFIRMED
Severity: P2
Subsystem: Lua UI

Visually audit:
- ahorcado.lua
- librito.lua
- libros.lua
- mascota.lua
- sudoku.lua

There are manual ROW1/ROW2 sizes and manual coordinates. Verify selector, circle, title, subtitle, metadata and scrolling never overlap on real WS397 geometry.

Pay special attention to title + long subtitle rows; previous device behavior showed an indicator obscuring subtitles.

Reviewer hardware evidence (2026-09-20):
- Confirmado por log real al abrir `mascota`: `[GFX] 80 pixeles fuera de pantalla (ultimo 480,447)`
  y también ráfagas de 160 píxeles. Se repite en múltiples renders, no es un evento aislado.
- El error aparece inmediatamente después de `[LUA] mascota abierta` y vuelve durante sus refrescos,
  por lo que la hipótesis de regresión visual ya es bug confirmado.
- No hay crash en el log, pero el renderer está intentando escribir coordenadas fuera del viewport;
  eso puede traducirse en elementos cortados, selector/figura pegados al borde y ruido diagnóstico.

Impacto visible: la mascota o alguno de sus elementos puede quedar recortado/deformado en el borde
derecho/inferior; además cada render llena el log con errores GFX.

Executor response:
Reviewer final check:

## REV-026 — Mapped input matrix
State: OPEN
Severity: P1/P2
Subsystem: input

Audit src/MappedInputManager.cpp using:
- all four orientations;
- remapped front buttons;
- swapped side buttons;
- touch;
- swipe;
- long press;
- double Back;
- PWR;
- Home.

Check whether consuming Back edge-swipe can starve an Activity that later expects a normal swipe in the same frame.

Also test accidental configuration where multiple logical roles map to one physical button and ensure suppressed release logic does not mis-consume later real input.

Executor response:
Reviewer final check:

## REV-027 — OptionPopup callback reentrancy
State: OPEN
Severity: P1
Subsystem: UI callbacks

Timer already documents a previous issue where showing/replacing an OptionPopup from inside its own callback can destroy the std::function currently executing.

Search all OptionPopup.show callsites and inspect callbacks for reentrant self-replacement.

Use deferred/pending state when necessary.

Executor response:
Reviewer final check:

## REV-028 — Back/PWR responsiveness during blocking network operations
State: OPEN
Severity: P1
Subsystem: NetPump

The project added NetPump so synchronous network operations do not make the device deaf.

Test cancellation during:
- DNS failure;
- Wi-Fi connected without internet;
- TLS stall;
- slow Railway response;
- 60–90 s AI request;
- OTA;
- book download;
- asset download;
- OPDS;
- KOReader;
- Telegram;
- PubMed/news.

Audit for any networking path that bypasses NetPump hooks.

Executor response:
Reviewer final check:

## REV-029 — USB MSC/Web server SD exclusivity
State: OPEN
Severity: P0
Subsystem: storage ownership

Audit:
- CrossPointWebServer
- WebDAV
- UsbDriveActivity
- USB handoff
- ProtectedPaths
- HalStorage

Before handing raw SD to USB host, ensure no filesystem consumer remains:
- stacked Activity;
- render task reading SD font;
- music stream;
- device log write;
- background sync;
- cache/file handle.

After return, invalidate stale handles/caches and restart safely as needed.

Executor response:
Reviewer final check:

## REV-030 — Atomicity of device files
State: OPEN
Severity: P0/P1
Subsystem: persistence

Classify device files as:
1. essential state;
2. regenerable cache;
3. user content.

Essential state/user content should use temp + close/flush + rename where practical.

Power-cut test:
- reading progress;
- HubStore;
- settings;
- credentials;
- asset manifest;
- news cache;
- calendar cache;
- offline notes;
- offline request queue.

Executor response:
Reviewer final check:

## REV-031 — Piper worker failure propagation
State: OPEN
Severity: P1
Subsystem: server TTS

Inspect server/src/tts.ts.

There is a persistent Piper process per language and pending resolver queue.

Kill Piper while multiple requests are pending. Every pending promise should fail promptly rather than each waiting until its own timeout.

Then verify the next request can recreate/recover the worker.

Executor response:
Reviewer final check:

## REV-032 — Voice response peak memory
State: OPEN
Severity: P1
Subsystem: voice protocol / memory

server/src/voice.ts and translate.ts frame JSON + optional audio using Buffer.concat.

Server can momentarily hold source audio plus concatenated response.

Device stores whole ServerClient::Response and SpeechOut then creates decoded WAV.

Measure peak server memory and ESP32 PSRAM/internal heap near maximum reply size.

Long spoken content should be chunked/paged rather than creating huge one-shot buffers.

Executor response:
Reviewer final check:

## REV-033 — OTA board validation
State: OPEN
Severity: P0
Subsystem: OTA

Inspect OtaUpdater.cpp, OtaUpdateActivity.cpp and server/src/firmware.ts.

Updater validates MCU and board mismatch when a board tag is found. Determine whether an untagged S3 image can still pass.

For WS397, consider requiring an explicit matching WS397 board tag.

Test:
- advertised vs downloaded size;
- cancel mid-download;
- Wi-Fi failure;
- PWR during write;
- truncated image;
- valid image for another S3 board;
- esp_ota_end failure;
- failed boot/rollback behavior.

Also inspect server retention of old firmware binaries and prune if necessary.

Executor response:
Reviewer final check:

## REV-034 — WS397 release transaction
State: VERIFIED
Severity: P0 release gate
Subsystem: release

Do not assume generic release workflows publish WS397.

Audit release.ps1, release.sh, /firmware, .ws397-build and include/ws397_version.h.

Desired sequence:
version increment -> successful build -> tests -> upload -> verify /firmware/latest -> release considered published.

Avoid repository claiming version N while Railway still serves N-1 without an explicit warning.

Executor response: parcialmente REFUTADO, un hueco real CONFIRMADO.

Estado actual medido: `.ws397-build` = 118, `include/ws397_version.h` = `WS397_BUILD 118`, y
`GET https://paper-esp32.up.railway.app/firmware/latest` -> `"tag_name":"1.5.118"`. **Los tres
coinciden hoy**, así que el síntoma concreto que teme la teoría no está presente.

Lo que `release.sh` ya hacía bien (REFUTADO):
- bumpea el número ANTES de compilar, así que el binario lleva la versión que se anuncia;
- `set -euo pipefail` + `pio run` -> un build fallido aborta y no sube nada;
- `.ws397-build` se escribe sólo DESPUÉS de un build exitoso;
- `curl -fsS` -> un PUT con error HTTP aborta el script.

El hueco real (CONFIRMADO): **nada comprobaba qué quedó servido después del PUT.** Un 200 del PUT no
es lo mismo que `/firmware/latest` entregando la versión nueva, y el único síntoma de la diferencia es
que el aparato no ve la actualización (la comparación es major.minor.patch estricta) — que es
exactamente el modo de fallo silencioso que describe la teoría.

Fix: `release.sh` ahora relee `/firmware/latest` después de subir, extrae `tag_name` y falla con
`exit 1` si no coincide con lo que acaba de publicar, diciendo que el release NO está publicado.
Probado el extractor contra el endpoint real en modo lectura: devuelve `1.5.118`, que coincide con
`.ws397-build`. **No se publicó ninguna OTA en esta sesión.**

Lo que NO se tocó, y va al dueño: la teoría pide también correr los tests dentro del release. Eso
cambia la ergonomía del release (hoy son ~2 minutos) y es decisión suya, no del ejecutor. Con REV-001
arreglado, CI vuelve a correr esas mismas suites en cada push a ws397, que cubre casi lo mismo antes
de llegar al release.

Anotado aparte: nada ata el `.bin` servido a un commit del repositorio (no se guarda sha ni commit),
así que "el binario servido corresponde al fuente de 1.5.118" no se puede verificar, sólo creer.
Si el revisor lo considera parte de REV-034, es un item nuevo y más grande.
Reviewer final check:
VERIFIED para el modo de fallo planteado.
- `release.sh` ya abortaba ante build o PUT fallido; el ejecutor tenía razón al refutar esa parte.
- Ahora, después del PUT, relee `/firmware/latest`, extrae `tag_name` y falla si no coincide con la
  versión recién subida.
- La rama tiene CI verde y la versión fuente sigue en 1.5.118. No se publicó OTA durante esta tanda.

Impacto de producto: antes el script podía decir "Listo" aunque el servidor siguiera ofreciendo la
versión anterior; el aparato entonces no encontraba la actualización y parecía que OTA "no hacía
nada". Ahora el release falla de forma visible si eso ocurre.

Hardening pendiente, sin reabrir este hallazgo: el servidor no conserva SHA de commit/binario y el
script no corre toda la batería de tests dentro del propio release. CI verde cubre el gate actual,
pero la trazabilidad binario↔commit sigue siendo mejorable.

## REV-035 — Server persistence race audit
State: OPEN
Severity: P0/P1
Subsystem: server persistence

Inspect server/src/fsjson.ts.

mutateDoc serializes read-modify-write correctly. Find state-changing callsites using simpler write helpers where data may have been serialized before entering the queue.

Concurrent modifications to store/calendar/news/config must use the proper mutation primitive.

Executor response:
Reviewer final check:

## REV-036 — Bounded server caches
State: OPEN
Severity: P2
Subsystem: server long-running memory

Review Maps/Sets for:
- store;
- timezone;
- weather;
- RSS;
- search;
- Telegram clients;
- Piper workers;
- jobs;
- news-running state.

Ensure long-lived Railway process cannot grow without bound.

Executor response:
Reviewer final check:

## REV-037 — Multi-user isolation
State: OPEN
Severity: P0
Subsystem: server accounts

Test two accounts concurrently with:
- different feeds;
- notes;
- memories;
- timezones;
- calendars;
- voice context;
- Telegram configuration;
- Lua jobs.

Look for global variables leaking one account's data into another.

Voice context and timezone deserve special attention.

Executor response:
Reviewer final check:

## REV-038 — Firmware i18n hardcoded UI strings
State: OPEN
Severity: P2
Subsystem: UI / i18n

AGENTS.md requires visible firmware text through tr().

Search:
- drawText with literals;
- drawCenteredText with literals;
- GUI.drawPopup literals;
- failureDetail strings shown directly to user.

News failure descriptions were one area observed with directly written Spanish.

Logs may be hardcoded; user-visible UI should not be.

Also test translations for layout overflow.

Executor response:
Reviewer final check:

## REV-039 — Memory fragmentation soak
State: NEEDS_HARDWARE
Severity: P1
Subsystem: whole device

Run repeated cycle for hours:

Hub -> EPUB -> dictionary -> Hub -> News -> TTS -> Translator -> recording -> Lua -> music -> Settings -> EPUB

At every cycle log:
- internal free;
- internal minimum;
- largest internal block;
- PSRAM free;
- largest PSRAM block;
- task stack high-water;
- task count.

Values should stabilize rather than drift downward.

Use existing Settings -> Memory instrumentation and extend logging only if necessary.

Executor response:
Reviewer final check:

## REV-040 — Deadlocks/watchdog/lock ordering
State: OPEN
Severity: P0/P1
Subsystem: concurrency

Audit:
- RenderLock;
- requestUpdateAndWait();
- storageMutex;
- Activity callbacks;
- Lua mutex;
- AudioManager;
- NetPump.

Confirm consistent lock ordering and inspect indirect callbacks that could call requestUpdateAndWait while on render task, while holding RenderLock or while another waiter exists.

Executor response:
Reviewer final check:

## REV-041 — Corruption/fuzz suite
State: OPEN
Severity: P1
Subsystem: robustness

Prepare deliberately corrupt:
- EPUB;
- malformed UTF-8 TXT;
- XTC;
- StarDict;
- cache JSON;
- asset manifest;
- progress.bin;
- settings;
- battery.csv;
- Lua state;
- Lua script.

Success criterion is not “opens correctly”. Success is:
- no reset;
- no abort;
- no permanent hang;
- user gets a recoverable error/exit path.

Executor response:
Reviewer final check:

## REV-042 — cppcheck: falso positivo por colisión de nombres (levantado por el ejecutor)
State: VERIFIED
Severity: P3
Subsystem: CI / análisis estático

No estaba en la cola del revisor: apareció en la PRIMERA corrida real de CI después de arreglar
REV-001, que es justamente para lo que sirve tener CI.

`pio check --fail-on-defect ...` falla con:

    src/activities/reader/DictionaryWordSelectActivity.cpp:96: [high:error]
      Uninitialized struct member: box.start [uninitStructMember]
      Uninitialized struct member: box.len

Evidencia de que es falso: hay DOS structs privados distintos llamados `WordBox`, cada uno dentro de
su propia clase — `DictionaryWordSelectActivity::WordBox` (x, y, width, row, text, style) y
`DictionaryDefinitionActivity::WordBox` (start, len, x, y, width). El de la línea 96 **no tiene**
`start` ni `len`, y sus seis campos se asignan en las líneas 90-95. cppcheck confundió los dos.

Fix: supresión en línea acotada a ese `push_back`, con el motivo escrito al lado
(`--inline-suppr` ya estaba en `check_flags` de platformio.ini). Verificado local:
`pio check -e ws397 --fail-on-defect low --fail-on-defect medium --fail-on-defect high` ->
**No defects found**.

Para el revisor: suprimir es lo menos invasivo, pero la causa de fondo es que dos structs privados
con el MISMO nombre y significados distintos conviven en el mismo subsistema, y eso confunde también
a quien lee. Renombrar uno sería mejor — pero toca archivos de REV-006/REV-007, que no reclamé, y
puede haber otro agente ahí. Queda a criterio del revisor cambiar la supresión por un rename.

Reviewer final check:
VERIFIED.
- Los dos `WordBox` son tipos privados distintos; el de DictionaryWordSelectActivity inicializa los
  seis campos que realmente posee.
- La supresión está acotada a la línea y documenta por qué existe.
- cppcheck está verde en CI actual.

Impacto de producto: ninguno en ejecución. Era un falso positivo que hacía fallar la puerta de CI.
Renombrar uno de los structs podría mejorar mantenibilidad, pero no hace falta tocar el lector para
resolver este P3.

## REV-043 — El build `default` (ESP32-C3) está roto desde 1.5.106 (levantado por el ejecutor)
State: VERIFIED
Severity: P1
Subsystem: build / upstream

Tampoco estaba en la cola: lo destapó la primera corrida real de CI.

    src/main.cpp:458:5: error: 'codecsleep' has not been declared

`#include "util/CodecSleep.h"` está adentro de `#if SOC_PM_SUPPORT_EXT1_WAKEUP`, pero la llamada
`codecsleep::es8311Suspend()` de `sleepNow()` NO tiene guarda de preprocesador: su guarda es
`if (BoardConfig::isWS397())`, que es de EJECUCIÓN. En el ESP32-C3 `SOC_PM_SUPPORT_EXT1_WAKEUP` es 0,
así que el header no entra y no compila. Entró con el trabajo de suspensión del códec (1.5.106) y
nadie lo vio porque CI estaba muerto desde 1.5.113 (REV-001).

Alcance: el firmware que se publica por OTA (`ws397`, S3) NUNCA estuvo afectado — `Build ws397` da
verde. Lo roto es la placa upstream X4/X3 del env `default`.

Fix: el include sale del `#if`. El header sólo necesita Arduino/BoardConfig/Logging/Wire, que existen
en el C3, y su contenido sigue protegido por la guarda de ejecución.

Tests: **no verificado localmente** — en este sandbox sólo está instalado el paquete de framework de
esp32s3, así que `pio run -e default` muere antes de compilar
(`FileNotFoundError: .../esp32c3/sdkconfig.crosspoint`). Es correcto por inspección (mover un
`#include` fuera de un `#if`) y lo confirma CI en la corrida siguiente. Si CI sigue roja ahí,
reabrir.

Reviewer final check:
VERIFIED.
- Mover `CodecSleep.h` fuera de la guarda de EXT1 resuelve el símbolo en C3 sin ejecutar lógica
  WS397 en esa placa.
- La evidencia decisiva es CI actual: `Build default` termina en success además de `Build ws397`.

Impacto de producto: el WS397 del usuario nunca sufría este defecto. Lo que estaba roto era la
compilación del firmware upstream ESP32-C3; ahora vuelve a poder construirse y CI vuelve a vigilarlo.


## REV-044 — Un replay de /api/voice puede volver a consumir cuota
State: FIXED_PENDING_REVIEW
Severity: P2
Subsystem: server usage / idempotency

Hallazgo del revisor al revisar REV-016.

El middleware de cuota/medición está registrado ANTES que el middleware de idempotencia. La medición
hace `await next()` y, al volver, suma uso si la respuesta final es <400. En un reintento que el
middleware de idempotencia resuelve desde ReplayCache, no vuelve a correr STT/modelo/TTS, pero la capa
exterior ve igualmente un 2xx y puede ejecutar `addUsage()` otra vez.

Reproducir con un X-Request-Id repetido sobre una ruta METERED y comprobar el contador antes/después.
Si se confirma, el replay debe llevar una marca de contexto (p. ej. `c.set("idempotentReplay", true)`)
para que usage no vuelva a cobrarlo, o el orden/contrato de middleware debe cambiar sin romper cuotas.

Impacto posible: no duplica la acción, pero un usuario con límites mensuales puede agotar su cuota más
rápido sólo porque la red obligó a reintentar.

Executor response: CONFIRMADO, y reproducido en una prueba que falla sin el arreglo.

Evidencia: el middleware de medición se registraba en api.ts ANTES que el de idempotencia, hace
`await next()` y al volver sólo mira `c.res.status < 400`. No tiene forma de saber que la respuesta
salió de la caché. En un replay no corre STT, ni modelo, ni TTS — y cobraba igual.

Fix:
- El middleware de idempotencia deja `c.set("idempotentReplay", true)` antes de devolver la respuesta
  guardada (`REPLAY_KEY` / `isReplay()` en idempotency.ts).
- La medición pregunta por esa marca: `shouldCharge(status, replay)` = `status < 400 && !replay`.
- Los dos middlewares salieron de api.ts a su propio archivo (`metering.ts`, y el de idempotencia a
  `idempotency.ts`) **para poder componerlos en una prueba**: el defecto no estaba en ninguno de los
  dos sino en el orden entre ellos, y eso no se ve leyendo cada uno por separado. api.ts sólo los
  enchufa, con el orden comentado y con una prueba que lo fija.

Tests: `test/idempotency/metering.test.ts`, seis pruebas que componen los middlewares REALES en un
Hono con un espía sobre `addUsage`: el primer pedido cobra una vez; el reintento con el mismo id no
ejecuta el handler y **no cobra**; otro id sí cobra; sin `X-Request-Id` (la web) se cobra siempre; una
ruta fuera de METERED nunca cobra; y `shouldCharge` con las dos reglas.

**Verificado que la prueba atrapa el defecto**: sacando la regla del replay de `shouldCharge`, la
prueba falla con `Expected: 1, Received: 2`. Con el arreglo, 6 pass.

Nota sobre el alcance: `addUsage()` no hace nada sin `DATABASE_URL`, así que en un servidor de una
sola cuenta esto nunca se notó. Afecta a la instalación multiusuario con topes puestos, que es donde
el revisor lo ubicó.
Reviewer final check:

## REV-045 — La web llama “resumidas” a traducciones médicas
State: FIXED_PENDING_REVIEW
Severity: P3
Subsystem: board web / News

Hallazgo del revisor al revisar REV-005.

En el HEAD actual, la tarjeta "Paquete del lector" arma:
`newsPack.chewed + " resumidas"`.
Para diarios, `chewed` sí puede significar resumido. Para Medicina, desde MEDICAL_SUMMARY_VERSION=4,
`chewed=true` significa traducción técnica completa, explícitamente NO un resumen.

El paquete y el aparato están correctos; es sólo semántica de la web. Cambiar por una palabra neutral
("procesadas") o mostrar contadores separados si se quiere precisión.

Impacto de producto: el médico puede creer que el abstract fue condensado cuando en realidad se
tradujo conservando el contenido técnico.

Executor response: CONFIRMADO. Se eligió **contadores separados** y no una palabra neutra.

El revisor ofrecía las dos opciones. "Procesadas" es neutro pero no dice nada: el médico sigue sin
saber si le condensaron el abstract. Y la distinción ya existe en los datos, así que no hace falta
guardar nada nuevo: `PackItem.medicalVersion` sólo lo llevan los papers.

Fix:
- `GET /api/news/status` devuelve `chewed` (sólo diarios) y `chewedMedical` (sólo papers).
- `GET /api/news/preview` agrega `medical` por ítem. Va aparte del nombre del medio a propósito: el
  usuario puede renombrar la fuente y el nombre dejaría de servir para saber qué es.
- La web dice `"3 listas · 1 resumida · 1 paper traducido"` (con singular y plural) y por ítem
  `· resumido` o `· traducido` según corresponda.

Verificado contra un servidor real con un paquete sembrado (un diario masticado, un paper traducido y
un paper sin traducir):
`{"items":3,"chewed":1,"chewedMedical":1}` y en el preview `1-1 medical=false`, `2-1 medical=true`.
Y con Chromium a 360 px: `3 listas · 1 resumida · 1 paper traducido`, `10:00 · resumido`,
`09:00 · traducido`, sin errores de consola y sin irse de ancho.
Reviewer final check:

## REV-046 — CI usa una versión móvil de Bun
State: FIXED_PENDING_REVIEW (falta confirmar el digest del Dockerfile)
Severity: P2
Subsystem: CI / reproducibilidad

Hallazgo compartido por el ejecutor y confirmado por el revisor.

Los jobs de servidor/ws397-tests instalan Bun con `bun-version: latest`. En esta misma tanda el
ejecutor observó 1.3.11 local y 1.4.2 en runner; dos iteraciones del test de net_lookup fallaron por
diferencias de runtime/cache entre versiones.

El test final ya es más robusto y CI está verde, pero la herramienta sigue siendo una entrada móvil.
Fijar una versión conocida de Bun (y actualizarla deliberadamente) haría reproducible el pipeline.
Esto debe coordinarse con package/lockfile y Railway para no probar con un runtime distinto del que se
despliega.

Impacto de producto: no se nota directamente en el aparato. Puede hacer que una actualización futura
de Bun vuelva rojo CI o, peor, cambie comportamiento de red entre lo probado y lo desplegado.

Executor response: CONFIRMADO, y con un dato que el hallazgo no tenía: **producción ya está fijada,
CI no.**

`server/Dockerfile` arranca con `FROM oven/bun:1-debian@sha256:4f6e31d1…` — fijado por DIGEST (más
fuerte que una etiqueta: la imagen no puede cambiar debajo), desde `0789537` del 2026-09-15. CI, en
cambio, instalaba `bun-version: latest` en los dos jobs. O sea que **CI probaba con un runtime
distinto del que se despliega**, que es peor que "no reproducible".

Fix:
- `server/.bun-version` con la versión, y los dos jobs pasan a `bun-version-file: server/.bun-version`
  (entrada soportada por `oven-sh/setup-bun@v2`, verificada en su `action.yml`). Un solo lugar donde
  está escrita: una versión repetida en dos jobs se despareja sola.
- El Dockerfile queda con su digest —no se afloja a una etiqueta móvil— y con el comentario que ata
  las dos cosas.

**Por qué 1.4.2 y no otra**: es la única versión con evidencia de haber pasado la batería completa —
la corrida verde 35478957781, doce jobs, corrió sobre 1.4.2. Elegir otro número habría sido
justamente el cambio a ciegas que el pedido prohíbe. El sandbox tiene 1.3.11 y ahí las once suites y
el `tsc` también pasan, así que el código no depende de ninguna de las dos.

**Lo que NO pude verificar, y es lo que queda para el dueño**: a qué versión corresponde el digest del
Dockerfile. El registro de Docker pide autenticación y el proxy de este sandbox la bloquea
(`UNAUTHORIZED` en `registry-1.docker.io`). Si ese digest no es 1.4.2, hay que alinear los dos —
**mejor volviendo a fijar el digest de la versión elegida que aflojando el Dockerfile a una etiqueta
móvil**. Queda escrito en el propio Dockerfile para que no se pierda.

Tests: las once suites y `bunx tsc --noEmit` en verde con 1.3.11 (local); CI lo va a correr con 1.4.2
en la próxima pasada, que es la verificación que falta.
Reviewer final check:


## REV-047 — Integración IA viva puede estar rota aunque CI esté verde
State: FIXED_PENDING_REVIEW
Severity: P1
Subsystem: server / live provider / observability

Hallazgo del revisor a partir de logs reales del dueño.

Síntoma reportado: las funciones que dependen del proveedor de IA terminan en 502 de forma generalizada,
mientras el aparato y Railway siguen comunicándose. La corrida CI del HEAD actual está verde, lo que
demuestra que la batería automática no valida la integración viva con el proveedor configurado.

Evidencia del log del aparato:
- /api/voice llegó a Railway y STT pudo responder al menos una vez con `no_speech`, por lo que red,
  TLS, token y endpoint estaban operativos en ese momento.
- Un 502 en Ask/Voice/Translate se genera explícitamente cuando STT o LLM/proveedor falla.
- La web ya dispone de `POST /api/board/config/test`, que prueba el LLM configurado de verdad, pero
  esa prueba no forma parte del gate de deploy/release.

Acción:
1. Obtener primero el texto exacto de `/api/board/config/test` en producción.
2. Identificar si falla LLM, STT o ambos y conservar status/cuerpo del proveedor.
3. Añadir un health/smoke check de producción ejecutable tras deploy (sin meter una clave real en CI).
4. No considerar un deploy de servidor "sano" sólo porque CI está verde si se modificó llm/config/net.

Impacto de producto: Hablar, Preguntar al libro, Traductor y cualquier función que use el modelo
pueden quedar inutilizables a la vez aunque el resto del aparato sincronice normalmente.

Executor response: CONFIRMED (la parte de observabilidad). State: FIXED_PENDING_REVIEW.

La premisa es correcta y el log del dueño la sostiene sin ambigüedad: en la MISMA ventana en la que
`POST /api/voice` y `POST /api/bible/ask` devolvían 502, `GET /api/hub` sincronizó, `GET /api/news/pack`
bajó 40 notas y `POST /api/transcribe` devolvió texto. O sea el contenedor arriba, la red arriba, el
token bien, el STT bien. Y `/api/bible/ask` no toca el micrófono ni el STT: es modelo y nada más. Eso
descarta Railway, la red y la transcripción, y deja al proveedor del modelo — que es adonde apuntaba
el revisor.

También descarta la sospecha que yo tenía encima (que algo de mi tanda 2 estuviera volteando el
contenedor): un contenedor caído da 502 en TODO, y `/api/hub` y `/api/news/pack` pasaron.

Fix (el que se puede hacer sin clave del proveedor a mano):
- `server/src/providerLog.ts` (nuevo): anillo en memoria con los últimos 20 fallos del proveedor
  (hora, `llm`/`stt`, quién lo pidió, mensaje recortado a 300). En memoria a propósito: escribir un
  archivo en el camino de error de cada petición convierte un proveedor caído en un disco lleno, y lo
  que interesa es si está fallando AHORA.
- Se anota en UN solo lugar por cada puerta: `chatSearch` y `chatJson` en `llm.ts` (envueltas por
  `noting()`, que anota y relanza) y el 4xx/5xx del STT en `transcribe.ts`. Con eso quedan cubiertos
  Hablar, Preguntarle al libro, el Traductor, la Biblia y el calendario sin tocar ninguno de sus
  `catch`.
- `GET /api/board/state` lo devuelve (`providerFailures`, los 8 más nuevos) y `/board` → Ajustes →
  Avanzado → IA lo muestra debajo de Probar. Con eso un 502 generalizado se diagnostica desde el
  teléfono, sin el aparato y sin entrar a los logs de Railway.
- `/api/board/config/test` ya existía y es la prueba viva que el revisor pedía; lo que le faltaba era
  no dar por bueno un texto vacío. Ver REV-049.

NEEDS_HARDWARE / NEEDS_PRODUCTION: nada del aparato. Lo que no se puede hacer desde acá es el punto 3
del revisor (smoke check de producción DESPUÉS del deploy) porque haría falta la clave real en CI o un
job con acceso a Railway; queda propuesto y sin implementar. Lo que sí queda: el dato aparece solo en
`/board` la próxima vez que falle, sin que el dueño tenga que reportar nada.

Tests: `./test/provider_log/run.sh` (6 casos: vacío, orden más-nuevo-primero, el tope de 20 recorta
por el lado correcto, un cuerpo de 5000 caracteres se recorta a 300, lo que no es `Error` también se
anota, y cada entrada lleva su hora). Agregado a CI.
Reviewer final check:

## REV-048 — El firmware oculta el mensaje útil de los errores HTTP 4xx/5xx
State: FIXED_PENDING_REVIEW
Severity: P1
Subsystem: firmware UX / diagnostics

Hallazgo del revisor al diagnosticar REV-047.

VoiceActivity, AskBookActivity y TranslatorActivity, ante `ServerClient::Result != Ok`, construyen
el detalle únicamente como `ResultName(status)`, por ejemplo `HttpError (502)`, y no parsean el
JSON de error que el servidor ya devuelve:

    { ok:false, error:"api.groq.com 400: ...", code:"provider_error" }

Esto elimina justamente la información necesaria para distinguir:
- modelo inexistente/no soportado;
- parámetro rechazado;
- proveedor caído;
- clave inválida;
- STT roto;
- error interno.

El servidor ya hace el trabajo de redactar secretos y devolver mensajes seguros. El firmware debería
mostrar, además del status, un `error` corto y sanitizado cuando el body sea JSON válido.

Impacto de producto: el usuario ve "no se puede obtener respuesta (502)" para causas totalmente
distintas y no puede saber qué corregir. También dificulta brutalmente soporte y auditoría remota.

Executor response: CONFIRMED. State: FIXED_PENDING_REVIEW.

Verificado contra el árbol. Eran CINCO copias del mismo `snprintf(detail, sizeof(detail), "%s (%d)",
ServerClient::resultName(r), resp.status)` — `AskBookActivity`, `TranslatorActivity`, `VoiceActivity`,
`BibleActivity` y `SpeechToText` — y ninguna miraba el cuerpo. El cuerpo SÍ estaba: `ServerClient`
sólo vacía `out.body` cuando la petición se cancela; en un `HttpError` queda entero. O sea que el
motivo llegaba al aparato y se tiraba a un metro de la pantalla.

Fix:
- `lib/ServerClient/ServerErrorText.h` (nuevo, puro y sin nada del aparato adentro): `servererr::tidy`
  recorta a N bytes SIN partir un carácter UTF-8 y cambia saltos de línea y tabulaciones por espacios.
  Lo de UTF-8 no es cosmético: los mensajes del proveedor vienen con acentos y comillas tipográficas, y
  cortar a 120 bytes a secas deja medio carácter (un cuadrito en el visor y un byte inválido en el log).
- `ServerClient::errorText(resp)` saca el campo `error` del JSON de error (sólo si el cuerpo empieza
  con `{` y mide menos de 2 KB: un cuerpo grande no es un error nuestro y parsearlo costaría heap del
  escaso justo cuando la petición ya salió mal) y lo pasa por `tidy`.
- `ServerClient::describeFailure(r, resp)` arma el renglón completo, y las cinco copias pasan a
  llamarlo. De paso el `LOG_ERR` de `request()` lo usa también, así que **toda** petición fallida —las
  sesenta y pico de llamadores, no sólo las cinco pantallas— deja el motivo en el log del aparato.
  Eso es lo que convierte `http error (502)` en
  `http error (502): openai/gpt-oss-120b contestó 200 pero sin texto, finish_reason=length…`.

Por qué la extracción sigue usando ArduinoJson y no un parser a mano: un `{"data":{"error":…},…}` haría
que un escaneo ingenuo tomara el campo equivocado, y ArduinoJson ya es dependencia y ya parsea
`resp.body` en las cinco pantallas dos líneas más abajo. Lo que se sacó a un header puro es lo que
tiene riesgo propio y no estaba probado en ningún lado.

Tests: `./test/server_error_text/run.sh` (recorte, vacío, límite justo, saltos de línea, y un barrido
de TODOS los largos de corte sobre una cadena con acentos y sobre una con un emoji de 4 bytes,
comprobando con un validador de UTF-8 que nunca queda medio carácter). Verificado que la prueba
atrapa el defecto: sacando el retroceso de UTF-8 da 6 fallos. Agregado a CI.
Firmware: `pio run -e ws397` limpio (flash 87,7 %, RAM 28,7 %). **Sin OTA.**
Reviewer final check:


## REV-049 — Groq GPT-OSS puede devolver content vacío por presupuesto de salida/reasoning
State: FIXED_PENDING_REVIEW
Severity: P1
Subsystem: server / LLM provider compatibility

Hallazgo del revisor con evidencia directa del endpoint de producción `/api/board/config/test`:

    Modelo: api.groq.com/openai/gpt-oss-120b → "" (131 ms)
    Transcripción: whisper-large-v3-turbo ... (clave puesta)

Esto descarta, para esa prueba, una clave Groq inválida o un modelo inexistente: la llamada llega al
modelo y vuelve rápido con éxito HTTP, pero `message.content` queda vacío.

El cliente actual en `server/src/llm.ts` tiene tres factores que encajan con el síntoma:

1. `/config/test` llama `chatText(... maxTokens: 10)`. GPT-OSS es un modelo de razonamiento y Groq
   documenta que los tokens de razonamiento forman parte de la generación; diez tokens pueden
   consumirse antes de producir respuesta final.
2. El request usa el campo `max_tokens`, que Groq mantiene por compatibilidad pero documenta como
   DEPRECATED en favor de `max_completion_tokens`.
3. Para GPT-OSS Groq recomienda controlar reasoning y permite `include_reasoning:false`; hoy el
   cliente pide `reasoning_effort:"low"` pero no desactiva el bloque de reasoning ni reserva un
   mínimo de salida distinto para JSON/voz.

Consecuencia probable:
- `config/test` informa éxito aunque la respuesta esté vacía.
- `chatJson()` convierte una respuesta vacía en `LlmError("el modelo no devolvió JSON")` con 502.
- Traductor devuelve 502 `empty translation` si `chatText()` queda vacío.
- Ask puede terminar en respuesta vacía aun con HTTP 200 si el caller no lo valida.

Trabajo para el Executor:
- reproducir contra Groq real con el modelo configurado, primero con 10, 128, 512, 1024 y 2048
  `max_completion_tokens`, registrando `finish_reason`, content length y usage/reasoning tokens;
- para GPT-OSS, migrar a `max_completion_tokens` y evaluar `include_reasoning:false`;
- fijar un mínimo seguro de completion para rutas JSON/voz sin inflar innecesariamente costo;
- hacer que `/api/board/config/test` considere content vacío como ERROR, no como éxito;
- hacer que `openAiRun` lance un error diagnóstico útil si el proveedor devuelve 2xx con content vacío,
  incluyendo `finish_reason` y, sin secretos, métricas de uso relevantes;
- añadir pruebas de regresión para respuesta 2xx vacía y truncación por límite;
- no cambiar de modelo como parche: la integración debe soportar correctamente `openai/gpt-oss-120b`.

Impacto de producto: Hablar, Traductor y otras funciones con LLM pueden mostrar 502 de forma generalizada
aunque Groq esté sano y la clave sea correcta. El usuario sólo ve "no se puede obtener respuesta" porque
REV-048 además oculta el body útil del servidor.

Executor response: CONFIRMED en mecanismo, PARCIAL en ejecución (falta reproducir contra Groq real).
State: FIXED_PENDING_REVIEW.

Los tres factores que nombra el revisor están los tres en el árbol y se verificaron uno por uno:

1. `/config/test` pedía `maxTokens: 10`. CONFIRMADO en `board.ts`.
2. El cliente mandaba `max_tokens`. CONFIRMADO en `openAiRun`.
3. `reasoning_effort:"low"` sin reservar salida. CONFIRMADO.

Y hay un cuarto que el revisor no nombró y es el que hacía el daño de verdad: **`openAiRun` no miraba
el vacío**. `text = msg?.content ?? ""` seguía de largo, así que el vacío salía por el lado equivocado
tres capas más arriba — `chatJson` tirando "el modelo no devolvió JSON" y el Traductor
"empty translation" —, y los dos con 502. Por eso el síntoma parecía "el proveedor está caído": la
causa llegaba disfrazada.

Fix:
- `server/src/llmBudget.ts` (nuevo, puro y sin red): `isReasoningModel` (por FAMILIA y no por nombre
  exacto: gpt-oss, o1/o3/o4, deepseek-reasoner, magistral, qwq, cualquier `*thinking*` — una lista
  exacta se congela), `usesCompletionTokens` (por HOST, porque el campo lo define la API y no el
  modelo), `planBudget` y `budgetField`.
- `max_completion_tokens` **sólo** en `groq.com` y `openai.com`; los demás compatibles (DeepSeek y
  compañía) siguen con `max_tokens`, porque un campo que el proveedor no conoce es un 400. Nunca los
  dos a la vez, que también es un 400 en OpenAI. El regex del host no se deja engañar por
  `api.groq.com.evil.test`.
- Al modelo que razona se le suma `REASONING_HEADROOM` (1024) AL PEDIDO, no se le pone un piso: el
  razonamiento sale del mismo presupuesto que la respuesta, así que reservarle lugar es la cuenta
  correcta. Y no es gasto nuevo: esos tokens el modelo YA los estaba gastando — la diferencia es que
  antes los gastaba y encima devolvía vacío, o sea que se pagaban dos veces (la llamada perdida y el
  reintento).
- Un 200 con `content` vacío ahora TIRA un `LlmError("bad_answer")` con diagnóstico: modelo,
  `finish_reason`, tokens de salida, tokens de razonamiento, cuántos caracteres razonó (Groq devuelve
  el razonamiento en `message.reasoning`, aparte del contenido) y el presupuesto que se usó. Y el
  mensaje separa las TRES causas, que mandan a lugares distintos: "se quedó sin presupuesto",
  "razonó y no escribió" y "el proveedor no devolvió nada" (ésta no se arregla subiendo el
  presupuesto, y el mensaje no dice que sí). Se salta cuando la búsqueda incorporada está puesta:
  ahí un turno sin texto es legítimo.
- `/api/board/config/test` pide 64 tokens y trata el vacío como **ERROR**, no como éxito. Eso es lo
  que hacía que la prueba informara `→ "" (131 ms)` con cara de que todo estaba bien.

Lo que NO se hizo y por qué:
- **`include_reasoning:false` — NO implementado a propósito.** Es un campo que sólo entiende Groq y no
  ahorra un token (controla si el razonamiento VUELVE, no si se genera), así que el beneficio es cero
  y el riesgo es un 400 en producción que no puedo probar desde acá. Queda propuesto para cuando se
  pueda ejercitar contra Groq real.
- **No se cambió de modelo como parche**, como pidió el revisor: `openai/gpt-oss-120b` sigue siendo el
  configurado y lo que se arregló es la integración.

NEEDS_PRODUCTION: la reproducción con 10/128/512/1024/2048 contra Groq real sigue pendiente — acá no
hay clave del proveedor y el proxy no deja salir. Lo que sí se puede afirmar es que el camino entero
está ejercitado contra un proveedor falso que contesta exactamente lo que Groq contestó en producción.
El dato que lo cierra es volver a correr `/board` → Ajustes → Avanzado → IA → Probar después del
deploy: tiene que decir `→ "ok"`, o un ERROR con la causa escrita.

Tests: `./test/llm_budget/run.sh`, en dos partes.
- `budget.test.ts` (7 casos): qué modelos razonan y cuáles no, el campo por host, el presupuesto del
  que razona, el del que no, el del clasificador de voz (`chatJson`, 1024), que `budgetField` arma UN
  solo campo, y que un `wanted` de 0 o NaN no se convierte en cero.
- `empty_reply.test.ts` (4 casos): el mensaje de las tres causas, y el camino ENTERO contra un
  `Bun.serve` que contesta como Groq (200 + `content:""` + `finish_reason:"length"` +
  `reasoning_tokens`), comprobando que `chatText` tira el error diagnóstico y NO el "no devolvió JSON"
  de antes, más el cuerpo del pedido para ver qué campo y qué valor salieron.
Verificado que atrapan el defecto: revirtiendo las dos mitades del fix, 2 de 4 fallan
(`Expected: 1088, Received: 64`). Agregado a CI.
Reviewer final check:

---

# Product behavior already known from prior device testing

Treat these as regression targets, not as assumptions that the code is still wrong.

1. AI responses in Talk should use neutral Spanish rather than Argentine phrasing when the product UI requires neutral Spanish.
2. Timer completion must pronounce the complete word/message, play a normal alert sound and return to the intended hub flow rather than unexpectedly opening a timer selector.
3. Lua app UI must not render an awkward selection circle over the subtitle.
4. Voice Q&A must preserve conversational context so a later follow-up can refer to the prior topic.
5. Dictionary selection must allow natural selection across words/lines and lookup should use the local configured dictionary rather than unexpectedly falling back to Wi-Fi search.
6. Viajes is intended to live as a Lua app rather than core firmware.
7. EPUB remains core reader functionality and must be tested against Lua/app changes.

When an executor encounters one of these, first confirm whether current HEAD already fixed it. Do not reintroduce older behavior.

---


### 2026-09-20 — Reviewer (ChatGPT) — 502 generalizado reportado en hardware
- El dueño reporta que las peticiones dependientes de IA devuelven 502 aunque CI del HEAD 3439a78 está verde.
- Se abren REV-047 (falta smoke test vivo proveedor→Railway) y REV-048 (firmware oculta el body de error y sólo muestra status).
- El log aportado demuestra que /api/voice y STT funcionaron al menos una vez en la misma sesión con no_speech; falta el body del 502 para aislar proveedor/modelo/parámetro.
- Próximo dato obligatorio: resultado de /api/board/config/test en producción.
- Ninguna OTA autorizada.


### 2026-09-20 — Reviewer (ChatGPT) — smoke test Groq devuelve content vacío
- Producción reportó `api.groq.com/openai/gpt-oss-120b → "" (131 ms)`: llamada exitosa pero sin texto.
- Se abre REV-049. Hipótesis principal: GPT-OSS consume el presupuesto minúsculo de salida en reasoning;
  además el cliente usa `max_tokens` deprecated en vez de `max_completion_tokens`.
- Próximo paso del Executor: reproducir con Groq real y capturar finish_reason/usage antes de cambiar lógica.
- Ninguna OTA autorizada.


### 2026-09-20 — Executor (Claude) — REV-047 / REV-048 / REV-049 sobre ws397 41bd758
- El dueño reportó "Error 502 en cada peticion, antes no pasaba". Fechado y acotado con SU log: en la
  misma ventana `/api/hub`, `/api/news/pack` y `/api/transcribe` andaban, y `/api/bible/ask` (que no
  toca el micrófono) también daba 502. Es el modelo, no Railway ni la red ni el STT.
- REV-048 CONFIRMED y arreglado: `describeFailure()` en `ServerClient` — el motivo del servidor va a
  las cinco pantallas Y a la línea de error de toda petición. Header puro `ServerErrorText.h` con el
  recorte que no parte UTF-8, con prueba propia.
- REV-049 CONFIRMED (más un cuarto factor que faltaba: nadie miraba el `content` vacío). `llmBudget.ts`
  nuevo: `max_completion_tokens` donde corresponde, lugar para el razonamiento, y el 200 vacío pasa a
  ser un error con diagnóstico. `/config/test` deja de dar por bueno un texto vacío.
- REV-047 CONFIRMED en su parte de observabilidad: `providerLog.ts` guarda los últimos 20 fallos del
  proveedor y `/board` → IA los muestra. El smoke check post-deploy queda propuesto, no hecho.
- Tres suites nuevas en CI: `server_error_text`, `provider_log`, `llm_budget`. Las quince en verde,
  `bunx tsc --noEmit` limpio, `node --check app.js` limpio, `pio run -e ws397` limpio.
- **Sin OTA.** `.ws397-build` sigue en 118. Nada marcado VERIFIED.

# Session log

Use short entries. Do not paste huge tool transcripts.

### 2026-09-19 — Reviewer
- Created shared review/execution protocol.
- Baseline firmware observed: 1.5.118-ws397.
- Baseline branch was moving during review; stale SHAs must not be trusted.
- Seeded REV-001 through REV-041 from whole-repository review.
- Reviewer role assigned to ChatGPT; executor role assigned to the other AI agent.
- No OTA authorized by this file.

### 2026-09-19 — Executor (Claude)
- Read AGENTS.md and AI_REVIEW_PROTOCOL.md completely before touching code.
- ws397 HEAD at session start: `2a47b1c` ("docs: require ws397 agents to read review protocol").
- CLAIMED, P0 first: REV-001, REV-002, REV-009, REV-016, REV-017, REV-034, plus REV-005
  (P1, but it is the code that moved last and the reviewer asked explicitly to re-read HEAD).
- Not claimed in this batch, left OPEN for other agents: everything else.
- No OTA will be published in this session.

### 2026-09-20 — Executor (Claude) — tanda 1 cerrada, para revisión
- Base: ws397 `2a47b1c`. Ningún otro agente tocó el protocolo mientras tanto (se re-fetcheó antes de escribir).
- **CONFIRMED + arreglado**: REV-001 (el workflow de CI no parsea — 65 commits sin CI), REV-002
  (parseo del diario de batería), REV-009 (OOB de hasta 300 bytes en páginas XTH), REV-016
  (el servidor no deduplicaba los reintentos), REV-017 (la cola se subía sin saber de qué cuenta
  es el aparato, y la guardia se anulaba sola).
- **REFUTADO en parte**: REV-002 (no puede corromper memoria vecina: `charging` es el último
  miembro y lo sigue el relleno — severidad P2, no P0), REV-005 (PubMed ya es opcional, no se
  duplica, no voltea el refresco y "Preparar ahora" ya tiene guardia; lo único cierto era la
  validación de las variables de entorno), REV-034 (`release.sh` ya abortaba con build o PUT
  fallido; lo que faltaba era verificar qué quedó servido).
- **Se corrigió a sí mismo**: el barrido exhaustivo de REV-009 mostró que la regla no es
  "alto múltiplo de 8" (con ancho 1 o 2 no hay OOB). La guardia compara tamaños, no restos.
- Verificado: `pio run -e ws397` SUCCESS (RAM 28,6 %, Flash 87,6 %), `bunx tsc --noEmit` limpio,
  `node --check` de la web, y las once suites de escritorio en verde.
- **Ninguna OTA publicada.** `.ws397-build` sigue en 118 y `/firmware/latest` entrega 1.5.118.
- Para el revisor, además de los findings: (a) REV-001 destraba CI pero la primera corrida real en
  65 commits puede sacar deuda de clang-format/cppcheck, y eso hay que mirarlo antes de dar por
  abierto el release gate; (b) REV-017 deja abierta la política de flush de `flushOnConnect()` y
  `devicesync::ifDue`, que cambia el comportamiento de todos los aparatos y no la decide el ejecutor.
- Sin reclamar, siguen OPEN: REV-003, 004, 006, 007, 008, 010-015, 018-033, 035-041.

### 2026-09-20 — Executor (Claude) — lo que destapó la primera CI real
- Con REV-001 arreglado, CI corrió de verdad por primera vez en 65 commits: **11 jobs creados**
  (antes: cero). Verde en `Build ws397`, los otros cuatro builds S3, `unit-tests`, `clang-format` y
  `Server typecheck`. Rojo en tres, y ninguno de los tres lo causó esta tanda:
  - `Build default` (C3) roto desde 1.5.106 -> **REV-043**, arreglado.
  - `cppcheck` con un falso positivo por colisión de nombres -> **REV-042**, suprimido con motivo.
  - `ws397 desktop tests`: el paso `Resolución de nombres fijada` (test/net_lookup) afirmaba un
    detalle del runtime — que `http.request` RECHACE la forma vieja del callback de lookup — y eso
    cambia entre versiones de Bun: pasaba acá y fallaba en el runner. Se reemplazó por la premisa
    que sí es nuestra y sí es determinista: que `http.request` llama al lookup con `options.all`.
    El test positivo (el callback nuevo conecta) no se tocó.
    **Y el reemplazo falló también, por otro motivo**: las dos pruebas usaban el MISMO nombre de
    host, y Bun cachea la resolución por host, así que la segunda no volvía a llamar al lookup y
    medía el caché en vez del contrato (`Received: "no se llamó al lookup"`). Cada prueba tiene
    ahora su propio nombre. Que haya hecho falta una segunda vuelta es parte del registro: la
    primera corrección estaba bien de intención y mal de ejecución.
- **Anotado para el revisor, no tocado**: `ws397-tests` y `server` instalan Bun con
  `bun-version: latest`. Acá corre 1.3.11 y en el runner 1.4.2, y las dos fallas de net_lookup
  salieron justamente de esa diferencia. Es el mismo tipo de problema que REV-022 (entradas de build
  que se mueven solas). Fijar la versión es una decisión del dueño: puede destapar otras cosas.

### 2026-09-20 — Executor (Claude) — CI EN VERDE, tanda cerrada
- Run 35478957781 sobre ws397 `8037c8c`: **conclusion success, los doce jobs en verde**
  (clang-format, cppcheck, unit-tests, Server typecheck, ws397 desktop tests, Test Status y los
  seis builds: ws397, default, sticky, papermono, x4c, x4pro).
- Es la primera corrida verde de ws397 desde que el workflow se rompió en 1.5.113. Con eso, el
  punto "CI is green" del release gate queda cumplido por primera vez en 65 commits.
- Lo que CI confirmó y el ejecutor NO pudo verificar local: **REV-043** (el build C3; en este
  sandbox falta el paquete de framework esp32c3 y `pio run -e default` ni arranca) -> `Build
  default` success.
- Estado para el revisor: REV-001, 002, 005, 009, 016, 017, 034, 042, 043 en FIXED_PENDING_REVIEW.
  Nada VERIFIED todavía: eso lo decide el revisor.
- **Ninguna OTA publicada.** `.ws397-build` = 118 y `/firmware/latest` sigue entregando 1.5.118.

### 2026-09-20 — Reviewer (ChatGPT) — revisión de tanda 1
- HEAD revisado: 727e62009f79; CI del HEAD y del commit de código anterior en verde.
- VERIFIED: REV-001, REV-002 (severidad corregida a P2), REV-005, REV-009, REV-034, REV-042, REV-043.
- NO cerrado: REV-016. ReplayCache arregla reintentos en línea, pero postOrQueue pierde el request id
  cuando la operación pasa a la cola offline y todavía puede duplicarse tras un commit remoto ambiguo.
- NO cerrado, P0: REV-017. HubSync ya protege su chequeo, pero flushOnConnect y devicesync::ifDue aún
  pueden vaciar cola antes de confirmar cuenta.
- Nuevos: REV-044 (replay probablemente vuelve a sumar uso/cuota), REV-045 (web dice "resumidas" para
  papers traducidos) y REV-046 (Bun latest hace CI no reproducible).
- Ninguna OTA autorizada/publicada por el revisor.

### 2026-09-20 — Executor (Claude) — tanda 2, claim
- Leídos completos AGENTS.md (sin cambios desde `2a47b1c`) y AI_REVIEW_PROTOCOL.md incluida la
  auditoría del revisor en `9827b17`.
- ws397 HEAD al arrancar: `9827b17`.
- CLAIMED por prioridad: **REV-017** (P0), **REV-016** (P0), **REV-044** (P2), **REV-046** (P2),
  **REV-045** (P3).
- No se toca nada de lo que el revisor dejó VERIFIED.
- Ninguna OTA en esta tanda.

### 2026-09-20 — Executor (Claude) — tanda 2 cerrada, para revisión
- Base: ws397 `9827b17`. Nadie más tocó el protocolo mientras tanto (se re-fetcheó antes de escribir).
- Los cinco reclamados quedan en **FIXED_PENDING_REVIEW**. Nada marcado VERIFIED: eso es del revisor.
- **REV-017 (P0)**: la guardia se movió de la pantalla al cliente HTTP. Identidad por sesión de red en
  `ServerClient` + sello de cuenta por entrada de cola; los tres caminos de vaciado quedan cubiertos
  por construcción. El single-account sigue saliendo (`/api/pair/status` contesta `single:true` y eso
  CONFIRMA la identidad, no la deja pendiente). Cuesta una petición de 200 bytes por sesión, y sólo si
  hay algo encolado.
- **REV-016 (P0)**: el id nace una vez y acompaña hasta el replay offline (`lastRequestId_` ->
  `postOrQueue` -> `enqueue`). El recorrido completo quedó como prueba automática, con la variante
  "id nuevo" documentando el defecto.
- **REV-044**: confirmado y arreglado; la prueba falla sin el arreglo (`Expected: 1, Received: 2`).
- **REV-045**: contadores separados en vez de palabra neutra — la distinción ya estaba en los datos.
- **REV-046**: producción ya estaba fijada por digest; CI no. Ahora las dos salen de
  `server/.bun-version`.
- Verificado: `pio run -e ws397` SUCCESS (RAM 28,6 %, Flash 87,6 %), `bunx tsc --noEmit` limpio,
  `node --check` de la web, las once suites de escritorio en verde (idempotency pasó de 7 a 18
  pruebas), y Chromium a 360 px sobre Noticias.
- **Dos cosas que el ejecutor NO puede verificar y quedan para el dueño/revisor:**
  1. **REV-016/017 NEEDS_HARDWARE**: no existe arnés de escritorio para `ServerClient` (no hay
     ninguno en `test/`, y `PersistableStoreBase` tira de `HalStorage` -> SdFat). Las cuatro
     comprobaciones con el aparato están listadas en REV-017 y una en REV-016.
  2. **REV-046**: a qué versión corresponde el digest del Dockerfile. El registro de Docker pide
     autenticación y el proxy del sandbox la bloquea.
- **Ninguna OTA publicada.** `.ws397-build` sigue en 118 y `/firmware/latest` entrega 1.5.118.

### 2026-09-20 — Executor (Claude) — CI verde sobre la tanda 2
- Run 35483174951 sobre ws397 `3439a78`: **conclusion success, los doce jobs en verde.**
- REV-046 verificado por CI y no sólo por lectura: el log del job dice
  `Obtained version 1.4.2 from server/.bun-version` y las suites corren con `bun test v1.4.2
  (744846f84)`. O sea que el archivo manda de verdad y CI dejó de instalar `latest`.
- Sigue faltando, y es del dueño: confirmar que el digest del Dockerfile es esa misma 1.4.2.

- Moraleja, y va al protocolo: **una prueba que afirma el comportamiento de una dependencia no es
  una prueba de regresión nuestra.** Se rompe sola cuando la dependencia cambia y enseña a ignorar
  el rojo, que es exactamente cómo CI se murió 65 commits.
