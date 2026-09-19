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
State: OPEN
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

Executor response:
Reviewer final check:

## REV-002 — BatteryLog parses bool through int*
State: OPEN
Severity: P0
Subsystem: battery / memory safety

Inspect src/util/BatteryLog.cpp readAll().

Observed pattern parses CSV using sscanf and casts the address of Sample::charging, a bool, to int*. Writing %d through int* to bool storage is undefined behavior and may overwrite adjacent bytes. The time_t-to-long cast must also not be assumed safe on every target.

Parse into correctly typed temporaries and then assign to Sample.

Tests should include charging=0, charging=1, modern timestamps and post-2038 timestamps.

Executor response:
Reviewer final check:

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
State: OPEN
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

Executor response:
Reviewer final check:

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
State: OPEN
Severity: P0/P1
Subsystem: XTC reader

Inspect src/activities/reader/XtcReaderActivity.cpp.

For bitDepth 2, allocation was observed to use approximately ceil(width*height/8) per plane while pixel access appears column-major using:

    colBytes = ceil(height/8)
    offset = column * colBytes + byteInColumn

For height not divisible by 8, width * ceil(height/8) can exceed ceil(width*height/8).

Normal device dimensions may hide this, but malformed or unusual XTC dimensions could read out of bounds.

Validate dimensions and fuzz odd widths/heights.

Executor response:
Reviewer final check:

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
State: OPEN
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

Executor response:
Reviewer final check:

## REV-017 — Offline queue + account reassignment
State: OPEN
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

Executor response:
Reviewer final check:

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
State: OPEN
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
State: OPEN
Severity: P0 release gate
Subsystem: release

Do not assume generic release workflows publish WS397.

Audit release.ps1, release.sh, /firmware, .ws397-build and include/ws397_version.h.

Desired sequence:
version increment -> successful build -> tests -> upload -> verify /firmware/latest -> release considered published.

Avoid repository claiming version N while Railway still serves N-1 without an explicit warning.

Executor response:
Reviewer final check:

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

# Session log

Use short entries. Do not paste huge tool transcripts.

### 2026-09-19 — Reviewer
- Created shared review/execution protocol.
- Baseline firmware observed: 1.5.118-ws397.
- Baseline branch was moving during review; stale SHAs must not be trusted.
- Seeded REV-001 through REV-041 from whole-repository review.
- Reviewer role assigned to ChatGPT; executor role assigned to the other AI agent.
- No OTA authorized by this file.

### Executor
Add your first entry here after reading this file. Include current ws397 HEAD and which REV IDs you claim.
