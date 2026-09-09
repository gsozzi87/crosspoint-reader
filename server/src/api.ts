// API del aparato: todo lo que cuelga de /api exige el token del aparato
// (Authorization: Bearer $DEVICE_TOKEN). Es un token propio del lector, distinto
// del OTA_TOKEN (ese solo sube firmware). Las features se montan acá adentro y
// heredan el chequeo. En index.ts: app.route("/api", api);
import { Hono } from "hono";
import { config } from "./config";
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

const ENV_TOKEN = process.env.DEVICE_TOKEN ?? "";

export const api = new Hono();

// Vale el token del entorno (el de siempre) y también el que se haya puesto
// desde la web. Los dos: así cambiar el token en /board nunca deja a nadie
// afuera si el aparato todavía tiene el viejo.
api.use("*", async (c, next) => {
  const auth = c.req.header("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
  const extra = (await config()).deviceToken;
  const ok = !!token && ((ENV_TOKEN && token === ENV_TOKEN) || (extra && token === extra));
  if (!ok) return c.json({ ok: false, error: "unauthorized" }, 401);
  await next();
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
api.route("/log", deviceLog);         // POST /api/log → el aparato sube su log; se lee en /board/log         // GET  /api/photos, /api/photos/file?id= → álbum (BMP 2 bpp ya convertido)
