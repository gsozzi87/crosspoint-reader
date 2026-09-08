// Paquete de contenido descargable: todo lo pesado (la Biblia, los dibujos y la
// voz de las tarjetas de bebé, los sonidos) vive acá y el aparato se lo baja de
// una sola vez después de actualizar el firmware, en vez de tener un botón
// distinto por cada cosa.
//
// Idea: el aparato pide el manifiesto, lo compara con lo que ya tiene en la SD
// (mismo `path`, mismo `sha`) y baja SOLO lo que falta o cambió. Cada archivo se
// baja con `Range`, así que una descarga cortada se sigue después sin empezar de
// cero.
//
//   GET /api/assets/manifest?lang=xx[&kind=bible,cards][&from=0&limit=400]
//     -> { ok, version, lang, building, progress:{done,total}, total, from, count,
//          bytes, items: [{ id, kind, path, bytes, sha }] }
//        `version` es el resumen de TODO el paquete de ese idioma: si no cambió,
//        no hace falta mirar nada más.
//        `sha` = primeros 16 hex del sha256 del archivo (64 bits alcanzan y el
//        manifiesto entero entra en la RAM del aparato).
//        `path` = dónde va en la SD, tal cual.
//        `building` = todavía se están generando archivos (la voz de las
//        tarjetas la hace Piper una sola vez); volvé más tarde por el resto.
//
//   GET /api/assets/file?id=<id>
//     -> el archivo. `Content-Length`, `ETag: "<sha>"`, `Accept-Ranges: bytes`.
//        Con `Range: bytes=N-` devuelve 206 y `Content-Range`, para reanudar.
//
//   GET /api/assets/status   -> cómo va la generación (para /board)
//   POST /api/assets/build   -> forzar la generación (idempotente)
//
// Kinds: `bible` (66 archivos, uno por libro), `cards` (el índice de las
// tarjetas más el dibujo de cada una, BMP de 1 bpp que lee el `Bitmap` del
// firmware), `sounds` (la palabra de cada tarjeta dicha por Piper, ADPCM del
// mismo formato que /api/tts), `icons` (reservado).
//
// Las rutas y los nombres son los que espera el firmware (`AssetSyncActivity` y
// `CardsActivity`): el índice en `/.crosspoint/cards/index.json`, los dibujos en
// `cards/img/<id>.bmp` y la voz en `cards/audio/<es|en>/<id>.adp`.
//
// Todo se genera solo (al arrancar el servidor y al pedir el manifiesto) y queda
// cacheado en el volumen: no hay ningún paso a mano.
import { Hono } from "hono";
import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile, rename } from "node:fs/promises";
import sharp from "sharp";
import { normalizeLang, LANGS, type Lang } from "./lang";
import { CARDS, type Card } from "./cards";
import { bookText, bookCount } from "./bible";
import { synthesize } from "./tts";
import { readJsonSafe, writeJsonAtomic } from "./fsjson";

const DIR = process.env.ASSETS_DIR ?? "/data/assets";
const BUILD_ON_START = process.env.ASSETS_BUILD !== "0";
// Lucide fijado a una versión: si flota, un dibujo puede cambiar solo y el
// aparato se baja las 239 tarjetas de nuevo sin motivo.
const LUCIDE = process.env.LUCIDE_VERSION ?? "1.43.0";
const LUCIDE_URL = (name: string) => `https://cdn.jsdelivr.net/npm/lucide-static@${LUCIDE}/icons/${name}.svg`;

