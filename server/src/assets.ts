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
// tarjetas más el dibujo de cada una, BMP de 2 bpp en 4 grises que lee el
// `Bitmap` del firmware), `sounds` (la palabra de cada tarjeta dicha por Piper,
// ADPCM del mismo formato que /api/tts), `icons` (reservado).
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
// Los dibujos salen de Noto Color Emoji (googlefonts/noto-emoji): Apache 2.0 +
// OFL, o sea que NO obligan a atribuir a nadie, y son ilustraciones llenas, no
// iconos de trazo (un bebé no reconoce un contorno). El SVG de cada emoji se
// baja una sola vez y queda guardado en el volumen.
const NOTO_REF = process.env.NOTO_EMOJI_REF ?? "main";
const NOTO_URL = (file: string) => `https://cdn.jsdelivr.net/gh/googlefonts/noto-emoji@${NOTO_REF}/svg/${file}.svg`;

// El dibujo ocupa bien la pantalla de 480x800 sin comerse el lugar de la palabra.
export const CARD_PX = 320;
// Margen: el emoji ocupa el 94 % del cuadro y el resto queda de aire.
const CARD_INSET = 0.94;
// Los cuatro grises del panel (los mismos que usa toDeviceBmp en photos.ts).
const LEVELS = [0, 85, 170, 255];
// Hasta dónde se aclara la figura. Un emoji amarillo (la luna, la estrella, la
// banana) en gris queda casi blanco y DESAPARECE contra el fondo, así que el
// tono de cada dibujo se estira a [0, 190]: el más claro de la figura cae en el
// gris 170 y nunca en el blanco del fondo. Comparado mirando las tres opciones
// (tal cual / escala fija / normalizado): normalizado es el único que deja
// legibles la luna y el vaso de leche.
const ART_HI = 190;
// Marca de formato del dibujo. Al cambiarla, las tarjetas viejas del volumen se
// descartan solas y se regeneran (ver loadIndex).
const ART_REV = `noto/${NOTO_REF}/4gray-v1`;

export type AssetKind = "bible" | "cards" | "sounds" | "icons";
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
  out.set("cards/index", `index/${CARDS.length}`);
  for (const c of CARDS) {
    out.set(`cards/${c.id}`, c.icon);
    out.set(`sounds/es/${c.id}`, c.es);
    out.set(`sounds/en/${c.id}`, c.en);
  }
  return out;
}

async function loadIndex(lang: Lang): Promise<Index> {
  const have = indexes.get(lang);
  if (have) return have;
  const idx = await readJsonSafe<Index>(indexFile(lang), { art: ART_REV, entries: {} });
  idx.entries ??= {};
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
      if (!want.has(id)) return e.kind === "bible";  // ids de tarjetas que ya no existen
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

// ── Dibujos de las tarjetas ─────────────────────────────────────────────────
//
// BMP de 2 bits por píxel con paleta de cuatro grises: el MISMO formato que ya
// usan las fotos y los adjuntos (`toDeviceBmp` en photos.ts), o sea el que el
// lector de BMP del firmware (`lib/GfxRenderer/Bitmap`) ya sabe leer. Filas de
// abajo hacia arriba (BMP clásico) y padding a 4 bytes; con 320 px de ancho la
// fila mide 80 bytes y ya está alineada.
//
//   14  BITMAPFILEHEADER  ("BM", tamaño, offset de los datos = 70)
//   40  BITMAPINFOHEADER  (1 plano, 2 bpp, sin compresión, 4 colores)
//   16  paleta            (0 negro, 1 gris oscuro, 2 gris claro, 3 blanco)
//   …   las filas, 4 píxeles por byte, el de más a la izquierda en los bits altos
export function packBmp2(idx: Uint8Array, w: number, h: number): Uint8Array {
  const stride = Math.ceil((w * 2) / 32) * 4;         // múltiplo de 4
  const dataSize = stride * h;
  const offset = 14 + 40 + 16;
  const out = new Uint8Array(offset + dataSize);
  const dv = new DataView(out.buffer);
  out[0] = 0x42; out[1] = 0x4d;                       // "BM"
  dv.setUint32(2, out.length, true);
  dv.setUint32(10, offset, true);
  dv.setUint32(14, 40, true);                         // tamaño del INFOHEADER
  dv.setInt32(18, w, true);
  dv.setInt32(22, h, true);                           // positivo = de abajo hacia arriba
  dv.setUint16(26, 1, true);                          // planos
  dv.setUint16(28, 2, true);                          // bits por píxel
  dv.setUint32(30, 0, true);                          // sin compresión
  dv.setUint32(34, dataSize, true);
  dv.setUint32(38, 2835, true);                       // 72 ppp
  dv.setUint32(42, 2835, true);
  dv.setUint32(46, 4, true);                          // colores usados
  dv.setUint32(50, 4, true);                          // colores importantes
  for (let i = 0; i < 4; i++) {                       // paleta BGRA en gris
    const o = 54 + i * 4;
    out[o] = LEVELS[i]; out[o + 1] = LEVELS[i]; out[o + 2] = LEVELS[i]; out[o + 3] = 0;
  }
  for (let y = 0; y < h; y++) {
    const row = offset + (h - 1 - y) * stride;         // la primera fila del archivo es la de abajo
    for (let x = 0; x < w; x++) out[row + (x >> 2)] |= (idx[y * w + x] & 3) << (6 - 2 * (x % 4));
  }
  return out;
}

// Los emoji no traen dígitos, así que el dibujo de los números se arma acá: N
// puntos para contar, que además es lo que de verdad sirve a esa edad.
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
    circles.push(`<circle cx="${cx.toFixed(2)}" cy="${cy.toFixed(2)}" r="${r.toFixed(2)}"/>`);
  }
  return svgWrap(circles.join(""));
}

