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
import { savePhoto, MAX_PHOTO_BYTES } from "./photos";

export const boardApi = new Hono();

boardApi.post("/message", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 300);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.messages.push({ id: nextId(store), from: (b.from ?? "").toString().trim().slice(0, 40) || "web", text, createdAt: new Date().toISOString(), read: false });
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/reminder", async (c) => {
  const b = await c.req.json().catch(() => ({}));
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
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 200);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  const list = resolveList(store, b.list, true);
  store.lists[list].push({ id: nextId(store), text, done: false, dueDate: null, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true, list });
});

boardApi.post("/list", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const name = (b.name ?? "").toString().trim().slice(0, 40);
  if (!name) return c.json({ ok: false, error: "name required" }, 400);
  const store = await load();
  const list = resolveList(store, name, true);
  await save(store);
  return c.json({ ok: true, list });
});

boardApi.post("/list/delete", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const name = (b.name ?? "").toString();
  const store = await load();
  if (!(name in store.lists)) return c.json({ ok: false, error: "not found" }, 404);
  delete store.lists[name];
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/feed", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const url = (b.url ?? "").toString().trim().slice(0, 500);
  if (!/^https?:\/\//.test(url)) return c.json({ ok: false, error: "url required" }, 400);
  const store = await load();
  store.feeds ??= [];
  const name = (b.name ?? "").toString().trim().slice(0, 40) || new URL(url).hostname.replace(/^www\./, "");
  store.feeds.push({ id: nextId(store), name, url });
  await save(store);
  return c.json({ ok: true });
});

// El BMP ya viene convertido por el navegador (ver la página): 2 bpp, 4 grises.
boardApi.post("/photo", async (c) => {
  const name = (c.req.query("name") ?? "foto").toString().slice(0, 80);
  const bytes = new Uint8Array(await c.req.arrayBuffer());
  if (bytes.byteLength < 100 || bytes.byteLength > MAX_PHOTO_BYTES) return c.json({ ok: false, error: "bad size" }, 400);
  if (bytes[0] !== 0x42 || bytes[1] !== 0x4d) return c.json({ ok: false, error: "not a BMP" }, 400);
  const id = await savePhoto(name, bytes);
  console.log(`photo ${id}: ${name} (${bytes.byteLength} bytes)`);
  return c.json({ ok: true, id });
});

boardApi.get("/extra", async (c) => {
  const store = await load();
  return c.json({
    ok: true,
    feeds: store.feeds ?? [],
    memories: store.memories ?? [],
    settings: store.settings ?? DEFAULT_SETTINGS,
    lists: Object.keys(store.lists),
  });
});

// Ajustes del aparato. Cada cambio sube `rev`; el aparato los aplica en la
// próxima sincronización solo si la revisión es mayor a la que ya tenía.
boardApi.post("/settings", async (c) => {
  const b = await c.req.json().catch(() => ({}));
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

boardApi.post("/note", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 2000);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.notes.push({ id: nextId(store), text, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true });
});

const STYLE = `
:root{color-scheme:light dark}
body{font-family:system-ui,sans-serif;margin:0;background:#f4f4f4;color:#111}
header{background:#111;color:#fff;padding:12px 16px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:5}
header a{color:#fff}
main{max-width:720px;margin:0 auto;padding:12px 12px 40px}
section{background:#fff;border-radius:12px;padding:12px 14px;margin:12px 0;box-shadow:0 1px 3px #0002}
h2{margin:0 0 8px;font-size:17px}
h3{margin:12px 0 0;font-size:15px}
form{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px;align-items:center}
input,select,textarea,button{font:inherit;padding:8px 10px;border:1px solid #bbb;border-radius:8px;background:#fff;color:#111}
input[type=text],textarea{flex:1;min-width:150px}
input[type=range]{flex:1;min-width:150px;padding:0}
button{background:#111;color:#fff;border:none;cursor:pointer}
button.ghost{background:#eee;color:#111;padding:6px 10px}
button.link{background:none;color:#06c;padding:4px 6px}
ul{list-style:none;margin:0;padding:0}
li{display:flex;gap:8px;align-items:center;padding:8px 0;border-top:1px solid #eee}
li span{flex:1;word-break:break-word}
li small{color:#666}
label{font-size:14px;color:#444;min-width:120px}
.row{display:flex;gap:8px;align-items:center;margin:8px 0;flex-wrap:wrap}
.muted{color:#666;font-size:14px}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:16px;background:#111;color:#fff;padding:10px 16px;border-radius:20px;opacity:0;transition:opacity .2s;pointer-events:none}
.toast.on{opacity:1}
#gate{display:none;max-width:420px;margin:60px auto;text-align:center}
`;

