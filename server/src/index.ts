// Punto de entrada. Railway: Root Directory = server, start = bun run src/index.ts.
import { Hono } from "hono";
import { api } from "./api";
import { firmware } from "./firmware";
import { board } from "./board";
import { logPage } from "./devicelog";
import { warmUp } from "./tts";
import { warmAssets } from "./assets";
import { normalizeLang } from "./lang";
import { redactSecrets } from "./net";

const app = new Hono();

// Sin esto, cualquier excepción sale como el "Internal Server Error" en texto
// plano de Hono y el aparato revienta al parsearlo como JSON.
app.onError((err, c) => {
  console.error(`error en ${c.req.method} ${c.req.path}:`, err);
  const msg = redactSecrets(err instanceof Error ? err.message : String(err)).slice(0, 200);
  return c.json({ ok: false, error: msg || "internal", code: "internal" }, 500);
});

app.notFound((c) => c.json({ ok: false, error: "not found", code: "not_found" }, 404));

app.get("/", (c) => c.text("ws397 server ok"));
app.route("/firmware", firmware);
app.route("/board", board);
app.route("/board/log", logPage);  // log del aparato, texto plano  // página web para el teléfono (pide el token del aparato)
app.route("/api", api);

const hubLang = normalizeLang(process.env.HUB_LANG ?? "es");
warmUp(hubLang);       // Piper carga el modelo una vez
warmAssets(hubLang);   // genera el paquete de contenido si falta (una sola vez, queda en /data)

const port = Number(process.env.PORT ?? 3000);
console.log(`ws397 server on :${port}`);

export default { port, fetch: app.fetch };
