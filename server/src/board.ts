// La "app del teléfono": una página web del Hono para manejar el aparato desde
// cualquier navegador. Pide el token del aparato una vez (queda en
// localStorage) y usa la misma API que el aparato. Sirve para dejar mensajes en
// la pizarra del hub, crear recordatorios con fecha y repetición, manejar las
// listas, escribir notas, subir fotos, cargar feeds, ver la memoria del
// asistente y configurar el aparato (lugar del clima, idioma, voz, volumen).
//
//   GET  /board                      página (sin token; el JS lo pide)
//   GET  /board/log                  el log que sube el aparato (devicelog.ts)
//   POST /api/board/message  {from, text}
//   POST /api/board/reminder {title, dueAt: "YYYY-MM-DDTHH:MM"|null, repeat}
//   POST /api/board/item     {list, text}
//   POST /api/board/note     {text}
//   POST /api/board/feed     {name, url}
//   POST /api/board/photo?name=      (body: image/bmp de 2 bpp, lo arma el navegador)
//   POST /api/board/list     {name}          crea una lista
//   POST /api/board/list/delete {name}       la borra con todo lo que tenga
//   GET  /api/board/extra    -> {feeds, memories, settings, lists}
//   POST /api/board/settings {lang, speak, musicVolume, translatorLang}
//   (leer, tildar y borrar: GET /api/hub, POST /api/hub/done, POST /api/hub/edit)
import { Hono } from "hono";
import { load, save, nextId, resolveList, DEFAULT_SETTINGS, type Settings } from "./store";
import { savePhoto, toDeviceBmp, MAX_UPLOAD_BYTES } from "./photos";
import { hubDiagnostics } from "./hub";
import { config, saveConfig, publicConfig, type Config } from "./config";
import { chatText, providerLabel, searchToolLabel } from "./llm";
import { searchWeb } from "./websearch";
import { checkUrl, isSafeRemoteUrl, readBody } from "./net";
import { probeFeed, checkFeed } from "./rss";

export const boardApi = new Hono();

boardApi.post("/message", async (c) => {
  const b = await readBody(c);
  const text = (b.text ?? "").toString().trim().slice(0, 300);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.messages.push({ id: nextId(store), from: (b.from ?? "").toString().trim().slice(0, 40) || "web", text, createdAt: new Date().toISOString(), read: false });
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/reminder", async (c) => {
  const b = await readBody(c);
  const title = (b.title ?? "").toString().trim().slice(0, 200);
  if (!title) return c.json({ ok: false, error: "title required" }, 400);
  const dueAt = typeof b.dueAt === "string" && /^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2})?$/.test(b.dueAt) ? b.dueAt : null;
  const repeat = ["none", "daily", "weekly", "monthly"].includes(b.repeat) ? b.repeat : "none";
  const store = await load();
  store.reminders.push({ id: nextId(store), title, dueAt, repeat, done: false, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/item", async (c) => {
  const b = await readBody(c);
  const text = (b.text ?? "").toString().trim().slice(0, 200);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  const list = resolveList(store, b.list, true);
  store.lists[list].push({ id: nextId(store), text, done: false, dueDate: null, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true, list });
});

boardApi.post("/list", async (c) => {
  const b = await readBody(c);
  const name = (b.name ?? "").toString().trim().slice(0, 40);
  if (!name) return c.json({ ok: false, error: "name required" }, 400);
  const store = await load();
  const list = resolveList(store, name, true);
  await save(store);
  return c.json({ ok: true, list });
});

boardApi.post("/list/delete", async (c) => {
  const b = await readBody(c);
  const name = (b.name ?? "").toString();
  const store = await load();
  if (!(name in store.lists)) return c.json({ ok: false, error: "not found" }, 404);
  delete store.lists[name];
  await save(store);
  return c.json({ ok: true });
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
  const store = await load();
  store.feeds ??= [];
  if (store.feeds.some((f) => f.url === probe.url)) return c.json({ ok: false, error: "ese feed ya está cargado" }, 400);
  const name =
    (b.name ?? "").toString().trim().slice(0, 40) ||
    probe.title ||
    new URL(probe.url).hostname.replace(/^www\./, "");
  store.feeds.push({ id: nextId(store), name, url: probe.url });
  await save(store);
  return c.json({ ok: true, name, url: probe.url, count: probe.count });
});

// "Probar" de la pestaña Noticias: baja el feed sin caché y dice cuántos
// titulares trae o por qué no trae ninguno.
boardApi.post("/feed/test", async (c) => {
  const b = await readBody(c);
  const store = await load();
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
    const id = await savePhoto(name, out);
    return c.json({ ok: true, id });
  } catch (err) {
    console.error("photo save:", err);
    return c.json({ ok: false, error: `no se pudo guardar (${String(err).slice(0, 120)})` }, 500);
  }
});

boardApi.get("/extra", async (c) => {
  const store = await load();
  return c.json({
    ok: true,
    feeds: store.feeds ?? [],
    memories: store.memories ?? [],
    settings: store.settings ?? DEFAULT_SETTINGS,
    lists: Object.keys(store.lists),
    diag: await hubDiagnostics(),
  });
});