// El dibujo ocupa bien la pantalla de 480x800 sin comerse el lugar de la palabra.
export const CARD_PX = 320;
// Lucide dibuja a 24 px con trazo 2; escalado a 320 el trazo quedaría de 27 px y
// el dibujo sería una mancha. 1,25 da un trazo de ~17 px: grueso, redondo y
// clarísimo en tinta electrónica, que es justo lo que sirve para un bebé.
// Grosor del trazo del dibujo de la tarjeta. Se AGRANDA respecto del 2 que trae
// Lucide, no se achica: a 320 px en blanco y negro puro (sin grises), un trazo
// fino se lee lavado y de lejos no se distingue. Comparado renderizando las dos
// versiones y mirandolas: 1.25 quedaba debil, 2.6 se lee de lejos.
const STROKE = process.env.CARD_STROKE ?? "2.6";
// Un gris por debajo de esto cuenta como negro (mismo umbral que gen_icons.py).
const THRESHOLD = 110;

export type AssetKind = "bible" | "cards" | "sounds" | "icons";
export type AssetEntry = { id: string; kind: AssetKind; path: string; bytes: number; sha: string };

// El índice se guarda en el volumen: un reinicio del contenedor no tiene que
// volver a generar 239 dibujos y 478 clips de voz.
type Index = { lucide: string; entries: Record<string, AssetEntry> };
// La marca lleva el grosor: al cambiarlo, las tarjetas viejas se descartan solas.
const indexFile = (lang: Lang) => `${DIR}/index-${lang}.json`;
const indexes = new Map<Lang, Index>();