// Las formas también se dibujan acá: los emoji de formas son cuadraditos de
// colores y en gris no se distinguen entre sí. Llenas en negro, que es lo que
// más se lee en tinta electrónica.
function starPoints(cx: number, cy: number, outer: number, inner: number): string {
  const pts: string[] = [];
  for (let i = 0; i < 10; i++) {
    const r = i % 2 ? inner : outer;
    const a = -Math.PI / 2 + (i * Math.PI) / 5;
    pts.push(`${(cx + r * Math.cos(a)).toFixed(2)},${(cy + r * Math.sin(a)).toFixed(2)}`);
  }
  return pts.join(" ");
}

const SHAPES: Record<string, string> = {
  circulo: `<circle cx="12" cy="12" r="10.5"/>`,
  cuadrado: `<rect x="1.5" y="1.5" width="21" height="21" rx="1.2"/>`,
  rectangulo: `<rect x="1" y="5.5" width="22" height="13" rx="1.2"/>`,
  triangulo: `<polygon points="12,1.8 22.6,21.5 1.4,21.5"/>`,
  ovalo: `<ellipse cx="12" cy="12" rx="11" ry="7.5"/>`,
  rombo: `<polygon points="12,1.4 22.6,12 12,22.6 1.4,12"/>`,
  estrella: `<polygon points="${starPoints(12, 12.6, 11, 4.6)}"/>`,
  corazon: `<path d="M12 22.2 L2.4 12.2 A 5.6 5.6 0 1 1 12 6.0 A 5.6 5.6 0 1 1 21.6 12.2 Z"/>`,
  cruz: `<polygon points="9,1.5 15,1.5 15,9 22.5,9 22.5,15 15,15 15,22.5 9,22.5 9,15 1.5,15 1.5,9 9,9"/>`,
};

const svgWrap = (body: string) =>
  `<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="#000">${body}</svg>`;

const svgCache = new Map<string, string>();

// El nombre del archivo en noto-emoji: emoji_u1f436.svg, y los de varios puntos
// de código con "_" en el medio (emoji_u1f469_200d_1f373.svg).
const notoFile = (cp: string) => `emoji_u${cp.split("-").join("_")}`;

async function iconSvg(icon: string): Promise<string> {
  if (icon.startsWith("num:")) return numberSvg(Number(icon.slice(4)));
  if (icon.startsWith("shape:")) {
    const body = SHAPES[icon.slice(6)];
    if (!body) throw new Error(`forma desconocida: ${icon}`);
    return svgWrap(body);
  }
  const have = svgCache.get(icon);
  if (have) return have;
  const cp = icon.startsWith("emoji:") ? icon.slice(6) : icon;
  // Copia local primero: así una segunda generación (o un jsDelivr caído) no
  // deja el paquete a medias, y el original queda guardado en el volumen.
  const local = `${DIR}/emoji/${cp}.svg`;
  let svg = await readFile(local, "utf8").catch(() => "");
  if (!svg) {
    const res = await fetch(NOTO_URL(notoFile(cp)), { signal: AbortSignal.timeout(20_000) });
    if (!res.ok) throw new Error(`noto ${cp}: ${res.status}`);
    svg = await res.text();
    await writeAsset(local, new TextEncoder().encode(svg));
  }
  svgCache.set(icon, svg);
  return svg;
}