// Ajustes del aparato. Cada cambio sube `rev`; el aparato los aplica en la
// próxima sincronización solo si la revisión es mayor a la que ya tenía.
boardApi.post("/settings", async (c) => {
  const b = await readBody(c);
  const store = await load();
  const s: Settings = { ...DEFAULT_SETTINGS, ...(store.settings ?? {}) };
  if (typeof b.lang === "string" && /^[a-z]{2}$/.test(b.lang)) s.lang = b.lang;
  if (b.speak === "none" || b.speak === "short" || b.speak === "all") s.speak = b.speak;
  if (Number.isFinite(Number(b.musicVolume))) s.musicVolume = Math.max(0, Math.min(100, Math.round(Number(b.musicVolume))));
  if (typeof b.translatorLang === "string" && /^[a-z]{2}$/.test(b.translatorLang)) s.translatorLang = b.translatorLang;
  s.rev = (s.rev ?? 0) + 1;
  store.settings = s;
  await save(store);
  return c.json({ ok: true, settings: s });
});

// Proveedores de IA y token, configurables desde la web. Las claves entran acá y
// no salen nunca: la página solo ve si hay clave puesta.
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
  else if (conf.llm.provider === "anthropic") out.search = `la hace Claude solo (${searchToolLabel(conf.llm.model)}, hasta ${conf.search.maxUses} por respuesta)`;
  else {
    const t1 = Date.now();
    const r = await searchWeb("noticias de hoy", "es", 3);
    out.search = r.error
      ? `ERROR: ${r.error}`
      : `${r.provider}: ${r.results.length} resultados en ${Date.now() - t1} ms · "${(r.results[0]?.title ?? "").slice(0, 60)}"`;
  }
  return c.json({ ok: true, ...out });
});