async function loadIndex(lang: Lang): Promise<Index> {
  const have = indexes.get(lang);
  if (have) return have;
  const idx = await readJsonSafe<Index>(indexFile(lang), { lucide: `${LUCIDE}/${STROKE}`, entries: {} });
  // Si cambió la versión de Lucide, los dibujos se rehacen (el sha va a cambiar
  // igual, pero así no se sirven archivos viejos con el sha nuevo).
  if (idx.lucide !== `${LUCIDE}/${STROKE}`) idx.entries = Object.fromEntries(Object.entries(idx.entries ?? {}).filter(([, e]) => e.kind !== "cards"));
  idx.lucide = `${LUCIDE}/${STROKE}`;
  idx.entries ??= {};
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

// ── Dibujos de las tarjetas ─────────────────────────────────────────────────
//
// BMP de 1 bit por píxel con paleta de dos colores, que es lo que el lector de
// BMP del firmware (`lib/GfxRenderer/Bitmap`) ya sabe dibujar: nada de formatos
// propios. Filas de abajo hacia arriba (BMP clásico) y padding a 4 bytes; con
// 320 px de ancho la fila mide 40 bytes y ya está alineada.
//
//   14  BITMAPFILEHEADER  ("BM", tamaño, offset de los datos = 62)
//   40  BITMAPINFOHEADER  (1 plano, 1 bpp, sin compresión, 2 colores)
//    8  paleta            (índice 0 = negro, índice 1 = blanco)
//   …   las filas, MSB primero: el bit en 1 es blanco
export function packBmp1(gray: Uint8Array, w: number, h: number): Uint8Array {
  const stride = ((Math.ceil(w / 8) + 3) >> 2) << 2;  // múltiplo de 4
  const dataSize = stride * h;
  const offset = 14 + 40 + 8;
  const out = new Uint8Array(offset + dataSize);
  const dv = new DataView(out.buffer);
  out[0] = 0x42; out[1] = 0x4d;                       // "BM"
  dv.setUint32(2, out.length, true);
  dv.setUint32(10, offset, true);
  dv.setUint32(14, 40, true);                         // tamaño del INFOHEADER
  dv.setInt32(18, w, true);
  dv.setInt32(22, h, true);                           // positivo = de abajo hacia arriba
  dv.setUint16(26, 1, true);                          // planos
  dv.setUint16(28, 1, true);                          // bits por píxel
  dv.setUint32(30, 0, true);                          // sin compresión
  dv.setUint32(34, dataSize, true);
  dv.setUint32(38, 2835, true);                       // 72 ppp
  dv.setUint32(42, 2835, true);
  dv.setUint32(46, 2, true);                          // colores usados
  dv.setUint32(50, 2, true);                          // colores importantes
  // Paleta BGRA: 0 = negro, 1 = blanco.
  out[54] = 0; out[55] = 0; out[56] = 0; out[57] = 0;
  out[58] = 255; out[59] = 255; out[60] = 255; out[61] = 0;
  for (let y = 0; y < h; y++) {
    const row = offset + (h - 1 - y) * stride;         // la primera fila del archivo es la de abajo
    for (let xb = 0; xb * 8 < w; xb++) {
      let byte = 0;
      for (let b = 0; b < 8; b++) {
        const x = xb * 8 + b;
        const white = x < w && gray[y * w + x] < THRESHOLD ? 0 : 1;
        byte |= white << (7 - b);
      }
      out[row + xb] = byte;
    }
  }
  return out;
}

// Los números no están en Lucide (no hay dígitos), así que el dibujo se arma
// acá: N puntos para contar, que además es lo que de verdad sirve a esa edad.
function numberSvg(n: number): string {
  const cols = n <= 3 ? n : n <= 6 ? 3 : n <= 8 ? 4 : 5;
  const rows = Math.ceil(n / cols);
  const cell = 24 / Math.max(cols, rows);
  const r = cell * 0.34;
  const circles: string[] = [];
  for (let i = 0; i < n; i++) {
    const row = Math.floor(i / cols);
    const inRow = Math.min(cols, n - row * cols);
    const cx = 12 + (i % cols - (inRow - 1) / 2) * cell;
    const cy = 12 + (row - (rows - 1) / 2) * cell;
    circles.push(`<circle cx="${cx.toFixed(2)}" cy="${cy.toFixed(2)}" r="${r.toFixed(2)}" fill="black"/>`);
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">${circles.join("")}</svg>`;
}

const svgCache = new Map<string, string>();

async function iconSvg(icon: string): Promise<string> {
  if (icon.startsWith("num:")) return numberSvg(Number(icon.slice(4)));
  const have = svgCache.get(icon);
  if (have) return have;
  // Copia local primero: así una segunda generación (o un jsDelivr caído) no
  // deja el paquete a medias.
  const local = `${DIR}/svg/${icon}.svg`;
  let svg = await readFile(local, "utf8").catch(() => "");
  if (!svg) {
    const res = await fetch(LUCIDE_URL(icon), { signal: AbortSignal.timeout(20_000) });
    if (!res.ok) throw new Error(`lucide ${icon}: ${res.status}`);
    svg = await res.text();
    await writeAsset(local, new TextEncoder().encode(svg));
  }
  svgCache.set(icon, svg);
  return svg;
}

// SVG -> bitmap de 1 bpp. Rasteriza sharp (libvips ya trae el motor de SVG y
// sharp ya está instalado para las fotos): sin Python, sin rsvg-convert.
export async function renderCard(card: Card): Promise<Uint8Array> {
  let svg = await iconSvg(card.icon);
  // El trazo se engrosa ANTES de escalar (ver STROKE).
  svg = svg.replace(/stroke-width\s*=\s*"[^"]*"/g, `stroke-width="${STROKE}"`);
  const { data, info } = await sharp(Buffer.from(svg), { density: 384 })
    .resize(CARD_PX, CARD_PX, { fit: "contain", background: { r: 255, g: 255, b: 255, alpha: 1 } })
    .flatten({ background: "#ffffff" })
    .greyscale()
    .raw()
    .toBuffer({ resolveWithObject: true });
  return packBmp1(new Uint8Array(data), info.width, info.height);
}

// ── Rutas en la SD ──────────────────────────────────────────────────────────
// La Biblia mantiene la ruta que el aparato ya usa, así lo que esté bajado sigue
// sirviendo y el botón de "bajar la Biblia" se puede sacar sin migrar nada.
const biblePath = (lang: Lang, i: number) => `/.crosspoint/bible/${lang}/b${String(i).padStart(2, "0")}.txt`;
// Estas tres son las que espera CardsActivity del firmware; no cambiarlas sin
// cambiarlas allá.
const CARD_INDEX_PATH = "/.crosspoint/cards/index.json";
const cardImagePath = (id: string) => `/.crosspoint/cards/img/${id}.bmp`;
const cardAudioPath = (lang: Lang, id: string) => `/.crosspoint/cards/audio/${lang}/${id}.adp`;

// Dónde vive el archivo del lado del servidor: el mismo árbol que en la SD,
// colgando de ASSETS_DIR. Así el id no tiene que codificar nada.
function localPath(entry: { path: string }): string {
  return `${DIR}${entry.path.replace(/^\/\.crosspoint/, "")}`;
}

// El índice que lee CardsActivity: la palabra en los dos idiomas, la categoría
// y dónde están el dibujo y los dos audios (relativos a /.crosspoint, como los
// arma el firmware por su cuenta si faltan).
export function cardIndexJson(): string {
  return JSON.stringify({
    cards: CARDS.map((c) => ({
      id: c.id,
      es: c.es,
      en: c.en,
      cat: CATEGORY_LABEL[c.cat],
      img: `cards/img/${c.id}.bmp`,
      audioEs: `cards/audio/es/${c.id}.adp`,
      audioEn: `cards/audio/en/${c.id}.adp`,
    })),
  });
}

// Nombre de la categoría tal como se ve en el aparato (español neutro).
const CATEGORY_LABEL: Record<string, string> = {
  animales: "Animales", comida: "Comida", casa: "Casa", cuerpo: "Cuerpo", formas: "Formas",
  numeros: "Números", vehiculos: "Vehículos", naturaleza: "Naturaleza", ropa: "Ropa", juguetes: "Juguetes",
};

// ── Generación ──────────────────────────────────────────────────────────────
// Todo lo que hay que tener. El id lleva el idioma adentro para que un mismo
// manifiesto no pueda mezclar el audio de dos idiomas.
type Planned = { id: string; kind: AssetKind; path: string; make: () => Promise<Uint8Array | null> };

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
  // El índice va primero: sin él CardsActivity no sabe qué tarjetas hay.
  out.push({
    id: "cards/index",
    kind: "cards",
    path: CARD_INDEX_PATH,
    make: async () => new TextEncoder().encode(cardIndexJson()),
  });
  for (const card of CARDS) {
    out.push({ id: `cards/${card.id}`, kind: "cards", path: cardImagePath(card.id), make: () => renderCard(card) });
  }
  // La voz de la tarjeta SIEMPRE en español y en inglés, sea cual sea el idioma
  // del aparato: la gracia del juego es que el bebé escuche la palabra en los
  // dos, y son los dos únicos idiomas en los que están escritas las palabras
  // (CardsActivity lee justo cards/audio/es y cards/audio/en).
  for (const card of CARDS) {
    for (const voice of ["es", "en"] as const) {
      out.push({
        id: `sounds/${voice}/${card.id}`,
        kind: "sounds",
        path: cardAudioPath(voice, card.id),
        make: () => synthesize(voice === "en" ? card.en : card.es, voice, 4),
      });
    }
  }
  return out;
}

type Progress = { lang: Lang; done: number; total: number; building: boolean; error: string; startedAt: number; finishedAt: number; missing: number };
const progress = new Map<Lang, Progress>();
const running = new Map<Lang, Promise<void>>();
// No se vuelve a revisar en cada pedido del manifiesto: repasar 783 archivos es
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
        idx.entries[item.id] = { id: item.id, kind: item.kind, path: item.path, bytes: data.length, sha: sha16(data) };
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
  return c.json({ ok: true, lucide: LUCIDE, cards: CARDS.length, langs: rows });
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
// (Piper dice 478 palabras) y queda en el volumen para siempre.
export function warmAssets(lang: Lang): void {
  if (!BUILD_ON_START) return;
  setTimeout(() => void buildAssets(lang).catch((err) => console.error("assets:", err)), 5_000);
}