// Ojo al editar: esto vive dentro de un template literal, así que un
// backslash antes de una comilla se lo come el literal y rompe el script
// entero. Regla: nada de \\' ni backticks acá adentro; las cadenas van con
// comillas dobles y los atributos HTML con comillas simples. Los botones no
// llevan onclick: se manejan por delegación con data-act.
const SCRIPT = `
let token = localStorage.getItem("deviceToken") || "";
let settings = null;
const $ = (id) => document.getElementById(id);
function esc(s){ return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\\u0022":"&quot;","\\u0027":"&#39;"}[c])); }
function toast(msg){ const t = $("toast"); t.textContent = msg; t.classList.add("on"); setTimeout(() => t.classList.remove("on"), 1800); }

async function api(path, body, method){
  const r = await fetch(path, {
    method: method || (body ? "POST" : "GET"),
    headers: { "Authorization": "Bearer " + token, "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (r.status === 401) { gate("Token rechazado"); throw new Error("token"); }
  if (!r.ok) throw new Error("http " + r.status);
  return r.json();
}

function gate(msg){
  $("gate").style.display = "block";
  $("app").style.display = "none";
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
  refresh().catch(() => gate("No se pudo conectar"));
}

function btn(label, act, extra){
  let attrs = "";
  for (const k in extra) attrs += " data-" + k + "='" + esc(extra[k]) + "'";
  return "<button class='ghost' data-act='" + act + "'" + attrs + ">" + label + "</button>";
}

function row(text, small, buttons){
  return "<li><span>" + esc(text) + (small ? " <small>" + esc(small) + "</small>" : "") + "</span>" + buttons + "</li>";
}

function empty(text){ return "<li class='muted'>" + esc(text) + "</li>"; }

async function refresh(){
  const d = await api("/api/hub?lang=es");
  const x = await api("/api/board/extra");
  settings = x.settings;

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
    return "<h3>" + esc(n) + " <small class='muted'>" + items.length + "</small>" +
      btn("Borrar lista", "dellist", { name: n }) + "</h3><ul>" +
      (items.map((i) => row(i.text, "",
        btn("Hecho", "done", { kind: "item", id: i.id }) +
        btn("Borrar", "del", { kind: "item", id: i.id }))).join("") || empty("Vacía")) + "</ul>";
  }).join("");

  $("notes").innerHTML = d.notes.map((n) =>
    row(n.text, "", btn("Borrar", "del", { kind: "note", id: n.id }))).join("") || empty("Sin notas");

  $("feeds").innerHTML = x.feeds.map((f) =>
    row(f.name, f.url, btn("Borrar", "del", { kind: "feed", id: f.id }))).join("") || empty("Sin feeds");

  $("memories").innerHTML = x.memories.map((m) =>
    row(m.text, "", btn("Borrar", "del", { kind: "memory", id: m.id }))).join("") || empty("Nada guardado");

  const ph = await api("/api/photos");
  $("photos").innerHTML = ph.photos.map((p) =>
    row(p.name, Math.round(p.size / 1024) + " KB", btn("Borrar", "delphoto", { id: p.id }))).join("") || empty("Sin fotos");

  $("setLang").value = settings.lang;
  $("setSpeak").value = settings.speak;
  $("setTranslator").value = settings.translatorLang;
  $("setVolume").value = settings.musicVolume;
  $("volumeOut").textContent = settings.musicVolume + " %";

  const w = d.weather && d.weather.line ? d.weather.line + " · " + (d.weather.detail || "") : "";
  const loc = await api("/api/hub/location");
  $("place").textContent = loc.place ? (loc.place.label || loc.place.name || (loc.place.lat + ", " + loc.place.lon)) + (w ? " — " + w : "") : "Sin lugar configurado: el clima queda vacío";
}

// Un solo manejador para todos los botones de las listas.
document.addEventListener("click", async (ev) => {
  const b = ev.target.closest("button[data-act]");
  if (!b) return;
  const act = b.dataset.act;
  try {
    if (act === "done") await api("/api/hub/done", { kind: b.dataset.kind, id: Number(b.dataset.id) });
    else if (act === "del") await api("/api/hub/edit", { kind: b.dataset.kind, id: Number(b.dataset.id), action: "delete" });
    else if (act === "delphoto") await api("/api/photos/delete", { id: b.dataset.id });
    else if (act === "dellist") {
      if (!confirm("¿Borrar la lista " + b.dataset.name + " con todo lo que tenga?")) return;
      await api("/api/board/list/delete", { name: b.dataset.name });
    } else if (act === "place") {
      await api("/api/hub/location", JSON.parse(b.dataset.place));
      $("placeResults").innerHTML = "";
      toast("Lugar guardado");
    } else return;
    await refresh();
  } catch (e) { toast("No se pudo"); }
});

async function post(path, body){
  try { await api(path, body); await refresh(); toast("Listo"); return true; }
  catch (e) { toast("No se pudo guardar"); return false; }
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

// El aparato muestra 800x480 en 4 grises: convertimos acá (canvas, difuminado
// Floyd-Steinberg) y mandamos un BMP de 2 bpp ya listo, sin trabajo en el servidor.
async function sendPhoto(){
  const input = $("photoInput");
  const file = input.files && input.files[0];
  if (!file) { toast("Elegí una foto"); return; }
  const st = $("photoStatus");
  st.textContent = "Convirtiendo...";
  try {
    const img = new Image();
    img.src = URL.createObjectURL(file);
    await img.decode();
    const W = 800, H = 480;
    const cv = document.createElement("canvas");
    cv.width = W; cv.height = H;
    const ctx = cv.getContext("2d");
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, W, H);
    const s = Math.min(W / img.width, H / img.height);
    const dw = Math.round(img.width * s), dh = Math.round(img.height * s);
    ctx.drawImage(img, Math.round((W - dw) / 2), Math.round((H - dh) / 2), dw, dh);
    const px = ctx.getImageData(0, 0, W, H).data;
    const gray = new Float32Array(W * H);
    for (let i = 0, j = 0; i < px.length; i += 4, j++) gray[j] = 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2];
    const idx = new Uint8Array(W * H);
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const p = y * W + x;
        const old = gray[p];
        const q = Math.max(0, Math.min(3, Math.round(old / 85)));
        idx[p] = q;
        const err = old - q * 85;
        if (x + 1 < W) gray[p + 1] += err * 7 / 16;
        if (y + 1 < H) {
          if (x > 0) gray[p + W - 1] += err * 3 / 16;
          gray[p + W] += err * 5 / 16;
          if (x + 1 < W) gray[p + W + 1] += err * 1 / 16;
        }
      }
    }
    const rowBytes = Math.ceil(W * 2 / 32) * 4;   // 2 bpp, filas alineadas a 4
    const pixels = rowBytes * H;
    const off = 14 + 40 + 4 * 4;                  // cabeceras + paleta de 4 colores
    const buf = new Uint8Array(off + pixels);
    const dv = new DataView(buf.buffer);
    buf[0] = 0x42; buf[1] = 0x4d;
    dv.setUint32(2, buf.length, true); dv.setUint32(10, off, true);
    dv.setUint32(14, 40, true); dv.setInt32(18, W, true); dv.setInt32(22, H, true);
    dv.setUint16(26, 1, true); dv.setUint16(28, 2, true); dv.setUint32(34, pixels, true);
    dv.setUint32(46, 4, true); dv.setUint32(50, 4, true);
    const levels = [0, 85, 170, 255];
    for (let i = 0; i < 4; i++) { const o = 54 + i * 4; buf[o] = levels[i]; buf[o + 1] = levels[i]; buf[o + 2] = levels[i]; buf[o + 3] = 0; }
    for (let y = 0; y < H; y++) {
      const r0 = off + (H - 1 - y) * rowBytes;    // BMP: de abajo hacia arriba
      for (let x = 0; x < W; x++) buf[r0 + (x >> 2)] |= idx[y * W + x] << (6 - 2 * (x % 4));
    }
    st.textContent = "Subiendo " + Math.round(buf.length / 1024) + " KB...";
    const r = await fetch("/api/board/photo?name=" + encodeURIComponent(file.name), {
      method: "POST",
      headers: { "Authorization": "Bearer " + token, "Content-Type": "image/bmp" },
      body: buf,
    });
    st.textContent = r.ok ? "Foto subida" : "No se pudo subir (" + r.status + ")";
    input.value = "";
    await refresh();
  } catch (e) {
    st.textContent = "No se pudo convertir la foto";
  }
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
  wire("formFeed", "/api/board/feed", (f) => ({ name: f.name.value, url: f.url.value }));
  $("formPlace").addEventListener("submit", searchPlace);
  $("photoSend").addEventListener("click", sendPhoto);
  $("tokenSave").addEventListener("click", saveToken);
  $("tokenBtn").addEventListener("click", () => gate("Cambiá el token del aparato"));
  $("setVolume").addEventListener("input", () => { $("volumeOut").textContent = $("setVolume").value + " %"; });
  $("settingsSave").addEventListener("click", async () => {
    await post("/api/board/settings", {
      lang: $("setLang").value,
      speak: $("setSpeak").value,
      musicVolume: Number($("setVolume").value),
      translatorLang: $("setTranslator").value,
    });
  });
  if (!token) { gate("Está en la web UI del aparato → Servidor"); return; }
  refresh().catch(() => gate("No se pudo conectar: revisá el token"));
}
start();
`;