boardApi.post("/note", async (c) => {
  const b = await readBody(c);
  const text = (b.text ?? "").toString().trim().slice(0, 2000);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.notes.push({ id: nextId(store), text, createdAt: new Date().toISOString() });
  await save(store);
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
`;

// Ojo al editar: esto vive dentro de un template literal, así que un backslash
// antes de una comilla se lo come el literal y rompe el script entero. Regla:
// nada de \\' ni backticks acá adentro; las cadenas van con comillas dobles y los
// atributos HTML con comillas simples. Los botones no llevan onclick: se manejan
// por delegación con data-act.
const SCRIPT = `
let token = localStorage.getItem("deviceToken") || "";
let cfg = null;
const $ = (id) => document.getElementById(id);
function esc(s){ return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\\u0022":"&quot;","\\u0027":"&#39;"}[c])); }
function toast(msg){ const t = $("toast"); t.textContent = msg; t.classList.add("on"); setTimeout(() => t.classList.remove("on"), 2200); }

async function api(path, body, method){
  const r = await fetch(path, {
    method: method || (body ? "POST" : "GET"),
    headers: { "Authorization": "Bearer " + token, "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (r.status === 401) { gate("Token rechazado"); throw new Error("token"); }
  if (!r.ok) {
    let detail = "";
    try { detail = (await r.json()).error || ""; } catch (e) {}
    throw new Error(detail || ("http " + r.status));
  }
  return r.json();
}

function gate(msg){
  $("gate").style.display = "block";
  $("app").style.display = "none";
  $("nav").style.display = "none";
  $("gateMsg").textContent = msg || "";
  $("tokenInput").value = token;
}

function saveToken(){
  const v = $("tokenInput").value.trim();
  if (!v) { $("gateMsg").textContent = "Poné el token"; return; }
  token = v;
  localStorage.setItem("deviceToken", token);
  $("gate").style.display = "none";
  $("app").style.display = "block";
  $("nav").style.display = "flex";
  refresh().catch((e) => gate("No se pudo conectar: " + e.message));
}

function showTab(name){
  document.querySelectorAll("section.tab").forEach((s) => s.classList.toggle("on", s.id === "tab-" + name));
  document.querySelectorAll("nav button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
  localStorage.setItem("boardTab", name);
  if (name === "log") loadLog();
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

  $("messages").innerHTML = d.messages.map((m) =>
    row(m.from + ": " + m.text, "", btn("Leído", "done", { kind: "message", id: m.id }))).join("") || empty("Sin mensajes");

  $("reminders").innerHTML = d.reminders.map((r) =>
    row(r.title, r.when || "sin hora",
      btn("Hecho", "done", { kind: "reminder", id: r.id }) +
      btn("Borrar", "del", { kind: "reminder", id: r.id }))).join("") || empty("Sin recordatorios");

  const names = x.lists.length ? x.lists : d.lists.map((l) => l.name);
  const sel = $("listSelect");
  const keep = sel.value;
  sel.innerHTML = names.map((n) => "<option>" + esc(n) + "</option>").join("");
  if (names.indexOf(keep) >= 0) sel.value = keep;

  const byName = {};
  d.lists.forEach((l) => { byName[l.name] = l.items; });
  $("listItems").innerHTML = names.map((n) => {
    const items = byName[n] || [];
    return "<h3>" + esc(n) + " · " + items.length + " " + btn("Borrar lista", "dellist", { name: n }) + "</h3><ul>" +
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
}

async function loadConfig(){
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

document.addEventListener("click", async (ev) => {
  const nav = ev.target.closest("nav button[data-tab]");
  if (nav) { showTab(nav.dataset.tab); return; }
  const b = ev.target.closest("button[data-act]");
  if (!b) return;
  const act = b.dataset.act;
  try {
    if (act === "done") await api("/api/hub/done", { kind: b.dataset.kind, id: Number(b.dataset.id) });
    else if (act === "del") await api("/api/hub/edit", { kind: b.dataset.kind, id: Number(b.dataset.id), action: "delete" });
    else if (act === "delphoto") await api("/api/photos/delete", { id: b.dataset.id });
    else if (act === "testfeed") {
      toast("Probando el feed...");
      const r = await api("/api/board/feed/test", { id: Number(b.dataset.id) });
      toast(r.error ? r.name + ": " + r.error : r.name + ": " + r.count + " titulares");
      return;
    }
    else if (act === "dellist") {
      if (!confirm("¿Borrar la lista " + b.dataset.name + " con todo lo que tenga?")) return;
      await api("/api/board/list/delete", { name: b.dataset.name });
    } else if (act === "place") {
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

function start(){
  wire("formMessage", "/api/board/message", (f) => ({ from: f.from.value, text: f.text.value }));
  wire("formReminder", "/api/board/reminder", (f) => ({
    title: f.title.value,
    dueAt: f.date.value ? (f.date.value + (f.time.value ? "T" + f.time.value : "")) : null,
    repeat: f.repeat.value,
  }));
  wire("formItem", "/api/board/item", (f) => ({ list: $("listSelect").value, text: f.text.value }));
  wire("formList", "/api/board/list", (f) => ({ name: f.name.value }));
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

  showTab(localStorage.getItem("boardTab") || "pizarra");
  if (!token) { gate("Está en la web UI del aparato → Servidor"); return; }
  refresh().catch((e) => gate("No se pudo conectar: " + e.message));
}
start();
`;

const PAGE = `<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pizarra</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='6' fill='%23111'/%3E%3Crect x='8' y='9' width='16' height='2.5' fill='%23fff'/%3E%3Crect x='8' y='15' width='16' height='2.5' fill='%23fff'/%3E%3Crect x='8' y='21' width='10' height='2.5' fill='%23fff'/%3E%3C/svg%3E">
<style>${STYLE}</style></head><body>
<header><strong>Pizarra del aparato</strong><button id="tokenBtn">Token</button></header>
<nav id="nav" style="display:none">
  <button data-tab="pizarra">Pizarra</button>
  <button data-tab="listas">Listas</button>
  <button data-tab="notas">Notas</button>
  <button data-tab="fotos">Fotos</button>
  <button data-tab="noticias">Noticias</button>
  <button data-tab="ia">IA</button>
  <button data-tab="ajustes">Ajustes</button>
  <button data-tab="log">Log</button>
</nav>

<div id="gate">
  <h2>Token del aparato</h2>
  <p class="muted" id="gateMsg"></p>
  <p><input type="text" id="tokenInput" placeholder="Token" style="width:80%"></p>
  <p><button id="tokenSave">Entrar</button></p>
</div>

<main id="app" style="display:none">

<section class="tab" id="tab-pizarra">
  <div class="card"><h2>Mensaje para el hub</h2>
    <form id="formMessage">
      <input type="text" name="from" placeholder="De" style="max-width:110px">
      <input type="text" name="text" placeholder="Mensaje" required><button>Dejar</button>
    </form>
    <ul id="messages"></ul>
  </div>
  <div class="card"><h2>Recordatorios</h2>
    <form id="formReminder">
      <input type="text" name="title" placeholder="Qué" required>
      <input type="date" name="date"><input type="time" name="time">
      <select name="repeat">
        <option value="none">Una vez</option><option value="daily">Diario</option>
        <option value="weekly">Semanal</option><option value="monthly">Mensual</option>
      </select><button>Guardar</button>
    </form>
    <ul id="reminders"></ul>
  </div>
  <div class="card"><h2>Memoria del asistente</h2>
    <p class="muted">Lo que le pediste que recuerde ("acordate que..."). Entra en el prompt cuando le hablás.</p>
    <ul id="memories"></ul>
  </div>
</section>

<section class="tab" id="tab-listas">
  <div class="card"><h2>Listas</h2>
    <form id="formItem"><select id="listSelect"></select><input type="text" name="text" placeholder="Ítem" required><button>Agregar</button></form>
    <form id="formList"><input type="text" name="name" placeholder="Lista nueva" required><button>Crear lista</button></form>
    <div id="listItems"></div>
  </div>
</section>

<section class="tab" id="tab-notas">
  <div class="card"><h2>Notas</h2>
    <form id="formNote"><textarea name="text" rows="3" placeholder="Nota" required></textarea><button>Guardar</button></form>
    <p class="muted">En el aparato también se dictan: Atrás dos veces y decí "nota: ...".</p>
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
