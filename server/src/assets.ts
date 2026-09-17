// Paquete de contenido descargable: lo pesado (hoy la Biblia y las apps de Lua
// de fábrica) vive acá y el aparato se lo baja de una sola vez después de
// actualizar el firmware, en vez de tener un botón distinto por cada cosa.
//
// Idea: el aparato pide el manifiesto, lo compara con lo que ya tiene en la SD
// (mismo `path`, mismo `sha`) y baja SOLO lo que falta o cambió. Cada archivo se
// baja con `Range`, así que una descarga cortada se sigue después sin empezar de
// cero.
//
//   GET /api/assets/manifest?lang=xx[&kind=bible,apps][&from=0&limit=400]
//     -> { ok, version, lang, building, progress:{done,total}, total, from, count,
//          bytes, items: [{ id, kind, path, bytes, sha }] }
//        `version` es el resumen de TODO el paquete de ese idioma: si no cambió,
//        no hace falta mirar nada más.
//        `sha` = primeros 16 hex del sha256 del archivo (64 bits alcanzan y el
//        manifiesto entero entra en la RAM del aparato).
//        `path` = dónde va en la SD, tal cual.
//
//   GET /api/assets/file?id=<id>
//     -> el archivo. `Content-Length`, `ETag: "<sha>"`, `Accept-Ranges: bytes`.
//        Con `Range: bytes=N-` devuelve 206 y `Content-Range`, para reanudar.
//
//   GET /api/assets/status   -> cómo va la generación (para /board)
//   POST /api/assets/build   -> forzar la generación (idempotente)
//
// Kinds: `bible` (66 archivos, uno por libro) y `apps` (los .lua de fábrica).
//
// LAS TARJETAS DE BEBÉ SE BORRARON EN 1.5.91. Salieron del aparato en 1.5.63
// ("tarjetas afuera, se va, luego lo hacemos en LUA") y desde entonces quedaron
// acá 317 líneas de catálogo más el dibujante de emojis de Noto, apagados por
// una constante. `kind` sigue aceptando "cards" y "sounds" por una sola razón:
// el índice vive en el VOLUMEN, así que un servidor que ya las había armado las
// anuncia hasta que `loadIndex()` las pode.
//
// Todo se genera solo (al arrancar el servidor y al pedir el manifiesto) y queda
// cacheado en el volumen: no hay ningún paso a mano.
import { Hono } from "hono";
import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile, rename } from "node:fs/promises";
import { normalizeLang, LANGS, type Lang } from "./lang";
import { bookText, bookCount } from "./bible";
import { synthesize } from "./tts";
import { readJsonSafe, writeJsonAtomic } from "./fsjson";

const DIR = process.env.ASSETS_DIR ?? "/data/assets";
const FACTORY_APPS_DIR = process.env.FACTORY_APPS_DIR ?? `${import.meta.dir}/../../examples/Apps`;
const BUILD_ON_START = process.env.ASSETS_BUILD !== "0";
// Los dibujos salen de Noto Color Emoji (googlefonts/noto-emoji): Apache 2.0 +
// OFL, o sea que NO obligan a atribuir a nadie, y son ilustraciones llenas, no
// iconos de trazo (un bebé no reconoce un contorno). El SVG de cada emoji se
// baja una sola vez y queda guardado en el volumen.

// Los cuatro grises del panel (los mismos que usa toDeviceBmp en deviceBmp.ts).
// Hasta dónde se aclara la figura. Un emoji amarillo (la luna, la estrella, la
// banana) en gris queda casi blanco y DESAPARECE contra el fondo, así que el
// tono de cada dibujo se estira a [0, 190]: el más claro de la figura cae en el
// gris 170 y nunca en el blanco del fondo. Comparado mirando las tres opciones
// (tal cual / escala fija / normalizado): normalizado es el único que deja
// legibles la luna y el vaso de leche.
// Marca de formato del dibujo. Al cambiarla, las tarjetas viejas del volumen se
// descartan solas y se regeneran (ver loadIndex).
const ART_REV = "sin-tarjetas-v1";