// SVG -> BMP de 2 bpp en 4 grises. Rasteriza sharp (libvips ya trae el motor de
// SVG y sharp ya está instalado para las fotos): sin Python, sin rsvg-convert.
//
// Tres pasos, y los tres importan:
//  1. se rasteriza CON transparencia, para saber qué píxel es figura y cuál es
//     fondo;
//  2. el tono de la figura se estira a [0, ART_HI] (ver ART_HI): un dibujo
//     amarillo o blanco no puede quedar del color del papel;
//  3. Floyd-Steinberg a los cuatro grises del panel, igual que las fotos.
export async function renderCard(card: Card): Promise<Uint8Array> {
  const svg = await iconSvg(card.icon);
  const inner = Math.round(CARD_PX * CARD_INSET);
  const pad = (CARD_PX - inner) >> 1;
  const { data, info } = await sharp(Buffer.from(svg), { density: 512 })
    .resize(inner, inner, { fit: "contain", background: { r: 0, g: 0, b: 0, alpha: 0 } })
    .extend({ top: pad, bottom: CARD_PX - inner - pad, left: pad, right: CARD_PX - inner - pad, background: { r: 0, g: 0, b: 0, alpha: 0 } })
    .ensureAlpha()
    .toColourspace("srgb")
    .raw()
    .toBuffer({ resolveWithObject: true });
  const w = info.width;
  const h = info.height;
  const n = w * h;
  const ch = info.channels;
  const lum = new Float32Array(n);
  const alpha = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const o = i * ch;
    lum[i] = ch >= 3 ? 0.299 * data[o] + 0.587 * data[o + 1] + 0.114 * data[o + 2] : data[o];
    alpha[i] = ch === 4 || ch === 2 ? data[o + ch - 1] / 255 : 1;
  }
  // Percentiles 2/98 de la figura (no del fondo): un par de píxeles sueltos del
  // antialias no tienen que decidir el contraste de toda la tarjeta.
  const vals: number[] = [];
  for (let i = 0; i < n; i++) if (alpha[i] > 0.6) vals.push(lum[i]);
  vals.sort((a, b) => a - b);
  let lo = 0;
  let hi = 255;
  if (vals.length > 50) {
    lo = vals[Math.floor(vals.length * 0.02)];
    hi = vals[Math.floor(vals.length * 0.98)];
  }
  if (hi - lo < 20) {                                  // dibujo de un solo tono
    lo = Math.max(0, lo - 40);
    hi = lo + 60;
  }
  const gray = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const v = Math.max(0, Math.min(ART_HI, ((lum[i] - lo) / (hi - lo)) * ART_HI));
    gray[i] = 255 - alpha[i] * (255 - v);              // sobre blanco
  }
  // SOLO BORDES, en blanco y negro puro.
  //
  // La tarjeta se dibujaba con los cuatro grises del panel, y eso en el aparato
  // cuesta TRES pasadas de refresco (base en blanco y negro, pasada LSB, pasada
  // MSB) más el render por franjas: pasar una tarjeta tardaba una eternidad.
  // Con el dibujo a puro trazo alcanza UNA pasada, que es la diferencia entre
  // esperar y no esperar.
  //
  // El trazo sale de los saltos de tono: donde dos píxeles vecinos se
  // diferencian más que EDGE hay un borde. Eso agarra la silueta contra el
  // fondo blanco y también el detalle de adentro (las manchas del ala de la
  // mariposa), que es lo que un relleno plano se comería.
  const EDGE = 28;
  const idx = new Uint8Array(n);
  idx.fill(3);  // 3 = blanco
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const p = y * w + x;
      const g = gray[p];
      const right = x + 1 < w && Math.abs(g - gray[p + 1]) > EDGE;
      const down = y + 1 < h && Math.abs(g - gray[p + w]) > EDGE;
      // Dos píxeles de grosor: uno solo, en tinta y a este tamaño, queda tan
      // fino que el dibujo se lee desvaído.
      if (right || down) {
        idx[p] = 0;  // 0 = negro
        if (right) idx[p + 1] = 0;
        if (down) idx[p + w] = 0;
      }
    }
  }
  return packBmp2(idx, w, h);
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
  // El índice va primero: sin él CardsActivity no sabe qué tarjetas hay.
  out.push({
    id: "cards/index",
    kind: "cards",
    path: CARD_INDEX_PATH,
    tag: `index/${CARDS.length}`,
    make: async () => new TextEncoder().encode(cardIndexJson()),
  });
  for (const card of CARDS) {
    out.push({ id: `cards/${card.id}`, kind: "cards", path: cardImagePath(card.id), tag: card.icon, make: () => renderCard(card) });
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
        tag: voice === "en" ? card.en : card.es,
        make: () => synthesize(voice === "en" ? card.en : card.es, voice, 4),
      });
    }
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
  return c.json({ ok: true, art: ART_REV, lucide: ART_REV, cards: CARDS.length, langs: rows });
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
