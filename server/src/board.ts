// La "app del teléfono": una página web para manejar el aparato desde cualquier
// navegador, con la misma API que usa el aparato. Se entra con el token del
// aparato (queda en localStorage) o, en el servidor con cuentas, con correo y
// contraseña.
//
// La página en sí vive en `public/board/` (index.html, app.js, style.css): tres
// archivos estáticos que se sirven desde acá abajo. Hasta 1.5.68 era un
// template literal de mil líneas adentro de este archivo, y cada comilla mal
// escapada dejaba la página muerta entera.
//
//   GET  /board                      la aplicación (sin token; el JS lo pide)
//   GET  /board/log                  el log que sube el aparato (devicelog.ts)
//   GET  /api/board/state    -> TODO lo que la web necesita para pintar, en una
//                               sola respuesta: recordatorios (pendientes y
//                               hechos), las dos listas enteras, notas, memoria,
//                               feeds, ajustes, lugar y clima, estado del aparato
//                               (última sincronización, versión, último log) y
//                               el consumo del mes.
//   POST /api/board/reminder {id?, title, dueAt: "YYYY-MM-DDTHH:MM"|null, repeat, done?}
//   POST /api/board/item     {list, text} crea · {id, text?, done?} edita o destilda
//   POST /api/board/note     {id?, text}
//   POST /api/board/memory   {id?, text}
//   POST /api/board/feed     {name, url} crea · {id, name} renombra
//   POST /api/board/photo?name=      (la foto tal como sale del teléfono; convierte el servidor)
//   POST /api/board/attachment?trip=&name=   (multipart o cuerpo crudo: PDF del vuelo, del hotel...)
//   GET  /api/board/extra    -> {feeds, memories, settings, lists} (lo usa el firmware viejo; queda)
//   POST /api/board/settings {lang, speak, uiSound, musicVolume, translatorLang}
//   (tildar y borrar: POST /api/hub/done, POST /api/hub/edit)
import { Hono } from "hono";
import { readFile, stat } from "node:fs/promises";
import { join } from "node:path";
import { load, mutate, nextId, resolveList, upsertReminder, refreshTimeZone, repeatText, pendingReminders, normalizeRepeat, localToEpoch, whenLabel, todayLocal, DEFAULT_LISTS, listLabel, DEFAULT_SETTINGS, type Settings } from "./store";
import { logMeta } from "./devicelog";
import { LIMITS, quotasOn, usageOf } from "./usage";
import { multiUser } from "./db";
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
  const res = await mutate(acc, (store) => upsertReminder(store, b));
  if (!res.ok) {
    return res.error === "not_found"
      ? c.json({ ok: false, error: "no existe ese recordatorio" }, 404)
      : c.json({ ok: false, error: "title required" }, 400);
  }
  const r = res.reminder;
  return c.json({ ok: true, reminder: { id: r.id, title: r.title, at: r.dueAt, repeatSpec: r.repeat, repeatText: repeatText(r.repeat, r.dueAt, "es") } });
});

// Sin `id` es un alta en la lista que diga `list`. Con `id` se edita el que
// está: el texto, y `done` en los dos sentidos (destildar es lo que faltaba:
// /api/hub/done solo sabe tildar).
boardApi.post("/item", async (c) => {
  const b = await readBody(c);
  const acc = accountOf(c);
  const id = Math.floor(Number(b.id));
  if (Number.isFinite(id) && id > 0) {
    const found = await mutate(acc, (store) => {
      for (const items of Object.values(store.lists)) {
        const it = items.find((i) => i.id === id);
        if (!it) continue;
        if (typeof b.text === "string" && b.text.trim()) it.text = b.text.trim().slice(0, 200);
        if (typeof b.done === "boolean") it.done = b.done;
        return true;
      }
      return false;
    });
    return found ? c.json({ ok: true }) : c.json({ ok: false, error: "no existe ese ítem" }, 404);
  }
  const text = (b.text ?? "").toString().trim().slice(0, 200);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const list = await mutate(acc, (store) => {
    const l = resolveList(store, b.list);
    store.lists[l].push({ id: nextId(store), text, done: false, dueDate: null, createdAt: new Date().toISOString() });
    return l;
  });
  return c.json({ ok: true, list });
});