export type AssetKind = "bible" | "apps" | "cards" | "sounds" | "icons";
// `tag` es de qué se generó el archivo (el dibujo, o la palabra que dice el
// clip): si cambia, esa entrada sola se rehace. Sin eso, cambiar UNA palabra
// obligaba a regenerar los 480 clips de Piper (minutos) o dejaba el audio viejo
// diciendo "remera" para siempre.
export type AssetEntry = { id: string; kind: AssetKind; path: string; bytes: number; sha: string; tag?: string };

// El índice se guarda en el volumen: un reinicio del contenedor no tiene que
// volver a generar 240 dibujos y 480 clips de voz.
type Index = { art: string; entries: Record<string, AssetEntry> };
const indexFile = (lang: Lang) => `${DIR}/index-${lang}.json`;
const indexes = new Map<Lang, Index>();

// Lo que tendría que decir el `tag` de cada id con el catálogo de hoy. Lo que no
// coincida se descarta del índice y se genera de nuevo.
function expectedTags(): Map<string, string> {
  const out = new Map<string, string>();
  for (const name of ["reloj", "ahorcado", "tresenraya"]) out.set(`apps/${name}`, `factory/${name}/1`);
  return out;
}

// Las tarjetas se borraron en 1.5.91, pero el índice vive en el VOLUMEN: un
// servidor que ya las había armado las sigue anunciando hasta que se poden. Se
// reconocen por ID y no por `kind`, porque los DOS audios de cada tarjeta viven
// bajo `sounds/<lang>/<id>` con kind "sounds": filtrar por `kind !== "cards"`
// —como hacía la poda de 1.5.86— sacaba los 240 BMP y dejaba los 480 audios
// anunciados para siempre. La mitad de un arreglo es peor que ninguno, porque
// parece hecho.
function isCardEntry(id: string, kind: string): boolean {
  return kind === "cards" || kind === "sounds" || id === "cards/index" || id.startsWith("cards/");
}

async function loadIndex(lang: Lang): Promise<Index> {
  const have = indexes.get(lang);
  if (have) return have;
  const idx = await readJsonSafe<Index>(indexFile(lang), { art: ART_REV, entries: {} });
  idx.entries ??= {};
  // Las tarjetas se sacaron del producto en 1.5.65, pero poner `CARDS_IN_PACK`
  // en false sólo apagó la GENERACIÓN: el manifiesto se sirve desde este índice
  // persistido, así que un servidor que ya las había armado siguió
  // anunciándolas para siempre. En el aparato del usuario eso eran 787 archivos
  // en el manifiesto —240 BMP y 480 audios de tarjetas, más la Biblia— que se
  // bajan, ocupan la tarjeta y no los abre nadie, porque `CardsActivity` ya no
  // existe. Se podan acá, que es por donde pasan el manifiesto, el estado y la
  // versión.
  {
    const antes = Object.keys(idx.entries).length;
    idx.entries = Object.fromEntries(Object.entries(idx.entries).filter(([id, e]) => !isCardEntry(id, e.kind)));
    const sacadas = antes - Object.keys(idx.entries).length;
    if (sacadas > 0) console.log(`assets: ${sacadas} entradas de tarjetas podadas del índice (${lang})`);
  }
  // Cambió el formato del dibujo (o de dónde salen): las tarjetas viejas no
  // sirven más. La Biblia y los audios no se tocan.
  if (idx.art !== ART_REV) {
    idx.entries = Object.fromEntries(Object.entries(idx.entries).filter(([, e]) => e.kind !== "cards"));
    idx.art = ART_REV;
  }
  // Cambió una palabra o el dibujo de una tarjeta: se cae solo lo que cambió.
  const want = expectedTags();
  idx.entries = Object.fromEntries(
    Object.entries(idx.entries).filter(([id, e]) => {
      if (!want.has(id)) return e.kind === "bible";  // ids viejos que ya no existen
      return e.tag === want.get(id);
    }),
  );
  indexes.set(lang, idx);
  return idx;
}

let saveTimer: ReturnType<typeof setTimeout> | null = null;
function saveIndexSoon(lang: Lang): void {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    const idx = indexes.get(lang);
    if (idx) void writeJsonAtomic(indexFile(lang), idx).catch((err) => console.error("assets: no se pudo guardar el índice", err));
  }, 2_000);
}

