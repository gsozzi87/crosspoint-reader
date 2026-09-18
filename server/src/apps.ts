// La puerta de las apps de Lua al servidor: `cp.call(servicio, args)` en el
// aparato es esto.
//
//   POST /api/apps/call?lang=xx   {app, service, args}
//        → 200 {ok:true, …} o 200 {ok:false, error}   (4xx solo sin Bearer o con
//          el cuerpo ilegible: la app recibe SIEMPRE una tabla con `ok`)
//   GET  /api/apps/file/:id        → un archivo generado por un trabajo
//
// Servicios CON NOMBRE y no URLs, a propósito: la app no elige a dónde va ni
// qué cabeceras manda; acá hay una tabla y cada servicio valida sus
// argumentos, igual que el cajón de Lua deja `math` y no `os`. Lo que tarde
// más de 25 s no es un servicio, es un trabajo (appsJobs.ts) y se consulta con
// `job.status`.
import { Hono } from "hono";
import { jobFile, jobStatus } from "./appsJobs";
import { AppsLlmError } from "./appsLlm";
import { LIBRITO_SERVICES } from "./librito";
import { LIBROS_SERVICES } from "./libros";
import { VIAJES_SERVICES } from "./viajes";
import { limitBody, redactSecrets } from "./net";
import { normalizeLang, type Lang } from "./lang";
import { accountOf, type AppEnv } from "./tenant";

export type ServiceCtx = { accountId: number; lang: Lang; app: string };
export type Service = (ctx: ServiceCtx, args: Record<string, unknown>) => Promise<Record<string, unknown>>;

const APP_NAME = /^[a-z0-9_-]{1,32}$/;
const SERVICE_NAME = /^[a-z0-9_.-]{1,48}$/;
const MAX_BODY = 32 * 1024;

// Común a todas las apps: cómo va un trabajo. `files` va vacío hasta `done`.
const jobStatusService: Service = async (ctx, args) => {
  // La app manda el id como string; si lo guardó como número, también vale.
  const id = typeof args.id === "string" ? args.id.trim() : typeof args.id === "number" ? String(args.id) : "";
  if (!/^[0-9a-f]{16}$/.test(id)) return { ok: false, error: "trabajo desconocido" };
  const job = await jobStatus(ctx.accountId, id);
  if (!job) return { ok: false, error: "trabajo desconocido" };
  return { ok: true, state: job.state, step: job.step, total: job.total, label: job.label, files: job.files, error: job.error };
};

const SERVICES: Record<string, Service> = {
  "job.status": jobStatusService,
  ...LIBRITO_SERVICES,
  ...VIAJES_SERVICES,
  ...LIBROS_SERVICES,
};

export const apps = new Hono<AppEnv>();

apps.post("/call", limitBody(MAX_BODY), async (c) => {
  let body: Record<string, unknown>;
  try {
    const parsed = await c.req.json();
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("no es un objeto");
    body = parsed as Record<string, unknown>;
  } catch {
    return c.json({ ok: false, error: "cuerpo ilegible" }, 400);
  }
  const app = typeof body.app === "string" && APP_NAME.test(body.app) ? body.app : "";
  const service = typeof body.service === "string" && SERVICE_NAME.test(body.service) ? body.service : "";
  const args = body.args && typeof body.args === "object" && !Array.isArray(body.args) ? (body.args as Record<string, unknown>) : {};
  if (!app) return c.json({ ok: false, error: "falta el nombre de la app" });
  const fn = service ? SERVICES[service] : undefined;
  if (!fn) return c.json({ ok: false, error: "servicio desconocido" });
  const ctx: ServiceCtx = { accountId: accountOf(c), lang: normalizeLang(c.req.query("lang")), app };
  const t0 = Date.now();
  try {
    const out = await fn(ctx, args);
    console.log(`apps: ${app} ${service} ok ${Date.now() - t0} ms`);
    return c.json({ ok: true, ...out });
  } catch (err) {
    // Cualquier excepción vuelve como {ok:false, error} y 200: la app la muestra
    // tal cual, así que el mensaje es corto y sin secretos. El código de
    // AppsLlmError (no_key, …) viaja aparte por si la app quiere distinguirlo.
    const msg = redactSecrets(err instanceof Error ? err.message : String(err)).slice(0, 200) || "falló";
    console.error(`apps: ${app} ${service} FALLÓ tras ${Date.now() - t0} ms: ${msg}`);
    const code = err instanceof AppsLlmError ? err.code : "error";
    return c.json({ ok: false, error: msg, code });
  }
});

const TYPES: Record<string, string> = {
  epub: "application/epub+zip",
  pdf: "application/pdf",
  mobi: "application/x-mobipocket-ebook",
  azw3: "application/vnd.amazon.mobi8-ebook",
  fb2: "application/x-fictionbook+xml",
  txt: "text/plain; charset=utf-8",
  json: "application/json; charset=utf-8",
  bin: "application/octet-stream",
};

// El archivo tal cual, para que el aparato lo baje a la tarjeta con el mismo
// descargador de la OTA. Solo los de la propia cuenta: `jobFile` busca en su
// documento y en ningún otro.
apps.get("/file/:id", async (c) => {
  const found = await jobFile(accountOf(c), c.req.param("id"));
  if (!found) return c.json({ ok: false, error: "not found", code: "not_found" }, 404);
  const file = Bun.file(found.path);
  if (!(await file.exists())) return c.json({ ok: false, error: "not found", code: "not_found" }, 404);
  const ext = found.name.slice(found.name.lastIndexOf(".") + 1).toLowerCase();
  return new Response(file, {
    headers: {
      "Content-Type": TYPES[ext] ?? TYPES.bin,
      "Content-Length": String(file.size),
      "Content-Disposition": `attachment; filename="${found.name}"`,
      "Cache-Control": "private, max-age=3600",
    },
  });
});
