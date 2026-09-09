// La "app del teléfono": una página web del Hono para manejar el aparato desde
// cualquier navegador. Pide el token del aparato una vez (queda en
// localStorage) y usa la misma API que el aparato. Sirve para crear
// recordatorios con fecha y repetición, manejar las dos listas (compras y
// tareas), escribir notas, subir fotos, cargar feeds, ver la memoria del
// asistente y configurar el aparato (lugar del clima, idioma, voz, volumen).
//
//   GET  /board                      página (sin token; el JS lo pide)
//   GET  /board/log                  el log que sube el aparato (devicelog.ts)
//   POST /api/board/reminder {title, dueAt: "YYYY-MM-DDTHH:MM"|null, repeat}
//   POST /api/board/item     {list, text}
//   POST /api/board/note     {text}
//   POST /api/board/feed     {name, url}
//   POST /api/board/photo?name=      (body: image/bmp de 2 bpp, lo arma el navegador)
//   POST /api/board/attachment?trip=&name=   (multipart o cuerpo crudo: PDF del vuelo, del hotel...)
//   GET  /api/board/extra    -> {feeds, memories, settings, lists}
//   POST /api/board/settings {lang, speak, musicVolume, translatorLang}
//   (leer, tildar y borrar: GET /api/hub, POST /api/hub/done, POST /api/hub/edit)
import { Hono } from "hono";
import { load, save, nextId, resolveList, upsertReminder, refreshTimeZone, repeatText, DEFAULT_SETTINGS, type Settings } from "./store";
import { savePhoto, toDeviceBmp, MAX_UPLOAD_BYTES } from "./photos";
import { hubDiagnostics } from "./hub";
import { config, saveConfig, publicConfig, MODEL_PRICES, STT_PRICES, SEARCH_PRICE_ANTHROPIC, QUERY_SHAPE, deepSeekPeak, queryCost, type Config } from "./config";
import { chatText, providerLabel, searchToolLabel, searchKindLabel, providerSearchKind } from "./llm";
import { searchWeb } from "./websearch";
import { checkUrl, isSafeRemoteUrl, readBody } from "./net";
import { probeFeed, checkFeed } from "./rss";
import { boardAttachment } from "./attachments";
import { accountOf, isAdmin, type AppEnv } from "./tenant";
import { clampNote, MAX_NOTE_CHARS } from "./notes";

export const boardApi = new Hono<AppEnv>();

// Alta y edición (si trae id). La repetición es el objeto nuevo
// {kind, days, interval, until}; una cadena vieja también entra.
boardApi.post("/reminder", async (c) => {
  const acc = accountOf(c);
  await refreshTimeZone(acc);
  const b = await readBody(c);
  const store = await load(acc);
  const res = upsertReminder(store, b);
  if (!res.ok) {
    return res.error === "not_found"
      ? c.json({ ok: false, error: "no existe ese recordatorio" }, 404)
      : c.json({ ok: false, error: "title required" }, 400);
  }
  await save(acc, store);
  const r = res.reminder;
  return c.json({ ok: true, reminder: { id: r.id, title: r.title, at: r.dueAt, repeatSpec: r.repeat, repeatText: repeatText(r.repeat, r.dueAt, "es") } });
});

boardApi.post("/item", async (c) => {
  const b = await readBody(c);
  const text = (b.text ?? "").toString().trim().slice(0, 200);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const acc = accountOf(c);
  const store = await load(acc);
  const list = resolveList(store, b.list);
  store.lists[list].push({ id: nextId(store), text, done: false, dueDate: null, createdAt: new Date().toISOString() });
  await save(acc, store);
  return c.json({ ok: true, list });
});