function sha16(data: Uint8Array): string {
  return createHash("sha256").update(data).digest("hex").slice(0, 16);
}

// Escritura atómica: si el contenedor se cae generando, no queda medio archivo
// con el sha del entero.
async function writeAsset(file: string, data: Uint8Array): Promise<void> {
  await mkdir(file.slice(0, file.lastIndexOf("/")), { recursive: true });
  await writeFile(`${file}.tmp`, data);
  await rename(`${file}.tmp`, file);
}









// ── Rutas en la SD ──────────────────────────────────────────────────────────
// La Biblia mantiene la ruta que el aparato ya usa, así lo que esté bajado sigue
// sirviendo y el botón de "bajar la Biblia" se puede sacar sin migrar nada.
const biblePath = (lang: Lang, i: number) => `/.crosspoint/bible/${lang}/b${String(i).padStart(2, "0")}.txt`;
// Estas tres son las que espera CardsActivity del firmware; no cambiarlas sin
// cambiarlas allá.


// Dónde vive el archivo del lado del servidor: el mismo árbol que en la SD,
// colgando de ASSETS_DIR. Así el id no tiene que codificar nada.
function localPath(entry: { path: string }): string {
  return `${DIR}${entry.path.replace(/^\/\.crosspoint/, "")}`;
}

// El índice que lee CardsActivity: la palabra en los dos idiomas, la categoría
// y dónde están el dibujo y los dos audios (relativos a /.crosspoint, como los
// arma el firmware por su cuenta si faltan).


// ── Generación ──────────────────────────────────────────────────────────────
// Todo lo que hay que tener. El id lleva el idioma adentro para que un mismo
// manifiesto no pueda mezclar el audio de dos idiomas.
type Planned = { id: string; kind: AssetKind; path: string; tag?: string; make: () => Promise<Uint8Array | null> };

async function plan(lang: Lang): Promise<Planned[]> {
  const out: Planned[] = [];
  const books = await bookCount(lang);
  for (let i = 0; i < books; i++) {
    out.push({
      id: `bible/${lang}/b${String(i).padStart(2, "0")}`,
      kind: "bible",
      path: biblePath(lang, i),
      make: async () => {
        const t = await bookText(lang, i);
        return t ? new TextEncoder().encode(t) : null;
      },
    });
  }
  for (const name of ["reloj", "ahorcado", "tresenraya"]) {
    out.push({
      id: `apps/${name}`,
      kind: "apps",
      path: `/Apps/${name}.lua`,
      tag: `factory/${name}/1`,
      make: async () => new Uint8Array(await readFile(`${FACTORY_APPS_DIR}/${name}.lua`)),
    });
  }
  return out;
}

type Progress = { lang: Lang; done: number; total: number; building: boolean; error: string; startedAt: number; finishedAt: number; missing: number };
const progress = new Map<Lang, Progress>();
const running = new Map<Lang, Promise<void>>();
// No se vuelve a revisar en cada pedido del manifiesto: repasar 786 archivos es
// barato pero no gratis, y si Piper está apagado siempre va a faltar el audio.
const RECHECK_MS = 10 * 60 * 1000;

export function assetProgress(lang: Lang): Progress {
  return progress.get(lang) ?? { lang, done: 0, total: 0, building: false, error: "", startedAt: 0, finishedAt: 0, missing: -1 };
}

// ¿Vale la pena volver a mirar si falta algo?
function shouldRebuild(lang: Lang): boolean {
  const p = assetProgress(lang);
  if (p.building) return false;
  if (!p.finishedAt) return true;                       // nunca corrió
  if (p.missing === 0) return false;                    // está completo
  return Date.now() - p.finishedAt > RECHECK_MS;
}