// Memoria del asistente: alta y edición a mano (hasta ahora solo entraba por
// voz y solo se podía borrar).
boardApi.post("/memory", async (c) => {
  const b = await readBody(c);
  const text = (b.text ?? "").toString().replace(/\s+/g, " ").trim().slice(0, 300);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const acc = accountOf(c);
  const id = Math.floor(Number(b.id));
  const res = await mutate(acc, (store) => {
    store.memories ??= [];
    if (Number.isFinite(id) && id > 0) {
      const m = store.memories.find((x) => x.id === id);
      if (!m) return null;
      m.text = text;
      return m.id;
    }
    if (store.memories.length >= 60) store.memories.shift();
    const newId = nextId(store);
    store.memories.push({ id: newId, text, createdAt: new Date().toISOString() });
    return newId;
  });
  return res === null ? c.json({ ok: false, error: "no existe" }, 404) : c.json({ ok: true, id: res });
});

boardApi.post("/feed", async (c) => {
  const b = await readBody(c);
  // Con `id` y sin `url` es renombrar el que está.
  const editId = Math.floor(Number(b.id));
  if (Number.isFinite(editId) && editId > 0 && !b.url) {
    const name = (b.name ?? "").toString().trim().slice(0, 40);
    if (!name) return c.json({ ok: false, error: "name required" }, 400);
    const found = await mutate(accountOf(c), (store) => {
      const f = (store.feeds ?? []).find((x) => x.id === editId);
      if (!f) return false;
      f.name = name;
      return true;
    });
    return found ? c.json({ ok: true }) : c.json({ ok: false, error: "no está" }, 404);
  }
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
  const name =
    (b.name ?? "").toString().trim().slice(0, 40) ||
    probe.title ||
    new URL(probe.url).hostname.replace(/^www\./, "");
  // El "ya está cargado" se comprueba DENTRO del candado: dos altas del mismo
  // feed a la vez entraban las dos.
  const dup = await mutate(acc, (store) => {
    store.feeds ??= [];
    if (store.feeds.some((f) => f.url === probe.url)) return true;
    store.feeds.push({ id: nextId(store), name, url: probe.url });
    return false;
  });
  if (dup) return c.json({ ok: false, error: "ese feed ya está cargado" }, 400);
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
  const s = await mutate(acc, (store) => {
    const next: Settings = { ...DEFAULT_SETTINGS, ...(store.settings ?? {}) };
    if (typeof b.lang === "string" && /^[a-z]{2}$/.test(b.lang)) next.lang = b.lang;
    if (b.speak === "none" || b.speak === "short" || b.speak === "all") next.speak = b.speak;
    if (Number.isFinite(Number(b.musicVolume))) next.musicVolume = Math.max(0, Math.min(100, Math.round(Number(b.musicVolume))));
    if (typeof b.translatorLang === "string" && /^[a-z]{2}$/.test(b.translatorLang)) next.translatorLang = b.translatorLang;
    if (b.uiSound === "off" || b.uiSound === "soft" || b.uiSound === "normal") next.uiSound = b.uiSound;
    next.rev = (next.rev ?? 0) + 1;
    store.settings = next;
    return next;
  });
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
  const editId = Math.floor(Number(b.id));
  const id = await mutate(acc, (store) => {
    if (Number.isFinite(editId) && editId > 0) {
      const n = store.notes.find((x) => x.id === editId);
      if (!n) return null;
      n.text = text;
      return n.id;
    }
    const newId = nextId(store);
    store.notes.push({ id: newId, text, createdAt: new Date().toISOString() });
    return newId;
  });
  return id === null ? c.json({ ok: false, error: "no existe esa nota" }, 404) : c.json({ ok: true, id });
});

