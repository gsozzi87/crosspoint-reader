// API del aparato: todo lo que cuelga de /api exige el token del aparato
// (Authorization: Bearer $DEVICE_TOKEN). Es un token propio del lector, distinto
// del OTA_TOKEN (ese solo sube firmware). Las features se montan acá adentro y
// heredan el chequeo. En index.ts: app.route("/api", api);
import { Hono } from "hono";
import { ask } from "./ask";
import { transcribe } from "./transcribe";
import { hub } from "./hub";
import { voice } from "./voice";
import { tts } from "./tts";
import { translate } from "./translate";
import { boardApi } from "./board";
import { bibleApi } from "./bible";

const TOKEN = process.env.DEVICE_TOKEN ?? "";

export const api = new Hono();

api.use("*", async (c, next) => {
  const auth = c.req.header("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
  if (!TOKEN || token !== TOKEN) return c.json({ ok: false, error: "unauthorized" }, 401);
  await next();
});

// Lo usa Settings -> Prueba de servidor. X-Request-Id viene en cada pedido del
// aparato (estable entre reintentos); por ahora solo lo devolvemos.
api.get("/ping", (c) => c.json({ ok: true, now: Date.now(), requestId: c.req.header("x-request-id") ?? null }));

api.route("/ask", ask);              // POST /api/ask         → Claude sobre el capítulo
api.route("/transcribe", transcribe); // POST /api/transcribe  → voz a texto (Whisper)
api.route("/hub", hub);               // GET  /api/hub?lang=   → clima, recordatorios, listas, agenda, mensajes, frase; POST /api/hub/done
api.route("/voice", voice);           // POST /api/voice       → una grabación: el servidor decide qué es y lo hace
api.route("/tts", tts);               // GET  /api/tts?text=   → voz Piper en ADPCM (avisos que el aparato cachea en la SD)
api.route("/translate", translate);   // POST /api/translate?from=&to= → traductor en conversación: texto + traducción + voz
api.route("/board", boardApi);        // POST /api/board/{message,reminder,item,note} → lo que se carga desde la página web
api.route("/bible", bibleApi);        // GET  /api/bible/{books,chapter,day,find} → Biblia por capítulos, versículo del día, búsqueda