// Genera lo que falte. Idempotente y una sola corrida por idioma a la vez: dos
// pedidos juntos no pueden ponerse a sintetizar lo mismo dos veces.
export function buildAssets(lang: Lang): Promise<void> {
  const have = running.get(lang);
  if (have) return have;
  const job = (async () => {
    const idx = await loadIndex(lang);
    const items = await plan(lang);
    const p: Progress = { lang, done: 0, total: items.length, building: true, error: "", startedAt: Date.now(), finishedAt: 0, missing: items.length };
    progress.set(lang, p);
    for (const item of items) {
      const known = idx.entries[item.id];
      const file = localPath(item);
      // Ya está y el archivo sigue en el volumen: no se toca.
      if (known && known.sha) {
        const ok = await stat(file).then((s) => s.size === known.bytes, () => false);
        if (ok) {
          p.done++;
          continue;
        }
      }
      try {
        const data = await item.make();
        if (!data || !data.length) {
          // Piper apagado o sin voz para ese idioma: no es un error fatal, el
          // paquete sale sin audio y el resto se baja igual.
          p.done++;
          continue;
        }
        await writeAsset(file, data);
        idx.entries[item.id] = { id: item.id, kind: item.kind, path: item.path, bytes: data.length, sha: sha16(data), ...(item.tag ? { tag: item.tag } : {}) };
        saveIndexSoon(lang);
      } catch (err) {
        p.error = `${item.id}: ${String(err instanceof Error ? err.message : err).slice(0, 120)}`;
        console.error("assets:", p.error);
      }
      p.done++;
      // Se le suelta el turno al resto del servidor: mientras esto corre, el
      // usuario puede estar hablándole al aparato y Piper es uno solo.
      await new Promise((r) => setTimeout(r, 5));
    }
    p.building = false;
    p.finishedAt = Date.now();
    p.missing = items.filter((i) => !idx.entries[i.id]).length;
    const idx2 = indexes.get(lang);
    if (idx2) await writeJsonAtomic(indexFile(lang), idx2);
    console.log(`assets ${lang}: ${Object.keys(idx.entries).length} archivos, ${(totalBytes(idx) / 1e6).toFixed(1)} MB`);
  })().finally(() => running.delete(lang));
  running.set(lang, job);
  return job;
}

function totalBytes(idx: Index): number {
  return Object.values(idx.entries).reduce((a, e) => a + e.bytes, 0);
}

// El resumen de todo el paquete: si no cambió, el aparato no tiene que comparar
// archivo por archivo.
function versionOf(entries: AssetEntry[]): string {
  const h = createHash("sha256");
  for (const e of [...entries].sort((a, b) => (a.id < b.id ? -1 : 1))) h.update(`${e.id}:${e.sha};`);
  return h.digest("hex").slice(0, 16);
}

// Un id puede estar en el índice de otro idioma: las tarjetas y sus audios (que
// siempre son es + en) aparecen en el manifiesto de los seis, pero el archivo se
// generó una sola vez. Se busca primero en el idioma del pedido y después en los
// índices que existan en el volumen.
async function findEntry(id: string, prefer: Lang): Promise<AssetEntry | null> {
  const order: Lang[] = [prefer, ...LANGS.filter((l) => l !== prefer)];
  for (const lang of order) {
    if (!indexes.has(lang)) {
      const exists = await stat(indexFile(lang)).then(() => true, () => false);
      if (!exists) continue;
    }
    const idx = await loadIndex(lang);
    const e = idx.entries[id];
    if (e) return e;
  }
  return null;
}

export const assets = new Hono();

assets.get("/manifest", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const idx = await loadIndex(lang);
  // Arranca la generación si falta algo, pero NO se espera: el aparato se lleva
  // lo que ya está y vuelve por el resto.
  if (shouldRebuild(lang)) void buildAssets(lang).catch((err) => console.error("assets:", err));
  const kinds = (c.req.query("kind") ?? "").split(",").map((k) => k.trim()).filter(Boolean);
  const all = Object.values(idx.entries)
    .filter((e) => !kinds.length || kinds.includes(e.kind))
    .sort((a, b) => (a.id < b.id ? -1 : 1));
  const from = Math.max(0, Number(c.req.query("from") ?? 0) || 0);
  const limit = Math.max(1, Math.min(2000, Number(c.req.query("limit") ?? 2000) || 2000));
  const items = all.slice(from, from + limit);
  const p = assetProgress(lang);
  return c.json({
    ok: true,
    version: versionOf(Object.values(idx.entries)),
    lang,
    building: p.building,
    progress: { done: p.done, total: p.total },
    total: all.length,
    from,
    count: items.length,
    bytes: all.reduce((a, e) => a + e.bytes, 0),
    // `items` (y no `files`): es el nombre que parsea AssetSyncActivity.
    items,
  });
});