// ── Todo lo que la web necesita, en una sola respuesta ──────────────────────
// La página vieja armaba cada pestaña con dos o tres pedidos distintos y, al
// guardar en una, las otras quedaban con lo de antes. Acá va TODO el estado de
// la cuenta junto; la web lo vuelve a pedir entero después de cada cambio, así
// nunca hay una parte vieja y una nueva en la misma pantalla.
boardApi.get("/state", async (c) => {
  const acc = accountOf(c);
  await refreshTimeZone(acc);
  const [store, diag, meta, usage] = await Promise.all([load(acc), hubDiagnostics(acc), logMeta(acc), usageOf(acc)]);
  const rem = (r: (typeof store.reminders)[number]) => ({
    id: r.id,
    title: r.title,
    at: r.dueAt,
    dueAt: localToEpoch(r.dueAt),
    when: whenLabel(r.dueAt, "es"),
    repeatSpec: normalizeRepeat(r.repeat),
    repeatText: repeatText(r.repeat, r.dueAt, "es"),
    done: r.done,
    createdAt: r.createdAt,
  });
  return c.json({
    ok: true,
    now: Math.floor(Date.now() / 1000),
    today: todayLocal(),   // en la zona del lugar elegido, que es la del aparato
    reminders: pendingReminders(store).map(rem),
    doneReminders: store.reminders.filter((r) => r.done).slice(-30).reverse().map(rem),
    lists: DEFAULT_LISTS.map((key) => ({
      key,
      name: listLabel(key, "es"),
      items: (store.lists[key] ?? []).map((i) => ({ id: i.id, text: i.text, done: i.done, dueDate: i.dueDate, createdAt: i.createdAt })),
    })),
    notes: store.notes.slice().reverse(),
    memories: store.memories ?? [],
    feeds: store.feeds ?? [],
    settings: { ...DEFAULT_SETTINGS, ...(store.settings ?? {}) },
    place: diag.place,
    weather: diag.weather,
    device: {
      lastFetch: diag.lastDeviceFetch,   // ms; 0 = desde que arrancó el servidor no sincronizó
      logAt: meta.at,                    // ISO de la última subida del log, o ""
      logBytes: meta.bytes,
      firmware: meta.firmware,           // "1.5.68-ws397" del último arranque que se vio en el log
      wake: meta.wake,                   // por qué arrancó la última vez
    },
    usage: { ...usage, limits: LIMITS, quotasOn },
    multi: multiUser,
  });
});

// ── La página ───────────────────────────────────────────────────────────────
// Tres archivos estáticos de public/board/. El HTML se sirve sin caché (así un
// deploy se ve al recargar) y el JS y el CSS con la marca de tiempo del archivo
// en la URL (?v=), que es lo que el HTML referencia.
const PUBLIC_DIR = join(import.meta.dir, "..", "public", "board");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".webmanifest": "application/manifest+json",
};

async function serveFile(name: string, cache: string): Promise<Response> {
  const file = join(PUBLIC_DIR, name);
  try {
    const bytes = await readFile(file);
    const ext = name.slice(name.lastIndexOf("."));
    return new Response(bytes, {
      headers: { "Content-Type": MIME[ext] ?? "application/octet-stream", "Cache-Control": cache, "Content-Length": String(bytes.length) },
    });
  } catch {
    return new Response("no está la página (falta public/board en el deploy)", { status: 404 });
  }
}

// La marca de versión de los estáticos: el mtime más nuevo de los tres.
async function assetsVersion(): Promise<string> {
  let newest = 0;
  for (const f of ["index.html", "app.js", "style.css"]) {
    const st = await stat(join(PUBLIC_DIR, f)).catch(() => null);
    if (st && st.mtimeMs > newest) newest = st.mtimeMs;
  }
  return Math.floor(newest / 1000).toString(36);
}

export const board = new Hono();

board.get("/", async (c) => {
  const v = await assetsVersion();
  const res = await serveFile("index.html", "no-cache");
  if (res.status !== 200) return res;
  const html = (await res.text()).replaceAll("__V__", v);
  return c.html(html, 200, { "Cache-Control": "no-cache" });
});
board.get("/app.js", () => serveFile("app.js", "public, max-age=31536000, immutable"));
board.get("/style.css", () => serveFile("style.css", "public, max-age=31536000, immutable"));
board.get("/icon.svg", () => serveFile("icon.svg", "public, max-age=86400"));
board.get("/manifest.webmanifest", () => serveFile("manifest.webmanifest", "no-cache"));
