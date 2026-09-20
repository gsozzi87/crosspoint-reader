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
import { normalizeLang } from "./lang";
import { addUsage, audioSeconds, overQuota, QUOTA_CODE, QUOTA_MSG } from "./usage";
import { cacheable, ReplayCache } from "./idempotency";

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

// ── Tope mensual ────────────────────────────────────────────────────────────
// Lo único que se cobra es lo que le cuesta plata al operador: el modelo y la
// transcripción. Pasado el tope, estas cuatro rutas contestan 429 en el idioma
// del pedido y TODO lo demás (hub, calendario, biblia, música, fotos, noticias)
// sigue andando. Sin `DATABASE_URL` o sin topes puestos, `overQuota` es false y
// esto no hace nada.
// Una tabla y no una lista de rutas: hay rutas que gastan modelo pero NO mandan
// audio, y con la lista pelada los 40 KB de JSON de un capítulo de la Biblia se
// cobraban como segundos de audio. `audio` dice si el cuerpo es una toma del
// micrófono; `llm` cuántas llamadas al modelo hace la ruta.
//
// Las que faltaban: /api/bible/ask (chatText con hasta 40 KB de capítulo, la
// llamada más cara del sistema) y /api/calendar/dictate (chatJson). Las dos
// pasaban por afuera del tope y no sumaban nada. (/api/suggest se fue en 1.5.108
// con las sugerencias de Mi día, que eran de los viajes.)
const METERED: { path: string; llm: number; audio: boolean }[] = [
  { path: "/api/ask", llm: 1, audio: false },
  { path: "/api/voice", llm: 1, audio: true },
  { path: "/api/transcribe", llm: 0, audio: true },
  { path: "/api/translate", llm: 1, audio: true },
  { path: "/api/bible/ask", llm: 1, audio: false },
  { path: "/api/calendar/dictate", llm: 1, audio: false },
];

api.use("*", async (c, next) => {
  const path = c.req.path;
  const m = METERED.find((e) => path === e.path || path.startsWith(`${e.path}/`));
  if (!m) return next();
  const acc = accountOf(c);
  if (await overQuota(acc)) {
    const lang = normalizeLang(c.req.query("lang"));
    return c.json({ ok: false, error: QUOTA_MSG[lang], code: QUOTA_CODE }, 429);
  }
  const declaredBytes = Number(c.req.header("content-length") ?? 0) || 0;
  const type = c.req.header("content-type");
  await next();
  const bytes = m.audio ? c.get("bodyBytes") ?? declaredBytes : 0;
  // Solo se cobra lo que salió bien: un 502 del proveedor no se le carga a nadie.
  if (c.res.status < 400) {
    void addUsage(acc, { llm: m.llm, sttSeconds: m.audio ? audioSeconds(bytes, type) : 0 });
  }
});

// ── Reintentos: un POST no se aplica dos veces ──────────────────────────────
// El aparato manda el MISMO `X-Request-Id` en los tres intentos de una
// petición. Si el primero se aplicó y la respuesta se perdió (conexión cortada,
// 5xx con el trabajo ya hecho), el reintento tiene que recibir la respuesta
// guardada y no volver a ejecutar el handler. Ver idempotency.ts.
const replays = new ReplayCache();

api.use("*", async (c, next) => {
  if (c.req.method !== "POST") return next();
  const id = (c.req.header("x-request-id") ?? "").trim();
  if (!id) return next();  // la web no lo manda; ahí no hay reintento automático
  const key = ReplayCache.key(accountOf(c), c.req.path, id);
  const hit = replays.get(key);
  if (hit) {
    console.log(`replay ${c.req.path} (${id}): se devuelve la respuesta guardada, no se aplica de nuevo`);
    return new Response(hit.body.slice().buffer as ArrayBuffer, {
      status: hit.status,
      headers: { "content-type": hit.type },
    });
  }
  await next();
  // `clone()` porque el cuerpo de una Response se lee una sola vez y el que
  // tiene que recibirlo es el aparato.
  const copy = c.res.clone();
  const body = new Uint8Array(await copy.arrayBuffer());
  if (!cacheable(c.res.status, body.byteLength)) return;
  replays.put(key, {
    status: c.res.status,
    type: c.res.headers.get("content-type") ?? "application/octet-stream",
    body,
    at: Date.now(),
  });
});

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
