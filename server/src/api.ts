// API del aparato: todo lo que cuelga de /api exige identificarse. Es un token
// propio del lector, distinto del OTA_TOKEN (ese solo sube firmware). Las
// features se montan acá adentro y heredan el chequeo. En index.ts:
// app.route("/api", api);
//
// Quién puede entrar (ver `tenant.ts`):
//   - el aparato con su `Authorization: Bearer <token>`;
//   - la web con la cookie de sesión (modo multiusuario);
//   - el `DEVICE_TOKEN` del entorno y el `config.deviceToken`, que valen SIEMPRE
//     y son la cuenta 1 — es lo que hace que el aparato que ya estaba andando
//     siga andando sin tocarle nada.
// Sin `DATABASE_URL` no hay cuentas: el token de siempre es lo único que hay y
// todo es la cuenta 1, exactamente como hasta hoy.
import { Hono } from "hono";
import { ask } from "./ask";
import { transcribe } from "./transcribe";
import { hub } from "./hub";
import { voice } from "./voice";
import { tts } from "./tts";
import { translate } from "./translate";
import { boardApi } from "./board";
import { bibleApi } from "./bible";
import { news } from "./news";
import { rss } from "./rss";
import { deviceLog } from "./devicelog";
import { assets } from "./assets";
import { calendar } from "./calendar";
import { notes } from "./notes";
import { apps } from "./apps";
import { accountApi, pairStatus, startPairing } from "./accounts";
import { readBody } from "./net";
import { accountOf, bearerOf, requireTenant, type AppEnv } from "./tenant";
import { withTimeZone } from "./store";

import { idempotency, ReplayCache } from "./idempotency";
import { metering } from "./metering";

export const api = new Hono<AppEnv>();

// ── Vinculación ─────────────────────────────────────────────────────────────
// Estas DOS rutas van ANTES del middleware a propósito: Hono corre los handlers
// en el orden en que se registraron, así que estas contestan sin pasar por el
// chequeo de token. Es la única forma de que un aparato recién sacado de la caja
// —que todavía no está en ninguna cuenta— pueda pedir su código.
//
//   POST /api/pair/start   { deviceId, token }  -> { ok, code, expiresIn }
api.post("/pair/start", async (c) => {
  const b = await readBody(c);
  const r = await startPairing(b.deviceId, b.token);
  if (!r.ok) return c.json({ ok: false, error: r.error, code: r.code }, r.status);
  console.log(`pair: código emitido para el aparato ${String(b.deviceId).slice(0, 32)}`);
  return c.json({ ok: true, code: r.code, expiresIn: r.expiresIn });
});

//   GET /api/pair/status   (Bearer del aparato) -> { ok, paired, account }
// El aparato la consulta cada pocos segundos mientras muestra el código, y
// todavía no tiene cuenta: por eso tampoco pasa por el middleware.
api.get("/pair/status", async (c) => {
  const st = await pairStatus(bearerOf(c));
  return c.json({ ok: true, ...st });
});

// ── De acá para abajo hay que identificarse ─────────────────────────────────
api.use("*", requireTenant);

// La zona horaria del pedido es la de la cuenta (el lugar que eligió para el
// clima). Se deja puesta para todo lo sincrónico de adentro (localToEpoch y
// compañía); ver `withTimeZone` en store.ts.
api.use("*", async (c, next) => withTimeZone(accountOf(c), () => next()));

// ── Tope mensual y reintentos ───────────────────────────────────────────────
// Los dos middlewares viven en su propio archivo (metering.ts, idempotency.ts)
// y acá sólo se los enchufa, EN ESTE ORDEN.
//
// El orden importa y es la mitad del arreglo de REV-044: medición por AFUERA
// —tiene que poder contestar 429 antes de tocar nada— e idempotencia por
// adentro. Como la de afuera sólo ve el estado de la respuesta, un replay le
// parecía un 2xx normal y volvía a cobrar consumo por un pedido que no ejecutó
// nada. Ahora el replay deja una marca en el contexto (`isReplay`) y la
// medición la mira.
api.use("*", metering());

const replays = new ReplayCache();
api.use("*", idempotency(replays, accountOf));

// Lo usa Settings -> Prueba de servidor.
api.get("/ping", (c) => c.json({ ok: true, now: Date.now(), requestId: c.req.header("x-request-id") ?? null }));

api.route("/ask", ask);              // POST /api/ask         → Claude sobre el capítulo
api.route("/transcribe", transcribe); // POST /api/transcribe  → voz a texto (Whisper)
api.route("/hub", hub);               // GET  /api/hub?lang=   → clima, recordatorios, listas, agenda, notas, frase; POST /api/hub/done
api.route("/voice", voice);           // POST /api/voice       → una grabación: el servidor decide qué es y lo hace
api.route("/tts", tts);               // GET  /api/tts?text=   → voz Piper en ADPCM (avisos que el aparato cachea en la SD)
api.route("/translate", translate);   // POST /api/translate?from=&to= → traductor en conversación: texto + traducción + voz
api.route("/board", boardApi);        // POST /api/board/{reminder,item,note} → lo que se carga desde la página web
api.route("/notes", notes);          // POST /api/notes → nota rápida, sin pasar por el clasificador
api.route("/bible", bibleApi);        // GET  /api/bible/{books,chapter,day,find}; POST /api/bible/ask → Biblia y preguntas sobre el capítulo
api.route("/news", news);             // paquete masticado, estado y actualización manual
api.route("/rss", rss);               // GET  /api/rss, /api/rss/article → noticias de los feeds cargados en /board
api.route("/calendar", calendar);  // GET /api/calendar, /day, /repeat; POST /api/calendar/event, /event/delete, /dictate → calendario local
api.route("/assets", assets);   // GET /api/assets/manifest, /file, /status → paquete de contenido (Biblia, tarjetas, sonidos)
api.route("/account", accountApi);  // POST /api/account/pair, /device/rename, /device/delete, /password (desde la web)
api.route("/log", deviceLog);         // GET/POST/DELETE /api/log → el aparato sube su log; se lee en /board/log
// POST /api/apps/call {app, service, args}; GET /api/apps/file/:id → la puerta
// de las apps de Lua (cp.call / cp.download): servicios con nombre, trabajos
// largos por jobId y los archivos que generan (Librito → EPUB).
api.route("/apps", apps);
