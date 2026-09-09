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
import { auth, seedFromFiles } from "./accounts";
import { initDb, multiUser } from "./db";

const app = new Hono();

// Sin esto, cualquier excepción sale como el "Internal Server Error" en texto
// plano de Hono y el aparato revienta al parsearlo como JSON.
app.onError((err, c) => {
  console.error(`error en ${c.req.method} ${c.req.path}:`, err);
  const msg = redactSecrets(err instanceof Error ? err.message : String(err)).slice(0, 200);
  return c.json({ ok: false, error: msg || "internal", code: "internal" }, 500);
});

app.notFound((c) => c.json({ ok: false, error: "not found", code: "not_found" }, 404));

// La base de datos, si la hay. SIN `DATABASE_URL` esto no hace nada y el
// servidor arranca igual que siempre: archivos en /data y un solo DEVICE_TOKEN.
// Con `DATABASE_URL` crea las tablas que falten y, la primera vez, migra lo que
// había en /data a la cuenta 1 (ver `seedFromFiles`).
await initDb();
await seedFromFiles().catch((err) => console.error("db: no se pudo migrar /data", err));

app.get("/", (c) => c.text("ws397 server ok"));
app.route("/auth", auth);   // registro, login, logout y quién soy (solo con base de datos)
app.route("/firmware", firmware);
app.route("/board", board);
app.route("/board/log", logPage);  // log del aparato, texto plano  // página web para el teléfono (pide el token del aparato)
app.route("/api", api);

const hubLang = normalizeLang(process.env.HUB_LANG ?? "es");
warmUp(hubLang);       // Piper carga el modelo una vez
warmAssets(hubLang);   // genera el paquete de contenido si falta (una sola vez, queda en /data)

const port = Number(process.env.PORT ?? 3000);
console.log(`ws397 server on :${port} (${multiUser ? "multiusuario, Postgres" : "un solo usuario, archivos en /data"})`);

export default { port, fetch: app.fetch };