const PAGE = `<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pizarra</title>
<style>${STYLE}</style></head><body>
<header><strong>Pizarra del aparato</strong><span><a href="/board/log" style="margin-right:12px">Log</a><button class="ghost" id="tokenBtn">Token</button></span></header>

<div id="gate">
  <h2>Token del aparato</h2>
  <p class="muted" id="gateMsg"></p>
  <p><input type="text" id="tokenInput" placeholder="Token" style="width:80%"></p>
  <p><button id="tokenSave">Entrar</button></p>
</div>

<main id="app" style="display:none">
<section><h2>Mensaje para el hub</h2>
  <form id="formMessage">
    <input type="text" name="from" placeholder="De" style="max-width:110px">
    <input type="text" name="text" placeholder="Mensaje" required><button>Dejar</button>
  </form>
  <ul id="messages"></ul>
</section>

<section><h2>Recordatorios</h2>
  <form id="formReminder">
    <input type="text" name="title" placeholder="Qué" required>
    <input type="date" name="date"><input type="time" name="time">
    <select name="repeat">
      <option value="none">Una vez</option><option value="daily">Diario</option>
      <option value="weekly">Semanal</option><option value="monthly">Mensual</option>
    </select><button>Guardar</button>
  </form>
  <ul id="reminders"></ul>
</section>

<section><h2>Listas</h2>
  <form id="formItem"><select id="listSelect"></select><input type="text" name="text" placeholder="Ítem" required><button>Agregar</button></form>
  <form id="formList"><input type="text" name="name" placeholder="Lista nueva" required><button>Crear lista</button></form>
  <div id="listItems"></div>
</section>

<section><h2>Notas</h2>
  <form id="formNote"><textarea name="text" rows="2" placeholder="Nota" required></textarea><button>Guardar</button></form>
  <ul id="notes"></ul>
</section>

<section><h2>Fotos</h2>
  <div class="row"><input type="file" id="photoInput" accept="image/*"><button type="button" id="photoSend">Subir</button></div>
  <p class="muted" id="photoStatus"></p>
  <ul id="photos"></ul>
</section>

<section><h2>Noticias (RSS)</h2>
  <form id="formFeed"><input type="text" name="name" placeholder="Nombre" style="max-width:130px"><input type="text" name="url" placeholder="https://.../rss" required><button>Agregar</button></form>
  <ul id="feeds"></ul>
</section>

<section><h2>Memoria del asistente</h2><ul id="memories"></ul></section>

<section><h2>Ajustes</h2>
  <p class="muted">Lugar del clima: <b id="place">—</b></p>
  <form id="formPlace"><input type="text" id="placeQuery" placeholder="Ciudad" required><button>Buscar</button></form>
  <ul id="placeResults"></ul>
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
  <div class="row"><label for="setVolume">Volumen música</label><input type="range" id="setVolume" min="0" max="100" step="5"><span id="volumeOut" class="muted"></span></div>
  <div class="row"><button id="settingsSave">Guardar ajustes</button></div>
  <p class="muted">Los ajustes viajan al aparato en la próxima sincronización del hub (Atrás 1,2 s en el hub o Ajustes → Sincronizar hub).</p>
</section>

<p class="muted">Lo que cargues acá aparece en el aparato en la próxima sincronización.</p>
</main>
<div class="toast" id="toast"></div>
<script>${SCRIPT}</script></body></html>`;

export const board = new Hono();
board.get("/", (c) => c.html(PAGE));
