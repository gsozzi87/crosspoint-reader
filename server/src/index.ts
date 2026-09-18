// Punto de entrada. Railway: Root Directory = server, start = bun run src/index.ts.
import { Hono } from "hono";
import { startRefresher } from "./news";
import { startJobsJanitor } from "./appsJobs";
import { api } from "./api";
import { firmware } from "./firmware";
import { board } from "./board";
import { logPage } from "./devicelog";
import { warmUp } from "./tts";
import { warmAssets } from "./assets";
import { normalizeLang } from "./lang";
import { redactSecrets } from "./net";
import { applyAdminPasswordFromEnv, auth, seedFromFiles } from "./accounts";
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
// Después de la migración: si está ADMIN_PASSWORD, la cuenta de administrador
// queda con esa contraseña. Es la salida cuando se perdió la provisoria.
await applyAdminPasswordFromEnv().catch((err) => console.error("db: ADMIN_PASSWORD falló", err));

app.get("/", (c) => c.text("ws397 server ok"));
app.route("/auth", auth);   // registro, login, logout y quién soy (solo con base de datos)
app.route("/firmware", firmware);
// `/board/` con barra final daba 404, y es como termina escribiendo la
// dirección cualquiera que la teclea a mano. Va ACÁ y no adentro del sub-app:
// montado en "/board", Hono no le pasa la barra final a `board.get("")`.
app.get("/board/", (c) => c.redirect("/board"));
app.route("/board", board);
app.route("/board/log", logPage);  // log del aparato, texto plano  // página web para el teléfono (pide el token del aparato)
app.route("/api", api);

const hubLang = normalizeLang(process.env.HUB_LANG ?? "es");
warmUp(hubLang);       // Piper carga el modelo una vez
warmAssets(hubLang);   // genera el paquete de contenido si falta (una sola vez, queda en /data)

const port = Number(process.env.PORT ?? 3000);
// El masticador de noticias corre solo, cada hora: el aparato se baja el
// paquete ya armado en vez de esperar a que se limpie cada artículo.
startRefresher();
// Los trabajos de las apps de Lua y sus archivos se podan a las 24 h (al
// arrancar y cada hora); los que quedaron a medias en un reinicio se dan por
// fallados cuando alguien los consulta.
startJobsJanitor();

console.log(`ws397 server on :${port} (${multiUser ? "multiusuario, Postgres" : "un solo usuario, archivos en /data"})`);

export default { port, fetch: app.fetch };
