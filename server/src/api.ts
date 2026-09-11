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
import { rss } from "./rss";
import { photos } from "./photos";
import { deviceLog } from "./devicelog";
import { assets } from "./assets";
import { tripApi, tripsApi } from "./trips";
import { attachmentApi } from "./attachments";
import { calendar } from "./calendar";
import { suggest } from "./suggest";
import { notes } from "./notes";
import { accountApi, pairStatus, startPairing } from "./accounts";
import { readBody } from "./net";
import { accountOf, bearerOf, requireTenant, type AppEnv } from "./tenant";
import { withTimeZone } from "./store";
import { normalizeLang } from "./lang";
import { addUsage, audioSeconds, overQuota, QUOTA_CODE, QUOTA_MSG } from "./usage";

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
  console.log(`pair: código ${r.code} para el aparato ${String(b.deviceId).slice(0, 32)}`);
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
const METERED = ["/api/ask", "/api/voice", "/api/transcribe", "/api/translate"];

api.use("*", async (c, next) => {
  const path = c.req.path;
  if (!METERED.some((p) => path === p || path.startsWith(`${p}/`))) return next();
  const acc = accountOf(c);
  if (await overQuota(acc)) {
    const lang = normalizeLang(c.req.query("lang"));
    return c.json({ ok: false, error: QUOTA_MSG[lang], code: QUOTA_CODE }, 429);
  }
  const bytes = Number(c.req.header("content-length") ?? 0) || 0;
  const type = c.req.header("content-type");
  await next();
  // Solo se cobra lo que salió bien: un 502 del proveedor no se le carga a nadie.
  if (c.res.status < 400) {
    void addUsage(acc, {
      llm: path.startsWith("/api/transcribe") ? 0 : 1,
      sttSeconds: path.startsWith("/api/ask") ? 0 : audioSeconds(bytes, type),
    });
  }
});

// Lo usa Settings -> Prueba de servidor. X-Request-Id viene en cada pedido del
// aparato (estable entre reintentos); por ahora solo lo devolvemos.
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
api.route("/rss", rss);               // GET  /api/rss, /api/rss/article → noticias de los feeds cargados en /board
api.route("/photos", photos);
api.route("/calendar", calendar);  // GET /api/calendar, /day, /repeat; POST /api/calendar/event, /event/delete, /dictate → calendario local
api.route("/assets", assets);   // GET /api/assets/manifest, /file, /status → paquete de contenido (Biblia, tarjetas, sonidos)
api.route("/trips", tripsApi);       // GET  /api/trips?lang= → lista de viajes
api.route("/trip", tripApi);         // GET  /api/trip?id= y los POST de días, ítems, para llevar y adjuntos
api.route("/suggest", suggest);        // GET /api/suggest/day, /trip → sugerencias del día y del viaje (cacheadas, con tope diario)
api.route("/attachment", attachmentApi);  // GET /api/attachment?id=&page= → el bitmap listo para pintar; /info → texto extraído
api.route("/account", accountApi);  // POST /api/account/pair, /device/rename, /device/delete, /password (desde la web)
api.route("/log", deviceLog);         // POST /api/log → el aparato sube su log; se lee en /board/log         // GET  /api/photos, /api/photos/file?id= → álbum (BMP 2 bpp ya convertido)