boardApi.post("/feed", async (c) => {
  const b = await readBody(c);
  let url = (b.url ?? "").toString().trim().slice(0, 500);
  if (url && !/^[a-z]+:\/\//i.test(url)) url = "https://" + url;  // "diario.com/rss"
  // El servidor es el que va a buscar el feed, y está adentro de la red privada
  // de Railway: un "feed" apuntando ahí adentro es SSRF.
  if (!isSafeRemoteUrl(url)) return c.json({ ok: false, error: "la URL del feed no sirve (tiene que ser http(s) a un host público)" }, 400);
  // Se lee ANTES de guardarlo. Si el usuario pegó la dirección de la página del
  // diario (y no la del feed), acá se descubre el feed de verdad por el
  // <link rel="alternate">; si no hay ninguno, se avisa en vez de guardar algo
  // que nunca iba a traer noticias.
  let probe: { url: string; title: string; count: number };
  try {
    probe = await probeFeed(url);
  } catch (err) {
    return c.json({ ok: false, error: `no se pudo leer el feed: ${String(err instanceof Error ? err.message : err).slice(0, 160)}` }, 400);
  }
  const acc = accountOf(c);
  const store = await load(acc);
  store.feeds ??= [];
  if (store.feeds.some((f) => f.url === probe.url)) return c.json({ ok: false, error: "ese feed ya está cargado" }, 400);
  const name =
    (b.name ?? "").toString().trim().slice(0, 40) ||
    probe.title ||
    new URL(probe.url).hostname.replace(/^www\./, "");
  store.feeds.push({ id: nextId(store), name, url: probe.url });
  await save(acc, store);
  return c.json({ ok: true, name, url: probe.url, count: probe.count });
});

// "Probar" de la pestaña Noticias: baja el feed sin caché y dice cuántos
// titulares trae o por qué no trae ninguno.
boardApi.post("/feed/test", async (c) => {
  const b = await readBody(c);
  const store = await load(accountOf(c));
  const feed = (store.feeds ?? []).find((f) => f.id === Number(b.id));
  if (!feed) return c.json({ ok: false, error: "no está" }, 404);
  const r = await checkFeed(feed.url);
  return c.json({ ok: true, name: feed.name, ...r });
});

// La foto se sube tal como salió del teléfono (JPEG, PNG, lo que sea) y la
// convierte el servidor: rota por EXIF, escala a 480x800, pasa a 4 grises con
// difuminado y arma el BMP de 2 bpp. Antes lo hacía el navegador con un canvas y
// salía apaisado y sin control. Un BMP ya convertido se acepta igual.
boardApi.post("/photo", async (c) => {
  const name = (c.req.query("name") ?? "foto").toString().slice(0, 80);
  const bytes = new Uint8Array(await c.req.arrayBuffer());
  if (bytes.byteLength < 100 || bytes.byteLength > MAX_UPLOAD_BYTES) return c.json({ ok: false, error: "bad size" }, 400);
  const isBmp = bytes[0] === 0x42 && bytes[1] === 0x4d;
  let out: Uint8Array<ArrayBufferLike> = bytes;
  if (!isBmp) {
    try {
      const t0 = Date.now();
      out = await toDeviceBmp(bytes);
      console.log(`photo: ${name} ${bytes.byteLength} B -> ${out.byteLength} B en ${Date.now() - t0} ms`);
    } catch (err) {
      console.error("photo convert:", err);
      return c.json({ ok: false, error: `no se pudo convertir (${String(err).slice(0, 120)})` }, 400);
    }
  }
  try {
    const id = await savePhoto(accountOf(c), name, out);
    return c.json({ ok: true, id });
  } catch (err) {
    console.error("photo save:", err);
    return c.json({ ok: false, error: `no se pudo guardar (${String(err).slice(0, 120)})` }, 500);
  }
});

// Adjuntos de los viajes (attachments.ts): el PDF entra tal como salió del mail
// y sale convertido a bitmaps que el aparato pinta, con el código de barras
// vuelto a generar limpio.
boardApi.route("/attachment", boardAttachment);

boardApi.get("/extra", async (c) => {
  const acc = accountOf(c);
  const store = await load(acc);
  return c.json({
    ok: true,
    feeds: store.feeds ?? [],
    memories: store.memories ?? [],
    settings: store.settings ?? DEFAULT_SETTINGS,
    lists: Object.keys(store.lists),
    diag: await hubDiagnostics(acc),
  });
});

// Ajustes del aparato. Cada cambio sube `rev`; el aparato los aplica en la
// próxima sincronización solo si la revisión es mayor a la que ya tenía.
boardApi.post("/settings", async (c) => {
  const acc = accountOf(c);
  const b = await readBody(c);
  const store = await load(acc);
  const s: Settings = { ...DEFAULT_SETTINGS, ...(store.settings ?? {}) };
  if (typeof b.lang === "string" && /^[a-z]{2}$/.test(b.lang)) s.lang = b.lang;
  if (b.speak === "none" || b.speak === "short" || b.speak === "all") s.speak = b.speak;
  if (Number.isFinite(Number(b.musicVolume))) s.musicVolume = Math.max(0, Math.min(100, Math.round(Number(b.musicVolume))));
  if (typeof b.translatorLang === "string" && /^[a-z]{2}$/.test(b.translatorLang)) s.translatorLang = b.translatorLang;
  s.rev = (s.rev ?? 0) + 1;
  store.settings = s;
  await save(acc, store);
  return c.json({ ok: true, settings: s });
});

// Proveedores de IA y token, configurables desde la web. Las claves entran acá y
// no salen nunca: la página solo ve si hay clave puesta.
//
// OJO: esta configuración es DEL OPERADOR y es una sola para todo el servidor
// (con 1000 aparatos vendidos no puede poner cada uno su clave de Anthropic).
// Solo la puede ver y tocar una cuenta admin; para las demás la pestaña IA ni
// se muestra y estos endpoints contestan 403. Sin base de datos hay un solo
// usuario y es el admin, así que todo sigue igual que siempre.
boardApi.use("/config", async (c, next) => {
  if (!isAdmin(c)) return c.json({ ok: false, error: "solo el administrador", code: "forbidden" }, 403);
  await next();
});
boardApi.use("/config/*", async (c, next) => {
  if (!isAdmin(c)) return c.json({ ok: false, error: "solo el administrador", code: "forbidden" }, 403);
  await next();
});

boardApi.get("/config", async (c) => c.json({ ok: true, config: await publicConfig() }));

boardApi.post("/config", async (c) => {
  const b = await readBody(c);
  const cfg = await config();
  const next: Config = { llm: { ...cfg.llm }, stt: { ...cfg.stt }, search: { ...cfg.search }, deviceToken: cfg.deviceToken };
  // La clave del proveedor viaja como Bearer a este baseUrl: si se acepta
  // cualquier URL, cambiarla es exfiltrar la clave. Solo https a un host
  // público (o http a localhost, para un modelo corriendo en la misma máquina).
  const cleanUrl = (raw: string): { url: string } | { error: string } => {
    const trimmed = raw.trim().replace(/\/+$/, "");
    const r = checkUrl(trimmed, { allowLocal: true });
    return r.ok ? { url: trimmed } : { error: r.error };
  };
  // Cambiar de host sin cargar clave nueva NO reusa la vieja: se borra y hay
  // que volver a cargarla, en vez de mandársela a otro servidor.
  const hostOf = (raw: string): string => {
    try {
      return new URL(raw).host;
    } catch {
      return "";
    }
  };
  if (b.llm) {
    if (b.llm.provider === "anthropic" || b.llm.provider === "openai") next.llm.provider = b.llm.provider;
    if (typeof b.llm.baseUrl === "string" && b.llm.baseUrl.trim()) {
      const r = cleanUrl(b.llm.baseUrl);
      if ("error" in r) return c.json({ ok: false, error: `URL del proveedor: ${r.error}` }, 400);
      if (hostOf(r.url) !== hostOf(next.llm.baseUrl)) next.llm.key = "";
      next.llm.baseUrl = r.url;
    } else if (typeof b.llm.baseUrl === "string") {
      next.llm.baseUrl = "";  // Anthropic no usa baseUrl
    }
    if (typeof b.llm.model === "string" && b.llm.model.trim()) next.llm.model = b.llm.model.trim();
    if (typeof b.llm.key === "string" && b.llm.key.trim()) next.llm.key = b.llm.key.trim();
  }
  if (b.stt) {
    if (typeof b.stt.baseUrl === "string" && b.stt.baseUrl.trim()) {
      const r = cleanUrl(b.stt.baseUrl);
      if ("error" in r) return c.json({ ok: false, error: `URL de transcripción: ${r.error}` }, 400);
      if (hostOf(r.url) !== hostOf(next.stt.baseUrl)) next.stt.key = "";
      next.stt.baseUrl = r.url;
    }
    if (typeof b.stt.model === "string" && b.stt.model.trim()) next.stt.model = b.stt.model.trim();
    if (typeof b.stt.key === "string" && b.stt.key.trim()) next.stt.key = b.stt.key.trim();
  }
  // Búsqueda en internet: se prende y se apaga acá porque cada búsqueda cuesta.
  if (b.search) {
    if (typeof b.search.enabled === "boolean") next.search.enabled = b.search.enabled;
    if (["free", "tavily", "brave"].includes(b.search.provider)) next.search.provider = b.search.provider;
    if (Number.isFinite(Number(b.search.maxUses))) next.search.maxUses = Math.max(1, Math.min(10, Math.round(Number(b.search.maxUses))));
    if (typeof b.search.key === "string" && b.search.key.trim()) next.search.key = b.search.key.trim();
  }
  if (typeof b.deviceToken === "string") next.deviceToken = b.deviceToken.trim().slice(0, 200);
  if (next.llm.provider === "openai" && !next.llm.baseUrl) return c.json({ ok: false, error: "falta la URL del proveedor" }, 400);
  await saveConfig(next);
  console.log(`config: llm=${next.llm.provider}/${next.llm.model} stt=${next.stt.model} búsqueda=${next.search.enabled ? next.search.provider : "off"}`);
  return c.json({ ok: true, config: await publicConfig() });
});

// Prueba rápida de los dos servicios, para no descubrir que la clave está mal
// hablándole al aparato.
boardApi.post("/config/test", async (c) => {
  const out: { llm?: string; stt?: string; search?: string } = {};
  const t0 = Date.now();
  try {
    const answer = await chatText({ system: "Respondé exactamente: ok", user: "decime ok", maxTokens: 10 });
    out.llm = `${await providerLabel()} → "${answer.trim().slice(0, 40)}" (${Date.now() - t0} ms)`;
  } catch (err) {
    out.llm = `ERROR: ${String(err instanceof Error ? err.message : err).slice(0, 200)}`;
  }
  const conf = await config();
  out.stt = conf.stt.key ? `${conf.stt.model} en ${conf.stt.baseUrl} (clave puesta)` : "ERROR: falta la clave de transcripción";
  // La búsqueda: con Claude la hace él (no hay nada que probar acá); con las
  // compatibles la hace el servidor y sí se puede probar de verdad.
  if (!conf.search.enabled) out.search = "apagada";
  else if (conf.llm.provider === "anthropic") out.search = `la hace Claude solo (${searchToolLabel(conf.llm.model)}, hasta ${conf.search.maxUses} por respuesta, USD 0,01 cada una)`;
  else if (providerSearchKind(conf.llm.baseUrl, conf.llm.model) !== "none") out.search = await searchKindLabel();
  else {
    const t1 = Date.now();
    const r = await searchWeb("noticias de hoy", "es", 3);
    out.search = r.error
      ? `ERROR: ${r.error}`
      : `${r.provider}: ${r.results.length} resultados en ${Date.now() - t1} ms · "${(r.results[0]?.title ?? "").slice(0, 60)}"`;
  }
  return c.json({ ok: true, ...out });
});

// Cuánto sale cada consulta con cada modelo. El aparato se vende en volumen: el
// costo por consulta es lo que decide el proveedor, así que se muestra en la
// web al lado del selector en vez de quedar en una planilla aparte.
boardApi.get("/costs", async (c) => {
  const conf = await config();
  const now = new Date();
  const rows = Object.entries(MODEL_PRICES).map(([model, p]) => {
    const q = queryCost(model, conf.stt.model, now);
    return {
      model,
      in: q.peak === false && p.offPeakIn !== undefined ? p.offPeakIn : p.in,
      out: q.peak === false && p.offPeakOut !== undefined ? p.offPeakOut : p.out,
      llm: q.llm,
      stt: q.stt,
      total: q.total,
      peak: q.peak,
      note: p.note ?? "",
      current: model === conf.llm.model,
    };
  });
  rows.sort((a, b) => a.total - b.total);
  return c.json({
    ok: true,
    shape: QUERY_SHAPE,
    stt: conf.stt.model,
    sttPerHour: STT_PRICES[conf.stt.model] ?? null,
    searchAnthropic: SEARCH_PRICE_ANTHROPIC,
    deepSeekPeakNow: deepSeekPeak(now),
    rows,
  });
});

boardApi.post("/note", async (c) => {
  const b = await readBody(c);
  const text = clampNote(b.text);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const acc = accountOf(c);
  const store = await load(acc);
  store.notes.push({ id: nextId(store), text, createdAt: new Date().toISOString() });
  await save(acc, store);
  return c.json({ ok: true });
});

const STYLE = `
:root{color-scheme:light dark;--bg:#f2f3f5;--card:#fff;--ink:#111;--muted:#666;--line:#e6e6e6;--accent:#111}
@media (prefers-color-scheme:dark){:root{--bg:#15171a;--card:#1e2126;--ink:#e9e9e9;--muted:#9aa0a6;--line:#2c3036;--accent:#e9e9e9}}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:var(--bg);color:var(--ink)}
header{background:#111;color:#fff;padding:10px 14px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:6}
header strong{font-size:16px}
header button{background:#2a2a2a;color:#fff;border:none;padding:6px 10px;border-radius:8px;font:inherit}
nav{display:flex;gap:6px;overflow-x:auto;padding:8px 10px;background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:46px;z-index:5;scrollbar-width:none}
nav::-webkit-scrollbar{display:none}
nav button{flex:0 0 auto;background:transparent;color:var(--muted);border:1px solid var(--line);border-radius:999px;padding:7px 14px;font:inherit}
nav button.on{background:var(--accent);color:var(--bg);border-color:var(--accent);font-weight:600}
main{max-width:760px;margin:0 auto;padding:12px 12px 60px}
section.tab{display:none}
section.tab.on{display:block}
.card{background:var(--card);border-radius:14px;padding:14px;margin:0 0 12px;box-shadow:0 1px 2px #0000000d}
h2{margin:0 0 10px;font-size:16px}
h3{margin:14px 0 4px;font-size:14px;color:var(--muted);font-weight:600}
table.costs{border-collapse:collapse;width:100%;font-size:13px}
table.costs th,table.costs td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
table.costs th{color:var(--muted);font-weight:600}
table.costs td.num{text-align:right;font-variant-numeric:tabular-nums}
table.costs tr.on td{font-weight:700}
form{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px;align-items:center}
input,select,textarea,button{font:inherit;padding:9px 11px;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--ink)}
input[type=text],input[type=password],textarea{flex:1;min-width:150px}
input[type=range]{flex:1;min-width:140px;padding:0}
button{background:var(--accent);color:var(--bg);border:none;font-weight:600}
button.ghost{background:transparent;color:var(--ink);border:1px solid var(--line);font-weight:400;padding:7px 11px}
button.danger{background:#b3261e;color:#fff}
ul{list-style:none;margin:0;padding:0}
li{display:flex;gap:8px;align-items:center;padding:9px 0;border-top:1px solid var(--line)}
li span{flex:1;word-break:break-word}
li small{color:var(--muted)}
label{font-size:13px;color:var(--muted);min-width:130px}
.row{display:flex;gap:8px;align-items:center;margin:9px 0;flex-wrap:wrap}
.muted{color:var(--muted);font-size:13px;line-height:1.45}
.ok{color:#1e7d32;font-size:13px}
.bad{color:#b3261e;font-size:13px}
pre{white-space:pre-wrap;word-break:break-word;font:12px/1.4 ui-monospace,Menlo,monospace;background:var(--bg);padding:10px;border-radius:10px;max-height:60vh;overflow:auto}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:18px;background:#111;color:#fff;padding:11px 18px;border-radius:22px;opacity:0;transition:opacity .2s;pointer-events:none;z-index:9}
.toast.on{opacity:1}
#gate{display:none;max-width:420px;margin:60px auto;text-align:center}
.cal{display:grid;grid-template-columns:repeat(7,1fr);gap:4px;margin-top:6px}
.cal .h{font-size:11px;color:var(--muted);text-align:center;padding:2px 0;font-weight:600}
.cal button.d{display:block;text-align:left;min-height:52px;padding:4px 5px;background:var(--bg);color:var(--ink);border:1px solid transparent;border-radius:8px;font-weight:400;overflow:hidden}
.cal button.d b{font-size:13px;font-weight:600}
.cal button.d i{display:block;font-style:normal;font-size:10px;line-height:1.15;color:var(--muted);margin-top:2px;word-break:break-word;max-height:26px;overflow:hidden}
.cal button.d.off{opacity:.4}
.cal button.d.today{border-color:var(--muted)}
.cal button.d.sel{background:var(--accent);color:var(--bg)}
.cal button.d.sel i{color:var(--bg);opacity:.85}
.calhead{display:flex;align-items:center;gap:10px;justify-content:space-between}
.calhead b{font-size:15px}
label.chk{min-width:0;display:inline-flex;align-items:center;gap:4px;color:var(--ink)}
`;

// Ojo al editar: esto vive dentro de un template literal, así que un backslash
// antes de una comilla se lo come el literal y rompe el script entero. Regla:
// nada de \\' ni backticks acá adentro; las cadenas van con comillas dobles y los
// atributos HTML con comillas simples. Los botones no llevan onclick: se manejan
// por delegación con data-act.
const SCRIPT = `
// Dos formas de entrar, según cómo esté armado el servidor:
//   - sin base de datos (como siempre): el token del aparato, guardado acá;
//   - con base de datos (multiusuario): correo y contraseña, y la sesión viaja
//     en una cookie HttpOnly que este script ni ve. Ahí no se guarda ningún
//     token en el navegador.
// GET /auth/me es lo que dice en cuál de los dos estamos.
let token = localStorage.getItem("deviceToken") || "";
let me = null;
let cfg = null;
let entered = false;
const $ = (id) => document.getElementById(id);
function esc(s){ return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\\u0022":"&quot;","\\u0027":"&#39;"}[c])); }
function toast(msg){ const t = $("toast"); t.textContent = msg; t.classList.add("on"); setTimeout(() => t.classList.remove("on"), 2200); }

function multi(){ return !!(me && me.multi); }

async function api(path, body, method){
  const headers = { "Content-Type": "application/json" };
  // Con sesión no hay token: manda la cookie. Con token no hay sesión.
  if (token) headers["Authorization"] = "Bearer " + token;
  const r = await fetch(path, {
    method: method || (body ? "POST" : "GET"),
    headers,
    credentials: "same-origin",
    body: body ? JSON.stringify(body) : undefined,
  });
  if (r.status === 401) {
    // Antes de entrar ya hay pedidos en el aire (el texto de la repetición, por
    // ejemplo): un 401 de esos no tiene que pisar el cartel de la pantalla de
    // entrada con un "se cerró la sesión" que no pasó.
    if (entered) {
      if (multi()) showLogin("Se cerró la sesión, entra de nuevo");
      else gate("Token rechazado");
    }
    throw new Error("no autorizado");
  }
  if (!r.ok) {
    let detail = "";
    try { detail = (await r.json()).error || ""; } catch (e) {}
    throw new Error(detail || ("http " + r.status));
  }
  return r.json();
}

function screen(which, msg){
  entered = which === "app";
  $("gate").style.display = which === "token" ? "block" : "none";
  $("login").style.display = which === "login" ? "block" : "none";
  $("app").style.display = which === "app" ? "block" : "none";
  $("nav").style.display = which === "app" ? "flex" : "none";
  if (which === "token") { $("gateMsg").textContent = msg || ""; $("tokenInput").value = token; }
  if (which === "login") $("loginMsg").textContent = msg || "";
}

function gate(msg){ screen("token", msg); }
function showLogin(msg){ screen("login", msg); }

function saveToken(){
  const v = $("tokenInput").value.trim();
  if (!v) { $("gateMsg").textContent = "Pon el token"; return; }
  token = v;
  localStorage.setItem("deviceToken", token);
  screen("app");
  refresh().catch((e) => gate("No se pudo conectar: " + e.message));
}

// ── Cuentas (solo con base de datos) ───────────────────────────────────────

async function authPost(path, body){
  const r = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    credentials: "same-origin",
    body: JSON.stringify(body || {}),
  });
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok || !j.ok) throw new Error(j.error || ("http " + r.status));
  return j;
}

async function loadMe(){
  const r = await fetch("/auth/me", { credentials: "same-origin" });
  try { me = await r.json(); } catch (e) { me = null; }
  return me;
}

// Qué pestañas se ven: Aparatos solo en multiusuario, IA solo para el admin
// (la configuración de IA es del operador y es una sola para todo el servidor).
function applyMe(){
  const navBtn = (t) => document.querySelector("nav button[data-tab='" + t + "']");
  const isAdmin = !me || me.isAdmin !== false;
  if (navBtn("ia")) navBtn("ia").style.display = isAdmin ? "" : "none";
  if (navBtn("aparatos")) navBtn("aparatos").style.display = multi() ? "" : "none";
  $("tokenBtn").style.display = multi() ? "none" : "";
  $("who").textContent = multi() && me.email ? me.email : "";
  const open = localStorage.getItem("boardTab") || "pizarra";
  if ((open === "ia" && !isAdmin) || (open === "aparatos" && !multi())) showTab("pizarra");
}

async function loadDevices(){
  const info = await loadMe();
  if (!info || !info.ok) return;
  const list = info.devices || [];
  $("devices").innerHTML = list.map((d) => {
    const when = d.lastSeen ? new Date(d.lastSeen).toLocaleString() : "todavía no se conectó";
    return row(d.name || d.deviceId, d.deviceId + " · " + when,
      btn("Renombrar", "dev-rename", { id: d.deviceId, name: d.name || "" }) +
      btn("Desvincular", "dev-del", { id: d.deviceId }, "danger"));
  }).join("") || empty("Todavía no hay ningún aparato vinculado");
}

function showTab(name){
  document.querySelectorAll("section.tab").forEach((s) => s.classList.toggle("on", s.id === "tab-" + name));
  document.querySelectorAll("nav button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
  localStorage.setItem("boardTab", name);
  if (name === "log") loadLog();
  if (name === "calendario") loadCalendar().catch((e) => toast("No se pudo cargar el calendario: " + e.message));
  if (name === "aparatos") loadDevices().catch((e) => toast(e.message));
  window.scrollTo(0, 0);
}

function btn(label, act, extra, cls){
  let attrs = "";
  for (const k in extra) attrs += " data-" + k + "='" + esc(extra[k]) + "'";
  return "<button class='" + (cls || "ghost") + "' data-act='" + act + "'" + attrs + ">" + label + "</button>";
}
function row(text, small, buttons){
  return "<li><span>" + esc(text) + (small ? " <small>" + esc(small) + "</small>" : "") + "</span>" + (buttons || "") + "</li>";
}
function empty(text){ return "<li class='muted'>" + esc(text) + "</li>"; }

async function refresh(){
  const d = await api("/api/hub?lang=es");
  const x = await api("/api/board/extra");

  $("reminders").innerHTML = d.reminders.map((r) =>
    row(r.title, [r.when || "sin hora", r.repeatText].filter(Boolean).join(" · "),
      btn("Hecho", "done", { kind: "reminder", id: r.id }) +
      btn("Editar", "remedit", { rem: JSON.stringify({ id: r.id, title: r.title, at: r.at, repeat: r.repeatSpec }) }) +
      btn("Borrar", "del", { kind: "reminder", id: r.id }))).join("") || empty("Sin recordatorios");

  // Son dos listas fijas (compras y tareas): no se crean ni se borran.
  const names = x.lists;
  const sel = $("listSelect");
  const keep = sel.value;
  sel.innerHTML = names.map((n) => "<option>" + esc(n) + "</option>").join("");
  if (names.indexOf(keep) >= 0) sel.value = keep;

  const byKey = {};
  d.lists.forEach((l) => { byKey[l.key || l.name] = l.items; });
  $("listItems").innerHTML = names.map((n) => {
    const items = byKey[n] || [];
    return "<h3>" + esc(n) + " · " + items.length + "</h3><ul>" +
      (items.map((i) => row(i.text, "",
        btn("Hecho", "done", { kind: "item", id: i.id }) +
        btn("Borrar", "del", { kind: "item", id: i.id }))).join("") || empty("Vacía")) + "</ul>";
  }).join("");

  $("notes").innerHTML = d.notes.map((n) =>
    row(n.text, "", btn("Borrar", "del", { kind: "note", id: n.id }))).join("") || empty("Sin notas");
  $("feeds").innerHTML = x.feeds.map((f) =>
    row(f.name, f.url,
      btn("Probar", "testfeed", { id: f.id }) +
      btn("Borrar", "del", { kind: "feed", id: f.id }))).join("") || empty("Sin feeds");
  $("memories").innerHTML = x.memories.map((m) =>
    row(m.text, "", btn("Borrar", "del", { kind: "memory", id: m.id }))).join("") || empty("Nada guardado");

  const ph = await api("/api/photos");
  $("photos").innerHTML = ph.photos.map((p) =>
    row(p.name, Math.round(p.size / 1024) + " KB", btn("Borrar", "delphoto", { id: p.id }))).join("") || empty("Sin fotos");

  $("setLang").value = x.settings.lang;
  $("setSpeak").value = x.settings.speak;
  $("setTranslator").value = x.settings.translatorLang;
  $("setVolume").value = x.settings.musicVolume;
  $("volumeOut").textContent = x.settings.musicVolume + " %";

  const g = x.diag || {};
  const p0 = g.place;
  $("place").textContent = p0 ? (p0.label || p0.name || (p0.lat + ", " + p0.lon)) : "sin lugar configurado";
  const wx = g.weather || {};
  $("weatherNow").textContent = wx.line
    ? wx.line + (wx.detail ? " · " + wx.detail : "")
    : wx.noPlace ? "El servidor no tiene lugar: elegilo acá abajo"
    : wx.error ? "Falló: " + wx.error
    : "Sin datos todavía";
  const last = g.lastDeviceFetch || 0;
  $("lastSync").textContent = !last
    ? "El aparato todavía no vino a buscar datos."
    : "El aparato sincronizó hace " + Math.max(0, Math.round((Date.now() - last) / 60000)) + " min. Para que se lleve lo que cambiaste: mantené Atrás 1,2 s en el hub.";

  await loadConfig();
  loadAssets().catch(() => {});
  if ($("tab-calendario").classList.contains("on")) loadCalendar().catch(() => {});
}

async function loadConfig(){
  // La pestaña IA es del operador: si esta cuenta no es admin, ni se pide.
  if (me && me.isAdmin === false) return;
  const r = await api("/api/board/config");
  cfg = r.config;
  const presets = cfg.presets || {};
  $("llmPreset").innerHTML = Object.keys(presets).map((k) =>
    "<option value='" + esc(k) + "'>" + esc(presets[k].label) + "</option>").join("");
  // El preset que coincide con lo que está guardado.
  let current = "anthropic";
  for (const k in presets) {
    const p = presets[k];
    if (p.provider === cfg.llm.provider && (p.provider === "anthropic" || p.baseUrl === cfg.llm.baseUrl)) current = k;
  }
  $("llmPreset").value = current;
  fillModels(current, cfg.llm.model);
  $("llmBase").value = cfg.llm.baseUrl || "";
  $("llmKeyState").textContent = cfg.llm.hasKey ? "clave puesta" : "sin clave";
  $("llmKeyState").className = cfg.llm.hasKey ? "ok" : "bad";
  $("sttBase").value = cfg.stt.baseUrl;
  $("sttModel").value = cfg.stt.model;
  $("sttKeyState").textContent = cfg.stt.hasKey ? "clave puesta" : "sin clave";
  $("sttKeyState").className = cfg.stt.hasKey ? "ok" : "bad";
  const se = cfg.search || { enabled: false, provider: "free", maxUses: 3, hasKey: false };
  $("searchOn").checked = !!se.enabled;
  $("searchProvider").value = se.provider;
  $("searchMax").value = se.maxUses;
  $("searchKeyState").textContent = se.hasKey ? "clave puesta" : "sin clave (usa los buscadores gratis)";
  $("searchKeyState").className = se.hasKey ? "ok" : "muted";
  $("tokenState").textContent = cfg.deviceTokenSet ? "hay un token propio guardado" : "se usa el token del entorno";
  loadCosts().catch(() => {});
}

// Paquete de contenido: qué hay generado y cuánto pesa.
async function loadAssets(){
  const r = await api("/api/assets/status");
  const mb = (b) => (b / 1048576).toFixed(1) + " MB";
  const langs = r.langs || [];
  $("assetsState").textContent = langs.length
    ? "Dibujos " + r.lucide + " · " + r.cards + " tarjetas."
    : "Todavía no se generó nada. Tocá \u201cGenerar lo que falte\u201d.";
  $("assetsTable").innerHTML = !langs.length ? "" :
    "<tr><th>Idioma</th><th>Versión</th><th>Archivos</th><th>Tamaño</th><th>Por tipo</th><th>Estado</th></tr>" +
    langs.map((l) =>
      "<tr><td>" + esc(l.lang) + "</td><td>" + esc(l.version) + "</td>" +
      "<td class='num'>" + l.files + "</td><td class='num'>" + mb(l.bytes) + "</td>" +
      "<td>" + Object.keys(l.byKind || {}).map((k) => k + " " + l.byKind[k].count + " (" + mb(l.byKind[k].bytes) + ")").join(", ") + "</td>" +
      "<td>" + (l.building ? "generando " + l.done + "/" + l.total : "listo") + (l.error ? " · " + esc(l.error) : "") + "</td></tr>").join("");
}

// Precio por consulta. El número que importa es el total: es lo que se paga
// cada vez que alguien aprieta Hablar.
async function loadCosts(){
  const r = await api("/api/board/costs");
  const usd = (v) => "US$ " + v.toFixed(4).replace(".", ",");
  $("costShape").textContent = r.shape.seconds + " s de audio, " + r.shape.inTokens +
    " tokens de entrada y " + r.shape.outTokens + " de salida";
  $("costs").innerHTML =
    "<tr><th>Modelo</th><th>Entrada</th><th>Salida</th><th>Modelo</th><th>Voz a texto</th><th>Total</th><th></th></tr>" +
    r.rows.map((x) =>
      "<tr class='" + (x.current ? "on" : "") + "'>" +
      "<td>" + esc(x.model) + (x.current ? " ←" : "") + "</td>" +
      "<td class='num'>" + x.in + "</td><td class='num'>" + x.out + "</td>" +
      "<td class='num'>" + usd(x.llm) + "</td><td class='num'>" + usd(x.stt) + "</td>" +
      "<td class='num'>" + usd(x.total) + "</td>" +
      "<td class='muted'>" + esc(x.note) + "</td></tr>").join("");
  $("costNote").innerHTML =
    "Entrada y salida en dólares por millón de tokens. Transcripción: <b>" + esc(r.stt) + "</b>" +
    (r.sttPerHour !== null ? " (US$ " + r.sttPerHour + " la hora de audio)" : " (precio desconocido)") + ". " +
    "DeepSeek cobra el doble en hora pico (01:00-04:00 y 06:00-10:00 UTC de lunes a viernes): ahora está " +
    (r.deepSeekPeakNow ? "<b>en hora pico</b>" : "<b>fuera de pico</b>") + ". " +
    "Una búsqueda en internet con Claude suma US$ " + String(r.searchAnthropic).replace(".", ",") +
    " — más que la respuesta entera, por eso solo se busca cuando hace falta. Con Groq la búsqueda va incluida en los tokens.";
}

function fillModels(presetKey, selected){
  const p = (cfg.presets || {})[presetKey] || { models: [] };
  const models = p.models.slice();
  if (selected && models.indexOf(selected) < 0) models.unshift(selected);
  $("llmModel").innerHTML = models.map((m) => "<option>" + esc(m) + "</option>").join("");
  if (selected) $("llmModel").value = selected;
  $("llmBaseRow").style.display = p.provider === "anthropic" ? "none" : "flex";
  if (p.provider !== "anthropic" && p.baseUrl) $("llmBase").value = p.baseUrl;
  if (p.sttBaseUrl && $("sttFollow").checked) {
    $("sttBase").value = p.sttBaseUrl;
    $("sttModel").value = (p.sttModels || [])[0] || $("sttModel").value;
  }
}

// ── Repetición: los mismos controles para un evento y para un recordatorio ──
// El texto de "cada cuánto" lo escribe el servidor (/api/calendar/repeat), que
// es el mismo que después ve el aparato: una sola fuente para los seis idiomas.
const DOW_SHORT = ["D", "L", "M", "M", "J", "V", "S"];
const DOW_LONG = ["Dom", "Lun", "Mar", "Mié", "Jue", "Vie", "Sáb"];
const MONTHS = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"];
const REPEAT_UNIT = { daily: "días", weekly: "semanas", monthly: "meses", yearly: "años" };

function repeatControls(p){
  const days = [1, 2, 3, 4, 5, 6, 0].map((d) =>
    "<label class='chk'><input type='checkbox' id='" + p + "Day" + d + "'> " + DOW_LONG[d] + "</label>").join("");
  return "<div class='row'><label for='" + p + "Kind'>Se repite</label>" +
    "<select id='" + p + "Kind'>" +
      "<option value='none'>Una sola vez</option>" +
      "<option value='daily'>Todos los días</option>" +
      "<option value='weekdays'>De lunes a viernes</option>" +
      "<option value='weekly'>Ciertos días de la semana</option>" +
      "<option value='monthly'>Todos los meses</option>" +
      "<option value='yearly'>Todos los años</option>" +
    "</select></div>" +
    "<div class='row' id='" + p + "DaysRow'>" + days + "</div>" +
    "<div class='row' id='" + p + "EveryRow'><label for='" + p + "Interval'>Cada</label>" +
      "<input type='number' id='" + p + "Interval' min='1' max='99' value='1' style='max-width:80px'>" +
      "<span class='muted' id='" + p + "Unit'></span></div>" +
    "<div class='row' id='" + p + "UntilRow'><label for='" + p + "Until'>Hasta (opcional)</label><input type='date' id='" + p + "Until'></div>" +
    "<p class='muted'>Va a sonar: <b id='" + p + "Text'>Una sola vez</b></p>";
}

function readRepeat(p){
  const days = [];
  for (let d = 0; d < 7; d++) if ($(p + "Day" + d).checked) days.push(d);
  return {
    kind: $(p + "Kind").value,
    days: days,
    interval: Number($(p + "Interval").value) || 1,
    until: $(p + "Until").value || null,
  };
}

function setRepeat(p, rep, untilIso){
  const r = rep || { kind: "none" };
  $(p + "Kind").value = r.kind || "none";
  for (let d = 0; d < 7; d++) $(p + "Day" + d).checked = !!(r.days && r.days.indexOf(d) >= 0);
  $(p + "Interval").value = r.interval || 1;
  $(p + "Until").value = untilIso || "";
}

async function updateRepeatText(p, dateId){
  const r = readRepeat(p);
  $(p + "DaysRow").style.display = r.kind === "weekly" ? "flex" : "none";
  $(p + "EveryRow").style.display = (r.kind === "none" || r.kind === "weekdays") ? "none" : "flex";
  $(p + "UntilRow").style.display = r.kind === "none" ? "none" : "flex";
  $(p + "Unit").textContent = REPEAT_UNIT[r.kind] || "";
  const date = $(dateId) && $(dateId).value ? $(dateId).value : "";
  const q = "/api/calendar/repeat?lang=es&kind=" + encodeURIComponent(r.kind) +
    "&days=" + r.days.join(",") + "&interval=" + r.interval +
    (r.until ? "&until=" + r.until : "") + (date ? "&date=" + date : "");
  try { $(p + "Text").textContent = (await api(q)).text; } catch (e) {}
}

function wireRepeat(p, dateId){
  $(p + "RepeatBox").innerHTML = repeatControls(p);
  $(p + "RepeatBox").addEventListener("change", () => updateRepeatText(p, dateId));
  $(p + "RepeatBox").addEventListener("input", () => updateRepeatText(p, dateId));
  updateRepeatText(p, dateId);
}

// ── Calendario ──────────────────────────────────────────────────────────────
let calMonth = "";
let calDay = "";
let calData = { events: [], days: [], today: "" };

function monthAdd(first, n){
  const y = Number(first.slice(0, 4));
  const m = Number(first.slice(5, 7)) - 1 + n;
  const yy = y + Math.floor(m / 12);
  const mm = ((m % 12) + 12) % 12 + 1;
  return String(yy) + "-" + String(mm).padStart(2, "0") + "-01";
}
function monthLast(first){
  return new Date(Date.UTC(Number(first.slice(0, 4)), Number(first.slice(5, 7)), 0)).getUTCDate();
}
function dayAdd(date, n){
  return new Date(new Date(date + "T00:00:00Z").getTime() + n * 86400000).toISOString().slice(0, 10);
}

async function loadCalendar(){
  if (!calMonth) calMonth = new Date().toISOString().slice(0, 8) + "01";
  const to = calMonth.slice(0, 8) + String(monthLast(calMonth)).padStart(2, "0");
  calData = await api("/api/calendar?lang=es&from=" + calMonth + "&to=" + to);
  if (!calDay || calDay < calMonth || calDay > to) calDay = calData.today >= calMonth && calData.today <= to ? calData.today : calMonth;
  renderMonth();
  renderDay();
}

function renderMonth(){
  $("calTitle").textContent = MONTHS[Number(calMonth.slice(5, 7)) - 1] + " " + calMonth.slice(0, 4);
  const byDate = {};
  calData.days.forEach((d) => { byDate[d.date] = d; });
  let html = [1, 2, 3, 4, 5, 6, 0].map((d) => "<div class='h'>" + DOW_SHORT[d] + "</div>").join("");
  const firstDow = (new Date(calMonth + "T00:00:00Z").getUTCDay() + 6) % 7;  // la grilla arranca el lunes
  const start = dayAdd(calMonth, -firstDow);
  for (let i = 0; i < 42; i++) {
    const date = dayAdd(start, i);
    const info = byDate[date];
    const cls = "d" + (date.slice(0, 7) !== calMonth.slice(0, 7) ? " off" : "") +
      (date === calDay ? " sel" : "") + (date === calData.today ? " today" : "");
    html += "<button class='" + cls + "' data-act='calday' data-date='" + date + "'><b>" + Number(date.slice(8, 10)) + "</b>" +
      (info ? "<i>" + esc(info.firstTitle) + (info.count > 1 ? " +" + (info.count - 1) : "") + "</i>" : "") + "</button>";
  }
  $("calGrid").innerHTML = html;
}

function renderDay(){
  $("calDayTitle").textContent = calDay ? calDay.slice(8, 10) + "/" + calDay.slice(5, 7) + "/" + calDay.slice(0, 4) : "";
  const items = (calData.events || []).filter((i) => i.date === calDay);
  $("calDayList").innerHTML = items.map((i) => {
    const when = i.allDay ? "todo el día" : i.time + (i.endTime && i.endTime !== i.time ? " a " + i.endTime : "");
    const tag = i.kind === "reminder" ? "recordatorio" : i.kind === "trip" ? "viaje" : "";
    const extra = [when, i.place, i.repeatText, tag, i.days > 1 ? "día " + i.dayIndex + " de " + i.days : ""].filter(Boolean).join(" · ");
    const buttons = i.kind === "reminder"
      ? btn("Hecho", "done", { kind: "reminder", id: i.id }) + btn("Editar", "remedit", { rem: JSON.stringify({ id: i.id, title: i.title, at: i.startAt, repeat: i.repeat }) })
      : btn("Editar", "caledit", { key: i.key }) + btn("Borrar", "caldel", { id: i.id });
    return row(i.title, extra, buttons);
  }).join("") || empty("Nada este día");
}

function fillEvent(key){
  const o = (calData.events || []).filter((i) => i.key === key)[0];
  if (!o) return;
  $("evId").value = o.id;
  $("evTitle").value = o.title;
  $("evAllDay").checked = o.allDay;
  $("evDate").value = o.startAt.slice(0, 10);
  $("evEndDate").value = o.endAt.slice(0, 10);
  $("evTime").value = o.allDay ? "" : o.startAt.slice(11, 16);
  $("evEndTime").value = o.allDay ? "" : o.endAt.slice(11, 16);
  $("evPlace").value = o.place || "";
  $("evNote").value = o.note || "";
  setRepeat("ev", o.repeat, o.repeat && o.repeat.until ? new Date(o.repeat.until * 1000).toISOString().slice(0, 10) : "");
  updateRepeatText("ev", "evDate");
  $("evFormTitle").textContent = "Editar evento";
  $("tab-calendario").scrollIntoView ? $("evTitle").scrollIntoView({ block: "center" }) : 0;
}

function clearEvent(){
  $("evId").value = "";
  $("evTitle").value = "";
  $("evDate").value = calDay || "";
  $("evEndDate").value = "";
  $("evTime").value = "";
  $("evEndTime").value = "";
  $("evPlace").value = "";
  $("evNote").value = "";
  $("evAllDay").checked = false;
  setRepeat("ev", { kind: "none" }, "");
  updateRepeatText("ev", "evDate");
  $("evFormTitle").textContent = "Evento nuevo";
}

async function saveEvent(){
  const body = {
    id: $("evId").value ? Number($("evId").value) : null,
    title: $("evTitle").value,
    date: $("evDate").value,
    endDate: $("evEndDate").value || $("evDate").value,
    time: $("evAllDay").checked ? "" : $("evTime").value,
    endTime: $("evAllDay").checked ? "" : $("evEndTime").value,
    allDay: $("evAllDay").checked,
    place: $("evPlace").value,
    note: $("evNote").value,
    repeat: readRepeat("ev"),
  };
  if (!body.title.trim()) { toast("Ponle un título"); return; }
  if (!body.date) { toast("Elige el día"); return; }
  try {
    const r = await api("/api/calendar/event", body);
    calDay = r.event.start.slice(0, 10);
    calMonth = calDay.slice(0, 8) + "01";
    clearEvent();
    await loadCalendar();
    toast("Guardado · " + r.repeatText);
  } catch (e) { toast("No se pudo: " + e.message); }
}

function fillReminder(r){
  $("remId").value = r.id || "";
  $("remTitle").value = r.title || "";
  $("remDate").value = r.at ? r.at.slice(0, 10) : "";
  $("remTime").value = r.at && r.at.length > 10 ? r.at.slice(11, 16) : "";
  setRepeat("rem", r.repeat, r.repeat && r.repeat.until ? new Date(r.repeat.until * 1000).toISOString().slice(0, 10) : "");
  updateRepeatText("rem", "remDate");
  $("remFormTitle").textContent = r.id ? "Editando un recordatorio" : "Recordatorios";
  showTab("pizarra");
  $("remTitle").scrollIntoView({ block: "center" });
}

function clearReminder(){
  fillReminder({ id: "", title: "", at: "", repeat: { kind: "none" } });
  $("remFormTitle").textContent = "Recordatorios";
}

document.addEventListener("click", async (ev) => {
  const nav = ev.target.closest("nav button[data-tab]");
  if (nav) { showTab(nav.dataset.tab); return; }
  const b = ev.target.closest("button[data-act]");
  if (!b) return;
  const act = b.dataset.act;
  try {
    if (act === "calday") { calDay = b.dataset.date; renderMonth(); renderDay(); return; }
    if (act === "calprev" || act === "calnext") { calMonth = monthAdd(calMonth, act === "calnext" ? 1 : -1); await loadCalendar(); return; }
    if (act === "caltoday") { calMonth = calData.today.slice(0, 8) + "01"; calDay = calData.today; await loadCalendar(); return; }
    if (act === "caledit") { fillEvent(b.dataset.key); return; }
    if (act === "calnew") { clearEvent(); return; }
    if (act === "caldel") {
      if (!confirm("¿Borrar el evento?")) return;
      await api("/api/calendar/event/delete", { id: Number(b.dataset.id) });
      await loadCalendar();
      toast("Borrado");
      return;
    }
    if (act === "remedit") { fillReminder(JSON.parse(b.dataset.rem)); return; }
    if (act === "dev-rename") {
      const name = prompt("Nombre del aparato", b.dataset.name || "");
      if (name === null) return;
      await api("/api/account/device/rename", { deviceId: b.dataset.id, name });
      await loadDevices();
      toast("Listo");
      return;
    }
    if (act === "dev-del") {
      if (!confirm("El aparato va a dejar de ver los datos de esta cuenta. ¿Lo desvinculamos?")) return;
      await api("/api/account/device/delete", { deviceId: b.dataset.id });
      await loadDevices();
      toast("Desvinculado");
      return;
    }
    if (act === "done") await api("/api/hub/done", { kind: b.dataset.kind, id: Number(b.dataset.id) });
    else if (act === "del") await api("/api/hub/edit", { kind: b.dataset.kind, id: Number(b.dataset.id), action: "delete" });
    else if (act === "delphoto") await api("/api/photos/delete", { id: b.dataset.id });
    else if (act === "testfeed") {
      toast("Probando el feed...");
      const r = await api("/api/board/feed/test", { id: Number(b.dataset.id) });
      toast(r.error ? r.name + ": " + r.error : r.name + ": " + r.count + " titulares");
      return;
    }
    else if (act === "place") {
      const r = await api("/api/hub/location", JSON.parse(b.dataset.place));
      $("placeResults").innerHTML = "";
      toast(r.weather && r.weather.line ? "Lugar guardado · " + r.weather.line : "Lugar guardado");
    } else return;
    await refresh();
  } catch (e) { toast("No se pudo: " + e.message); }
});

async function post(path, body){
  try { await api(path, body); await refresh(); toast("Guardado · sincronizá el aparato"); return true; }
  catch (e) { toast("No se pudo: " + e.message); return false; }
}

function wire(id, path, build){
  $(id).addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    if (await post(path, build(f))) f.reset();
  });
}

async function searchPlace(ev){
  ev.preventDefault();
  const q = $("placeQuery").value.trim();
  if (!q) return;
  $("placeResults").innerHTML = "<li class='muted'>Buscando...</li>";
  try {
    const r = await api("/api/hub/location/search?q=" + encodeURIComponent(q));
    $("placeResults").innerHTML = r.results.map((p) =>
      row(p.label, "", btn("Usar", "place", { place: JSON.stringify(p) }))).join("") || empty("Sin resultados");
  } catch (e) { $("placeResults").innerHTML = empty("No se pudo buscar"); }
}

// La foto se manda tal cual y la convierte el servidor (rota por EXIF, escala a
// 480x800, 4 grises con difuminado).
async function sendPhoto(){
  const input = $("photoInput");
  const files = input.files ? Array.from(input.files) : [];
  if (!files.length) { toast("Elegí una foto"); return; }
  const st = $("photoStatus");
  let done = 0;
  for (const file of files) {
    st.textContent = "Subiendo " + file.name + " (" + Math.round(file.size / 1024) + " KB)...";
    try {
      const r = await fetch("/api/board/photo?name=" + encodeURIComponent(file.name), {
        method: "POST",
        headers: { "Authorization": "Bearer " + token, "Content-Type": file.type || "application/octet-stream" },
        body: file,
      });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) { st.textContent = j.error || ("No se pudo subir " + file.name); break; }
      done++;
    } catch (e) { st.textContent = "No se pudo subir " + file.name; break; }
  }
  if (done) st.textContent = done + (done === 1 ? " foto lista" : " fotos listas");
  input.value = "";
  await refresh();
}

async function loadLog(){
  $("logBox").textContent = "Cargando...";
  try {
    const r = await fetch("/api/log", { headers: { "Authorization": "Bearer " + token } });
    $("logBox").textContent = await r.text();
    $("logBox").scrollTop = $("logBox").scrollHeight;
  } catch (e) { $("logBox").textContent = "No se pudo leer el log"; }
}

async function start(){
  // Lo PRIMERO: saber si este servidor tiene cuentas y si hay sesión. Si no, un
  // 401 de cualquier pedido suelto (el texto de la repetición, por ejemplo)
  // llegaba antes y mostraba la pantalla del token en un servidor con login.
  await loadMe().catch(() => { me = null; });
  applyMe();
  wireRepeat("rem", "remDate");
  wireRepeat("ev", "evDate");
  $("formReminder").addEventListener("submit", async (e) => {
    e.preventDefault();
    const body = {
      id: $("remId").value ? Number($("remId").value) : null,
      title: $("remTitle").value,
      dueAt: $("remDate").value ? ($("remDate").value + ($("remTime").value ? "T" + $("remTime").value : "")) : null,
      repeat: readRepeat("rem"),
    };
    if (!body.title.trim()) { toast("Ponle un título"); return; }
    try {
      const r = await api("/api/board/reminder", body);
      clearReminder();
      await refresh();
      toast("Guardado · " + r.reminder.repeatText);
    } catch (err) { toast("No se pudo: " + err.message); }
  });
  $("remClear").addEventListener("click", clearReminder);
  $("evSave").addEventListener("click", saveEvent);
  $("evNew").addEventListener("click", clearEvent);
  $("evAllDay").addEventListener("change", () => {
    $("evTimeRow").style.display = $("evAllDay").checked ? "none" : "flex";
  });
  wire("formItem", "/api/board/item", (f) => ({ list: $("listSelect").value, text: f.text.value }));
  wire("formNote", "/api/board/note", (f) => ({ text: f.text.value }));
  // El feed se prueba al agregarlo, así que este formulario cuenta cuántos
  // titulares trajo (o dice por qué no trajo ninguno) en vez del "Guardado" seco.
  $("formFeed").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    toast("Buscando el feed...");
    try {
      const r = await api("/api/board/feed", { name: f.name.value, url: f.url.value });
      f.reset();
      await refresh();
      toast(r.name + ": " + r.count + " titulares · " + r.url);
    } catch (e) { toast(e.message); }
  });
  $("formPlace").addEventListener("submit", searchPlace);
  $("photoSend").addEventListener("click", sendPhoto);
  $("tokenSave").addEventListener("click", saveToken);
  $("tokenBtn").addEventListener("click", () => gate("Cambiá el token del aparato"));
  $("setVolume").addEventListener("input", () => { $("volumeOut").textContent = $("setVolume").value + " %"; });
  $("llmPreset").addEventListener("change", () => fillModels($("llmPreset").value, null));
  $("logReload").addEventListener("click", loadLog);

  $("settingsSave").addEventListener("click", async () => {
    await post("/api/board/settings", {
      lang: $("setLang").value,
      speak: $("setSpeak").value,
      musicVolume: Number($("setVolume").value),
      translatorLang: $("setTranslator").value,
    });
  });

  $("aiSave").addEventListener("click", async () => {
    const preset = (cfg.presets || {})[$("llmPreset").value] || {};
    const body = {
      llm: { provider: preset.provider, baseUrl: $("llmBase").value, model: $("llmModel").value, key: $("llmKey").value },
      stt: { baseUrl: $("sttBase").value, model: $("sttModel").value, key: $("sttKey").value },
      search: {
        enabled: $("searchOn").checked,
        provider: $("searchProvider").value,
        maxUses: Number($("searchMax").value),
        key: $("searchKey").value,
      },
    };
    try {
      await api("/api/board/config", body);
      $("llmKey").value = "";
      $("sttKey").value = "";
      $("searchKey").value = "";
      await loadConfig();
      toast("Proveedor guardado");
    } catch (e) { toast("No se pudo: " + e.message); }
  });

  $("assetsReload").addEventListener("click", () => loadAssets().catch((e) => toast(e.message)));
  $("assetsBuild").addEventListener("click", async () => {
    await api("/api/assets/build", {});
    toast("Generando");
    setTimeout(() => loadAssets().catch(() => {}), 1500);
  });
  $("aiTest").addEventListener("click", async () => {
    $("aiTestOut").textContent = "Probando...";
    try {
      const r = await api("/api/board/config/test", {});
      $("aiTestOut").textContent = "Modelo: " + r.llm + "\\nTranscripción: " + r.stt + "\\nBúsqueda: " + r.search;
    } catch (e) { $("aiTestOut").textContent = "No se pudo probar: " + e.message; }
  });

  $("tokenChange").addEventListener("click", async () => {
    const v = $("newToken").value.trim();
    if (v.length < 8) { toast("Poné un token de al menos 8 caracteres"); return; }
    if (!confirm("El aparato va a necesitar este token nuevo (web UI del aparato → Servidor). El anterior sigue funcionando. ¿Seguimos?")) return;
    try {
      await api("/api/board/config", { deviceToken: v });
      $("newToken").value = "";
      await loadConfig();
      toast("Token guardado");
    } catch (e) { toast("No se pudo: " + e.message); }
  });

  $("loginForm").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const email = $("loginEmail").value.trim();
    const pass = $("loginPass").value;
    const creating = $("login").dataset.mode === "register";
    try {
      await authPost(creating ? "/auth/register" : "/auth/login", { email, password: pass });
      $("loginPass").value = "";
      await enter();
    } catch (e) { $("loginMsg").textContent = e.message; }
  });
  $("loginSwitch").addEventListener("click", () => {
    const creating = $("login").dataset.mode === "register";
    $("login").dataset.mode = creating ? "login" : "register";
    $("loginTitle").textContent = creating ? "Entrar" : "Crear cuenta";
    $("loginGo").textContent = creating ? "Entrar" : "Crear cuenta";
    $("loginSwitch").textContent = creating ? "Crear una cuenta" : "Ya tengo cuenta";
    $("loginMsg").textContent = "";
  });
  $("logout").addEventListener("click", async () => {
    await fetch("/auth/logout", { method: "POST", credentials: "same-origin" });
    location.reload();
  });
  $("pairForm").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    try {
      const r = await api("/api/account/pair", { code: $("pairCode").value.trim(), name: $("pairName").value.trim() });
      $("pairCode").value = "";
      $("pairName").value = "";
      await loadDevices();
      toast("Aparato vinculado: " + r.deviceId);
    } catch (e) { toast("No se pudo: " + e.message); }
  });
  $("passForm").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    try {
      await api("/api/account/password", { current: $("passOld").value, password: $("passNew").value });
      $("passOld").value = "";
      $("passNew").value = "";
      toast("Contraseña cambiada");
    } catch (e) { toast("No se pudo: " + e.message); }
  });

  showTab(localStorage.getItem("boardTab") || "pizarra");
  await enter();
}

// Decide qué pantalla mostrar: login (multiusuario), token (como siempre) o
// directamente la aplicación. El estado de la sesión ya se cargó en start().
async function enter(){
  const info = me;
  if (info && info.multi) {
    // Con cuentas no hay token en el navegador: la sesión va en la cookie.
    token = "";
    localStorage.removeItem("deviceToken");
    if (!info.ok) { showLogin(""); return; }
    applyMe();
    screen("app");
    refresh().catch((e) => showLogin("No se pudo conectar: " + e.message));
    return;
  }
  // Un solo usuario: exactamente como siempre, con el token del aparato.
  applyMe();
  if (!token) { gate("Está en la web UI del aparato → Servidor"); return; }
  screen("app");
  refresh().catch((e) => gate("No se pudo conectar: " + e.message));
}

start();

// ── Viajes ───────────────────────────────────────────────────────────────────
// Todo lo de esta pestaña cuelga de sus propios listeners y de data-act que
// empiezan con "trip-": el delegador de arriba los ignora (cae en su else) y
// así esta parte no se pisa con el resto de la página.
let tripId = localStorage.getItem("boardTrip") || "";
let tripData = null;

const TRIP_KINDS = [
  ["flight", "Vuelo"], ["train", "Tren"], ["hotel", "Hotel"], ["ticket", "Entrada"],
  ["meal", "Comida"], ["visit", "Visita"], ["other", "Otro"],
];

function tripKindName(k){
  for (const p of TRIP_KINDS) if (p[0] === k) return p[1];
  return "Otro";
}

async function tripsLoad(){
  const r = await api("/api/trips?lang=es");
  $("tripList").innerHTML = r.trips.map((t) => {
    const when = t.start + (t.end !== t.start ? " a " + t.end : "");
    const state = t.state === "now" ? "en curso" : t.state === "past" ? "terminado" : "próximo";
    return row(t.name + (t.place ? " · " + t.place : ""), when + " · " + t.items + " cosas · " + state,
      btn(t.id === tripId ? "Abierto" : "Abrir", "trip-open", { id: t.id }, t.id === tripId ? "" : "ghost") +
      btn("Borrar", "trip-del", { id: t.id }, "ghost"));
  }).join("") || empty("Todavía no hay viajes");
  if (tripId && !r.trips.some((t) => t.id === tripId)) tripId = "";
  await tripShow();
}

async function tripShow(){
  const on = !!tripId;
  for (const id of ["tripCard", "tripDocsCard", "tripPackCard"]) $(id).style.display = on ? "block" : "none";
  localStorage.setItem("boardTrip", tripId);
  if (!on) { tripData = null; return; }
  const r = await api("/api/trip?id=" + encodeURIComponent(tripId) + "&lang=es");
  tripData = r.trip;
  $("tripTitle").textContent = tripData.name;
  $("tripDates").textContent = tripData.start + " a " + tripData.end +
    (tripData.place ? " · " + tripData.place : "") + " · hoy es " + r.today;

  $("tripItemDate").innerHTML = tripData.days.map((d) =>
    "<option value='" + esc(d.date) + "'>" + esc(d.date) + "</option>").join("");

  const targets = ["<option value=''>Papeles del viaje</option>"];
  tripData.days.forEach((d) => d.items.forEach((i) => {
    targets.push("<option value='" + esc(d.date + "|" + i.id) + "'>" +
      esc(d.date + " " + (i.at || "") + " " + i.title) + "</option>");
  }));
  $("attachTarget").innerHTML = targets.join("");

  $("tripDays").innerHTML = tripData.days.map((d) => {
    const items = d.items.map((i) => {
      const head = (i.at || "--:--") + "  " + i.title;
      const sub = i.kindLabel + (i.place ? " · " + i.place : "") + (i.note ? " · " + i.note : "");
      const atts = i.attachments.map((a) => tripAttRow(a, d.date, i.id)).join("");
      return row(head, sub, btn("Borrar", "trip-delitem", { id: i.id, date: d.date }, "ghost")) + atts;
    }).join("");
    return "<h3>" + esc(d.date) + " · " + d.items.length + "</h3><ul>" + (items || empty("Nada ese día")) + "</ul>";
  }).join("");

  $("tripDocs").innerHTML = tripData.docs.map((a) => tripAttRow(a, "", "")).join("") ||
    empty("Sin papeles sueltos");

  $("tripPacking").innerHTML = tripData.packing.map((p) =>
    row((p.done ? "OK  " : "") + p.text, "",
      btn(p.done ? "Desmarcar" : "Listo", "trip-pack", { id: p.id, done: p.done ? "0" : "1" }, "ghost") +
      btn("Borrar", "trip-packdel", { id: p.id }, "ghost"))).join("") || empty("Nada anotado");
}

// Una fila de adjunto: lo que se extrajo y si el código sirve de verdad.
function tripAttRow(a, date, itemId){
  const bits = [];
  if (a.codes.length) {
    const c = a.codes[0];
    bits.push(c.copy ? "código copiado (puede no escanear)" : c.verified ? c.format + " verificado" : c.format);
  }
  bits.push(a.pages + (a.pages === 1 ? " página" : " páginas"));
  for (const f of a.fields.slice(0, 4)) bits.push(f.label + ": " + f.value);
  return "<li><span><small>" + esc("[" + a.name + "] " + bits.join(" · ")) + "</small></span>" +
    btn("Quitar", "trip-unattach", { id: a.id, date: date, item: itemId }, "ghost") +
    btn("Borrar", "trip-delatt", { id: a.id }, "ghost") + "</li>";
}

// El archivo se manda tal como salió del mail: lo convierte el servidor.
async function tripUpload(){
  const input = $("attachInput");
  const file = input.files && input.files[0];
  if (!tripId) { toast("Abre un viaje primero"); return; }
  if (!file) { toast("Elige un archivo"); return; }
  const st = $("attachStatus");
  st.textContent = "Subiendo " + file.name + " (" + Math.round(file.size / 1024) + " KB). El PDF se convierte en el servidor, puede tardar unos segundos.";
  const form = new FormData();
  form.append("file", file);
  try {
    const r = await fetch("/api/board/attachment?trip=" + encodeURIComponent(tripId), {
      method: "POST",
      headers: { "Authorization": "Bearer " + token },
      body: form,
    });
    const j = await r.json();
    if (!r.ok || !j.ok) { st.textContent = j.error || "No se pudo subir"; return; }
    const a = j.attachment;
    const target = $("attachTarget").value;
    if (target) {
      const parts = target.split("|");
      await api("/api/trip/attach", { tripId: tripId, date: parts[0], itemId: parts[1], attachmentId: a.id });
    } else {
      await api("/api/trip/attach", { tripId: tripId, attachmentId: a.id });
    }
    const lines = [a.pages + " página(s) listas para el aparato"];
    if (a.codes.length) {
      const c = a.codes[0];
      lines.push(c.copy
        ? "El código no se pudo leer: va como copia de la imagen y PUEDE NO ESCANEAR, lleva también el original."
        : "Código " + c.format + (c.verified ? " leído y vuelto a generar (verificado)" : " leído"));
    } else if (a.warn) lines.push(a.warn);
    for (const f of a.fields) lines.push(f.label + ": " + f.value);
    st.textContent = lines.join(" · ");
    input.value = "";
    await tripShow();
  } catch (e) { st.textContent = "No se pudo subir: " + e.message; }
}

document.addEventListener("click", async (ev) => {
  const tab = ev.target.closest("nav button[data-tab=viajes]");
  if (tab) { tripsLoad().catch((e) => toast("No se pudo: " + e.message)); return; }
  const b = ev.target.closest("button[data-act]");
  if (!b || b.dataset.act.slice(0, 5) !== "trip-") return;
  const act = b.dataset.act;
  try {
    if (act === "trip-open") { tripId = tripId === b.dataset.id ? "" : b.dataset.id; }
    else if (act === "trip-del") {
      if (!confirm("Borrar el viaje con sus días y sus papeles?")) return;
      await api("/api/trip/delete", { id: b.dataset.id });
      if (tripId === b.dataset.id) tripId = "";
    }
    else if (act === "trip-delitem") await api("/api/trip/day/item/delete", { tripId: tripId, date: b.dataset.date, id: b.dataset.id });
    else if (act === "trip-pack") await api("/api/trip/packing", { tripId: tripId, id: b.dataset.id, done: b.dataset.done === "1" });
    else if (act === "trip-packdel") await api("/api/trip/packing", { tripId: tripId, id: b.dataset.id, action: "delete" });
    else if (act === "trip-unattach") await api("/api/trip/attach", { tripId: tripId, date: b.dataset.date, itemId: b.dataset.item, attachmentId: b.dataset.id, action: "remove" });
    else if (act === "trip-delatt") {
      if (!confirm("Borrar el adjunto y sus páginas?")) return;
      await api("/api/attachment/delete", { id: b.dataset.id });
    }
    else return;
    await tripsLoad();
  } catch (e) { toast("No se pudo: " + e.message); }
});

function tripStart(){
  $("formTrip").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    try {
      const r = await api("/api/trip", { name: f.name.value, place: f.place.value, start: f.start.value, end: f.end.value });
      tripId = r.id;
      f.reset();
      await tripsLoad();
      toast("Viaje creado");
    } catch (e) { toast("No se pudo: " + e.message); }
  });
  $("formTripItem").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    try {
      await api("/api/trip/day/item", {
        tripId: tripId, date: f.date.value, at: f.at.value,
        kind: f.kind.value, title: f.title.value, place: f.place.value,
      });
      f.title.value = "";
      f.place.value = "";
      await tripsLoad();
      toast("Agregado · sincroniza el aparato");
    } catch (e) { toast("No se pudo: " + e.message); }
  });
  $("formPacking").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    try {
      await api("/api/trip/packing", { tripId: tripId, text: f.text.value });
      f.reset();
      await tripsLoad();
    } catch (e) { toast("No se pudo: " + e.message); }
  });
  $("attachSend").addEventListener("click", tripUpload);
  if (token && localStorage.getItem("boardTab") === "viajes") tripsLoad().catch(() => {});
}
tripStart();
`;

const PAGE = `<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pizarra</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='6' fill='%23111'/%3E%3Crect x='8' y='9' width='16' height='2.5' fill='%23fff'/%3E%3Crect x='8' y='15' width='16' height='2.5' fill='%23fff'/%3E%3Crect x='8' y='21' width='10' height='2.5' fill='%23fff'/%3E%3C/svg%3E">
<style>${STYLE}</style></head><body>
<header><strong>Pizarra del aparato</strong><span class="muted" id="who"></span><button id="tokenBtn">Token</button></header>
<nav id="nav" style="display:none">
  <button data-tab="pizarra">Pizarra</button>
  <button data-tab="calendario">Calendario</button>
  <button data-tab="listas">Listas</button>
  <button data-tab="notas">Notas</button>
  <button data-tab="fotos">Fotos</button>
  <button data-tab="noticias">Noticias</button>
  <button data-tab="viajes">Viajes</button>
  <button data-tab="aparatos" style="display:none">Aparatos</button>
  <button data-tab="ia">IA</button>
  <button data-tab="ajustes">Ajustes</button>
  <button data-tab="log">Log</button>
</nav>

<div id="gate" style="display:none">
  <h2>Token del aparato</h2>
  <p class="muted" id="gateMsg"></p>
  <p><input type="text" id="tokenInput" placeholder="Token" style="width:80%"></p>
  <p><button id="tokenSave">Entrar</button></p>
</div>

<div id="login" style="display:none" data-mode="login">
  <h2 id="loginTitle">Entrar</h2>
  <p class="muted" id="loginMsg"></p>
  <form id="loginForm">
    <input type="email" id="loginEmail" placeholder="Correo" autocomplete="username" required>
    <input type="password" id="loginPass" placeholder="Contraseña" autocomplete="current-password" required>
    <button id="loginGo">Entrar</button>
  </form>
  <p><button class="ghost" id="loginSwitch">Crear una cuenta</button></p>
  <p class="muted">La contraseña necesita al menos 8 caracteres. Después de entrar, vincula el aparato
    con el código de 6 dígitos que muestra su pantalla.</p>
</div>

<main id="app" style="display:none">

<section class="tab" id="tab-pizarra">
  <div class="card"><h2 id="remFormTitle">Recordatorios</h2>
    <form id="formReminder">
      <input type="hidden" id="remId">
      <input type="text" id="remTitle" placeholder="Qué" required>
      <input type="date" id="remDate"><input type="time" id="remTime">
      <div id="remRepeatBox" style="width:100%"></div>
      <button>Guardar</button><button type="button" class="ghost" id="remClear">Nuevo</button>
    </form>
    <p class="muted">"Va a sonar" es exactamente lo que muestra el aparato, así se sabe de antemano
      qué días te va a despertar. Con "Editar" se cambia uno que ya está.</p>
    <ul id="reminders"></ul>
  </div>
  <div class="card"><h2>Memoria del asistente</h2>
    <p class="muted">Lo que el aparato recuerda de ti y usa para contestarte mejor. Dile
      "recuerda que soy vegetariano" o "mi hija se llama Ana" y entra en cada pregunta que le hagas,
      en el hub y leyendo un libro. Si lo corriges ("ya no vivo en México"), reemplaza el dato viejo.
      Guarda hasta 40 datos; borra el que ya no quieras.</p>
    <ul id="memories"></ul>
  </div>
</section>

<section class="tab" id="tab-calendario">
  <div class="card">
    <div class="calhead">
      <button class="ghost" data-act="calprev">‹</button>
      <b id="calTitle">—</b>
      <span><button class="ghost" data-act="caltoday">Hoy</button></span>
      <button class="ghost" data-act="calnext">›</button>
    </div>
    <div class="cal" id="calGrid"></div>
    <p class="muted">Salen los eventos que cargues acá, los recordatorios (con su repetición ya
      resuelta) y los días de viaje.</p>
  </div>
  <div class="card"><h2>Día <span id="calDayTitle"></span></h2>
    <ul id="calDayList"></ul>
  </div>
  <div class="card"><h2 id="evFormTitle">Evento nuevo</h2>
    <input type="hidden" id="evId">
    <div class="row"><label for="evTitle">Qué</label><input type="text" id="evTitle" placeholder="Título"></div>
    <div class="row"><label for="evDate">Cuándo</label><input type="date" id="evDate"><span class="muted">a</span><input type="date" id="evEndDate"></div>
    <div class="row" id="evTimeRow"><label for="evTime">Hora</label><input type="time" id="evTime"><span class="muted">a</span><input type="time" id="evEndTime"></div>
    <div class="row"><label><input type="checkbox" id="evAllDay"> Todo el día</label></div>
    <div class="row"><label for="evPlace">Dónde</label><input type="text" id="evPlace" placeholder="Lugar (opcional)"></div>
    <div class="row"><label for="evNote">Nota</label><input type="text" id="evNote" placeholder="Opcional"></div>
    <div id="evRepeatBox"></div>
    <div class="row"><button id="evSave">Guardar</button><button class="ghost" id="evNew">Nuevo</button></div>
  </div>
</section>

<section class="tab" id="tab-listas">
  <div class="card"><h2>Listas</h2>
    <p class="muted">Hay dos listas y nada más: Compras y Tareas. Todo lo que dictes que no sea una compra
      va a Tareas.</p>
    <form id="formItem"><select id="listSelect"></select><input type="text" name="text" placeholder="Ítem" required><button>Agregar</button></form>
    <div id="listItems"></div>
  </div>
</section>

<section class="tab" id="tab-notas">
  <div class="card"><h2>Notas</h2>
    <form id="formNote"><textarea name="text" rows="8" placeholder="Nota" required></textarea><button>Guardar</button></form>
    <p class="muted">Las notas pueden ser largas (hasta 20.000 caracteres). En el aparato también se dictan:
      presiona Atrás dos veces y di "nota: ...".</p>
    <ul id="notes"></ul>
  </div>
</section>

<section class="tab" id="tab-fotos">
  <div class="card"><h2>Fotos</h2>
    <p class="muted">Subilas tal como salen del teléfono: el servidor las rota, las escala a 480x800 y las pasa a 4 grises. Se pueden elegir varias.</p>
    <div class="row"><input type="file" id="photoInput" accept="image/*" multiple><button type="button" id="photoSend">Subir</button></div>
    <p class="muted" id="photoStatus"></p>
    <ul id="photos"></ul>
  </div>
</section>

<section class="tab" id="tab-noticias">
  <div class="card"><h2>Noticias (RSS)</h2>
    <form id="formFeed"><input type="text" name="name" placeholder="Nombre (opcional)" style="max-width:130px"><input type="text" name="url" placeholder="https://.../rss o la página del diario" required><button>Agregar</button></form>
    <p class="muted">Sirve la dirección del feed o la del diario: si es una página web, el servidor busca adentro el feed que declara. Se prueba antes de guardarlo.</p>
    <ul id="feeds"></ul>
  </div>
</section>

<section class="tab" id="tab-viajes">
  <div class="card"><h2>Viajes</h2>
    <p class="muted">Un viaje son sus días, lo que se hace cada día con su hora, y los papeles.
      Lo que cargues aquí aparece también en el calendario del aparato.</p>
    <form id="formTrip">
      <input type="text" name="name" placeholder="Nombre (Roma, Madrid...)" required>
      <input type="text" name="place" placeholder="Lugar" style="max-width:130px">
      <input type="date" name="start" required>
      <input type="date" name="end" required>
      <button>Crear</button>
    </form>
    <ul id="tripList"></ul>
  </div>

  <div class="card" id="tripCard" style="display:none">
    <h2 id="tripTitle">-</h2>
    <p class="muted" id="tripDates"></p>
    <form id="formTripItem">
      <select id="tripItemDate" name="date"></select>
      <input type="time" name="at" style="max-width:110px">
      <select name="kind">
        <option value="flight">Vuelo</option><option value="train">Tren</option>
        <option value="hotel">Hotel</option><option value="ticket">Entrada</option>
        <option value="meal">Comida</option><option value="visit">Visita</option>
        <option value="other" selected>Otro</option>
      </select>
      <input type="text" name="title" placeholder="Qué (tren a Termini, entrada al Vaticano...)" required>
      <input type="text" name="place" placeholder="Dónde" style="max-width:130px">
      <button>Agregar</button>
    </form>
    <div id="tripDays"></div>
  </div>

  <div class="card" id="tripDocsCard" style="display:none"><h2>Papeles</h2>
    <p class="muted">Sube el PDF del vuelo, de la reserva o de la entrada tal como te llegó al correo.
      El servidor lo convierte a páginas que el aparato pinta de una, vuelve a generar el código de barras
      (PDF417, Aztec o QR) en blanco y negro puro para que escanee, y saca los datos que sirven.
      Si un código no se puede leer, se avisa aquí: esa copia puede no escanear.</p>
    <div class="row">
      <select id="attachTarget"></select>
      <input type="file" id="attachInput" accept="application/pdf,image/*">
      <button type="button" id="attachSend">Subir</button>
    </div>
    <p class="muted" id="attachStatus"></p>
    <ul id="tripDocs"></ul>
  </div>

  <div class="card" id="tripPackCard" style="display:none"><h2>Para llevar</h2>
    <form id="formPacking"><input type="text" name="text" placeholder="Cosa" required><button>Agregar</button></form>
    <ul id="tripPacking"></ul>
  </div>
</section>

<section class="tab" id="tab-aparatos">
  <div class="card"><h2>Vincular un aparato</h2>
    <p class="muted">En el aparato entra a Ajustes &rarr; Vincular: muestra un código de 6 dígitos que dura
      10 minutos. Escríbelo aquí y el aparato pasa a esta cuenta. Si estaba en otra, se mueve a esta.</p>
    <form id="pairForm">
      <input type="text" id="pairCode" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" placeholder="482913" style="max-width:120px" required>
      <input type="text" id="pairName" placeholder="Nombre (el lector de la cocina)" maxlength="60">
      <button>Vincular</button>
    </form>
  </div>
  <div class="card"><h2>Mis aparatos</h2>
    <ul id="devices"></ul>
  </div>
  <div class="card"><h2>Mi cuenta</h2>
    <form id="passForm">
      <input type="password" id="passOld" placeholder="Contraseña actual" autocomplete="current-password" required>
      <input type="password" id="passNew" placeholder="Contraseña nueva" autocomplete="new-password" required>
      <button>Cambiar contraseña</button>
    </form>
    <p><button class="ghost" id="logout">Salir</button></p>
  </div>
</section>

<section class="tab" id="tab-ia">
  <div class="card"><h2>Modelo de texto</h2>
    <p class="muted">Es el que entiende lo que decís, responde preguntas y traduce. Se puede cambiar sin tocar el aparato.</p>
    <div class="row"><label for="llmPreset">Proveedor</label><select id="llmPreset"></select></div>
    <div class="row" id="llmBaseRow"><label for="llmBase">URL</label><input type="text" id="llmBase" placeholder="https://api.groq.com/openai/v1"></div>
    <div class="row"><label for="llmModel">Modelo</label><select id="llmModel"></select></div>
    <div class="row"><label for="llmKey">Clave</label><input type="password" id="llmKey" placeholder="dejala vacía para no cambiarla" autocomplete="off"><span id="llmKeyState" class="muted"></span></div>
  </div>
  <div class="card"><h2>Buscar en internet</h2>
    <p class="muted">Para que conteste cosas de ahora (quién ganó, a cuánto está, qué pasó hoy) en vez de lo que se acuerda de cuando lo entrenaron. No busca en todas las preguntas: solo cuando hace falta.</p>
    <p class="muted">Con Claude busca el modelo solo: cuesta unos <b>USD 0,01 por búsqueda</b> (USD 10 cada 1000) más los tokens de lo que lee. Con Groq, DeepSeek u OpenAI busca este servidor: <b>gratis</b> con Google Noticias y DuckDuckGo, o mejor con una clave de Tavily o Brave (los dos tienen plan gratis mensual).</p>
    <div class="row"><label><input type="checkbox" id="searchOn"> Buscar cuando haga falta</label></div>
    <div class="row"><label for="searchProvider">Buscador</label>
      <select id="searchProvider">
        <option value="free">Gratis (Google Noticias + DuckDuckGo)</option>
        <option value="tavily">Tavily (con clave)</option>
        <option value="brave">Brave (con clave)</option>
      </select></div>
    <div class="row"><label for="searchMax">Máximo de búsquedas por respuesta</label><input type="number" id="searchMax" min="1" max="10" style="max-width:90px"></div>
    <div class="row"><label for="searchKey">Clave del buscador</label><input type="password" id="searchKey" placeholder="dejala vacía para no cambiarla" autocomplete="off"><span id="searchKeyState" class="muted"></span></div>
    <p class="muted">El buscador solo se usa con los proveedores tipo OpenAI; con Claude la búsqueda ya viene incluida.</p>
  </div>
  <div class="card"><h2>Transcripción de voz</h2>
    <p class="muted">Lo que pasa tu voz a texto. Groq (whisper-large-v3-turbo) es gratis y el más rápido.</p>
    <div class="row"><label for="sttBase">URL</label><input type="text" id="sttBase"></div>
    <div class="row"><label for="sttModel">Modelo</label><input type="text" id="sttModel"></div>
    <div class="row"><label for="sttKey">Clave</label><input type="password" id="sttKey" placeholder="dejala vacía para no cambiarla" autocomplete="off"><span id="sttKeyState" class="muted"></span></div>
    <div class="row"><label><input type="checkbox" id="sttFollow" checked> seguir al proveedor</label></div>
    <div class="row"><button id="aiSave">Guardar</button><button class="ghost" id="aiTest">Probar</button></div>
    <pre id="aiTestOut" class="muted"></pre>
  </div>
  <div class="card"><h2>Cuánto sale cada consulta</h2>
    <p class="muted">Una consulta de voz típica: <span id="costShape">5 s de audio, 1500 tokens de entrada y 300 de salida</span>.
      Incluye la transcripción con el modelo que tengas elegido. Precios revisados el 08/09/2026.</p>
    <div style="overflow-x:auto"><table id="costs" class="costs"></table></div>
    <p class="muted" id="costNote"></p>
  </div>
  <div class="card"><h2>Token del aparato</h2>
    <p class="muted" id="tokenState"></p>
    <div class="row"><input type="text" id="newToken" placeholder="token nuevo"><button class="danger" id="tokenChange">Cambiar</button></div>
    <p class="muted">El token del entorno sigue valiendo siempre, así que no te podés dejar afuera. Después hay que ponerlo también en la web UI del aparato → Servidor.</p>
  </div>
</section>

<section class="tab" id="tab-ajustes">
  <div class="card"><h2>Clima</h2>
    <p class="muted">Lugar: <b id="place">—</b></p>
    <p class="muted">En el servidor: <span id="weatherNow">—</span></p>
    <form id="formPlace"><input type="text" id="placeQuery" placeholder="Ciudad" required><button>Buscar</button></form>
    <ul id="placeResults"></ul>
  </div>
  <div class="card"><h2>Aparato</h2>
    <div class="row"><label for="setLang">Idioma</label>
      <select id="setLang">
        <option value="es">Español</option><option value="en">English</option><option value="fr">Français</option>
        <option value="de">Deutsch</option><option value="pt">Português</option><option value="ru">Русский</option>
      </select></div>
    <div class="row"><label for="setSpeak">Voz hablada</label>
      <select id="setSpeak"><option value="none">Nunca</option><option value="short">Respuestas cortas</option><option value="all">Siempre</option></select></div>
    <div class="row"><label for="setTranslator">Traductor: otro idioma</label>
      <select id="setTranslator">
        <option value="en">English</option><option value="es">Español</option><option value="fr">Français</option>
        <option value="de">Deutsch</option><option value="pt">Português</option><option value="ru">Русский</option>
      </select></div>
    <div class="row"><label for="setVolume">Volumen (voz y música)</label><input type="range" id="setVolume" min="0" max="100" step="5"><span id="volumeOut" class="muted"></span></div>
    <div class="row"><button id="settingsSave">Guardar ajustes</button></div>
    <p class="muted" id="lastSync"></p>
  </div>
  <div class="card"><h2>Paquete de contenido</h2>
    <p class="muted">Todo lo pesado (la Biblia entera, los dibujos y la voz de las tarjetas de bebé, los sonidos)
      se genera una sola vez en el servidor y el aparato se lo baja completo después de actualizar el firmware.
      Por eso ya no hay un botón para bajar la Biblia por separado.</p>
    <p class="muted" id="assetsState">—</p>
    <div class="row"><button class="ghost" id="assetsReload">Actualizar</button><button class="ghost" id="assetsBuild">Generar lo que falte</button></div>
    <div style="overflow-x:auto"><table id="assetsTable" class="costs"></table></div>
  </div>
</section>

<section class="tab" id="tab-log">
  <div class="card"><h2>Log del aparato</h2>
    <div class="row"><button class="ghost" id="logReload">Actualizar</button></div>
    <pre id="logBox"></pre>
  </div>
</section>

</main>
<div class="toast" id="toast"></div>
<script>${SCRIPT}</script></body></html>`;

export const board = new Hono();
board.get("/", (c) => c.html(PAGE));