assets.get("/status", async (c) => {
  const rows = [];
  for (const lang of LANGS) {
    const idx = indexes.get(lang) ?? (await stat(indexFile(lang)).then(() => loadIndex(lang), () => null));
    if (!idx) continue;
    const p = assetProgress(lang);
    const byKind: Record<string, { count: number; bytes: number }> = {};
    for (const e of Object.values(idx.entries)) {
      byKind[e.kind] ??= { count: 0, bytes: 0 };
      byKind[e.kind].count++;
      byKind[e.kind].bytes += e.bytes;
    }
    rows.push({
      lang,
      version: versionOf(Object.values(idx.entries)),
      files: Object.keys(idx.entries).length,
      bytes: totalBytes(idx),
      byKind,
      building: p.building,
      done: p.done,
      total: p.total,
      missing: p.missing,
      error: p.error,
    });
  }
  // `lucide` sigue saliendo por compatibilidad: la tarjeta de /board todavía lo
  // muestra (board.ts) y sin el campo diría "undefined". Cuando esa página diga
  // "Dibujos: <art>", se saca de acá.
  return c.json({ ok: true, art: ART_REV, lucide: ART_REV, langs: rows });
});

assets.post("/build", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  void buildAssets(lang).catch((err) => console.error("assets:", err));
  return c.json({ ok: true, ...assetProgress(lang) });
});

// El archivo, con Range para poder reanudar. El aparato baja de a pedazos y
// puede quedarse sin WiFi (o sin batería) en el medio.
assets.get("/file", async (c) => {
  const id = (c.req.query("id") ?? "").trim();
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  const entry = await findEntry(id, normalizeLang(c.req.query("lang")));
  if (!entry) return c.json({ ok: false, error: "unknown asset", code: "not_found" }, 404);
  const file = localPath(entry);
  const st = await stat(file).catch(() => null);
  if (!st) return c.json({ ok: false, error: "asset missing on the server", code: "gone" }, 410);

  const headers: Record<string, string> = {
    "Content-Type": entry.kind === "bible" ? "text/plain; charset=utf-8" : "application/octet-stream",
    "Accept-Ranges": "bytes",
    ETag: `"${entry.sha}"`,
    "Cache-Control": "public, max-age=31536000, immutable",
  };
  const range = c.req.header("range");
  const m = /^bytes=(\d*)-(\d*)$/.exec(range ?? "");
  if (m && (m[1] || m[2])) {
    let start = m[1] ? Number(m[1]) : st.size - Number(m[2]);
    let end = m[1] && m[2] ? Number(m[2]) : st.size - 1;
    if (!Number.isFinite(start) || start < 0) start = 0;
    if (!Number.isFinite(end) || end >= st.size) end = st.size - 1;
    if (start > end) {
      return new Response(null, { status: 416, headers: { ...headers, "Content-Range": `bytes */${st.size}` } });
    }
    headers["Content-Range"] = `bytes ${start}-${end}/${st.size}`;
    headers["Content-Length"] = String(end - start + 1);
    // Bun.file().slice() manda solo el pedazo pedido sin leerlo entero a memoria.
    return new Response(Bun.file(file).slice(start, end + 1), { status: 206, headers });
  }
  headers["Content-Length"] = String(st.size);
  return new Response(Bun.file(file), { headers });
});

// Al arrancar, como el warmUp de Piper: la primera vez tarda unos minutos
// (Piper dice 480 palabras) y queda en el volumen para siempre.
export function warmAssets(lang: Lang): void {
  if (!BUILD_ON_START) return;
  setTimeout(() => void buildAssets(lang).catch((err) => console.error("assets:", err)), 5_000);
}
