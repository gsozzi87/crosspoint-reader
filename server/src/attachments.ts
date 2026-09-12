// Adjuntos de los viajes: el PDF del vuelo, la reserva del hotel, la entrada al
// museo. El teléfono los sube tal como salen del mail y acá se convierten a algo
// que el aparato pueda pintar de una: BMP de 2 bpp de 480x800, igual que las
// fotos (`photos.ts`). El aparato no sabe leer PDF ni tiene CPU para nada de esto.
//
// Lo delicado son los códigos de los pases de embarque. Casi nunca son QR: son
// PDF417 (IATA BCBP) o Aztec, con módulos de menos de un milímetro. Escalar la
// imagen del PDF a 480 px de ancho deja un borrón que el escáner del aeropuerto
// no lee. Por eso el camino bueno es **decodificar y volver a generar**:
//   1. mupdf rasteriza la página a 200 dpi (y a 400 si hace falta),
//   2. zxing-wasm lee el código y devuelve el texto exacto,
//   3. bwip-js lo vuelve a generar limpio, en blanco y negro puro, sin grises ni
//      suavizado, al tamaño más grande que entre en la pantalla,
//   4. se vuelve a decodificar el bitmap final y solo si el texto coincide se
//      marca `verified: true`.
// Si no se puede decodificar, se recorta la región a máxima resolución y se
// marca `copy: true` con un aviso: el usuario tiene que saber ANTES de llegar al
// mostrador que ese código puede no escanear.
//
//   GET  /api/attachment?id=&page=   -> image/bmp de 2 bpp, 480x800 (96 KB)
//   GET  /api/attachment/info?id=    -> texto extraído, páginas y códigos
//   POST /api/attachment/delete      -> {id}
import { Hono } from "hono";
import { mkdir, readFile, readdir, rm } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import * as mupdf from "mupdf";
import sharp from "sharp";
import bwipjs from "bwip-js/node";
import { readBarcodes, prepareZXingModule } from "zxing-wasm/reader";
import { attachmentsDir, mutateDoc, readDoc, writeBytesAtomic } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";
import { bmpToPng, toDeviceBmp } from "./photos";
import { readBody } from "./net";

// Los bitmaps son archivos: cada cuenta tiene su directorio (la 1, la que ya
// estaba andando, se queda en /data/attachments). El índice es un documento.
export const dirFor = attachmentsDir;

// La pantalla del aparato.
const SCREEN_W = 480;
const SCREEN_H = 800;

// El aparato no soporta Range y baja el archivo entero con tope de 512 KB. Una
// página de 480x800 a 2 bpp son 96 KB, así que entra holgada; el tope está acá
// como red de seguridad por si alguien cambia el tamaño.
export const MAX_PAGE_BYTES = 512 * 1024;
// Lo que se acepta subir desde el teléfono (un PDF de reserva con fotos).
export const MAX_UPLOAD_BYTES = 20 * 1024 * 1024;
// Páginas que se rasterizan por adjunto: un itinerario de 40 páginas no tiene
// sentido en una pantalla de e-ink, y cada página ocupa 96 KB del volumen.
const MAX_PAGES = 12;
const MAX_ATTACHMENTS = 200;
// Presupuesto de píxeles para no reventar la memoria con un A3 a 400 dpi.
const MAX_RASTER_PIXELS = 14_000_000;

export type CodeInfo = {
  format: string;        // PDF417, Aztec, QRCode, ...
  text: string;          // lo que dice el código (vacío si no se pudo leer)
  page: number;          // qué página del adjunto lo muestra
  fromPage: number;      // en qué página del PDF estaba
  verified: boolean;     // el bitmap generado se volvió a leer y da lo mismo
  copy: boolean;         // es un recorte de la imagen original, puede no escanear
  warn?: string;
};

export type PageInfo = {
  n: number;
  kind: "code" | "page";
  label: string;
};

export type Attachment = {
  id: string;
  tripId?: string;
  name: string;
  mime: string;
  bytes: number;
  kind: "pdf" | "image" | "code";
  pages: number;
  pageList: PageInfo[];
  extracted: string;
  fields: { label: string; value: string }[];
  codes: CodeInfo[];
  warn: string;
  at: string;
};

type Index = { version: number; items: Attachment[] };

// ---------------------------------------------------------------- índice

function shapeIndex(raw: unknown): Index {
  const idx = (raw && typeof raw === "object" ? raw : {}) as Index;
  idx.version ||= 1;
  idx.items ??= [];
  return idx;
}

// Solo para LEER. Todo lo que modifica el índice va por mutateIndex: leer con
// readDoc, cambiar una copia y guardar con writeDoc son dos operaciones
// separadas, así que dos subidas o un borrado y una subida a la vez terminaban
// con el último escribiendo encima de lo que hizo el otro (una entrada del
// índice que desaparece y deja los archivos tirados en el volumen). mutateDoc
// toma el candado —pg_advisory_xact_lock en Postgres, serialize() por archivo
// en el volumen— sobre la lectura Y la escritura.
async function loadIndex(accountId: number): Promise<Index> {
  return shapeIndex(await readDoc<unknown>(accountId, "attachments", null));
}

function mutateIndex<R>(accountId: number, fn: (idx: Index) => R | Promise<R>): Promise<R> {
  return mutateDoc(accountId, "attachments", shapeIndex, fn);
}

export async function listAttachments(accountId: number, ids?: string[]): Promise<Attachment[]> {
  const idx = await loadIndex(accountId);
  if (!ids) return idx.items;
  return idx.items.filter((a) => ids.includes(a.id));
}

export async function getAttachment(accountId: number, id: string): Promise<Attachment | null> {
  const idx = await loadIndex(accountId);
  return idx.items.find((a) => a.id === id) ?? null;
}

export async function deleteAttachment(accountId: number, id: string): Promise<boolean> {
  const clean = safeId(id);
  if (!clean) return false;
  const gone = await mutateIndex(accountId, (idx) => {
    const before = idx.items.length;
    idx.items = idx.items.filter((a) => a.id !== clean);
    return idx.items.length !== before;
  });
  if (!gone) return false;
  await rm(`${dirFor(accountId)}/${clean}`, { recursive: true, force: true }).catch(() => {});
  return true;
}

// Borra los adjuntos de un viaje que se borró (si no, quedan 96 KB por página
// tirados en el volumen para siempre).
export async function deleteAttachmentsOfTrip(accountId: number, tripId: string): Promise<number> {
  const mine = await mutateIndex(accountId, (idx) => {
    const found = idx.items.filter((a) => a.tripId === tripId);
    if (found.length) idx.items = idx.items.filter((a) => a.tripId !== tripId);
    return found;
  });
  if (!mine.length) return 0;
  for (const a of mine) await rm(`${dirFor(accountId)}/${a.id}`, { recursive: true, force: true }).catch(() => {});
  return mine.length;
}

export function safeId(raw: unknown): string {
  return (raw ?? "").toString().replace(/[^a-z0-9]/gi, "").slice(0, 24);
}

// ---------------------------------------------------------------- zxing

let zxingReady: Promise<void> | null = null;

// El .wasm viene adentro del paquete (1 MB). Sin este override, zxing-wasm se
// lo baja de un CDN en cada arranque: en Railway eso es una dependencia de red
// que no hace falta, y acá la red sale por un proxy.
function initZxing(): Promise<void> {
  zxingReady ??= (async () => {
    const path = fileURLToPath(import.meta.resolve("zxing-wasm/reader/zxing_reader.wasm"));
    const wasm = await readFile(path);
    const bin = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength) as ArrayBuffer;
    await prepareZXingModule({ overrides: { wasmBinary: bin }, fireImmediately: true });
  })();
  return zxingReady;
}

type RawImage = { data: Uint8ClampedArray<ArrayBuffer>; width: number; height: number };

type Found = { format: string; text: string; box: { x: number; y: number; w: number; h: number } | null };

async function decodeCodes(img: RawImage): Promise<Found[]> {
  await initZxing();
  const res = await readBarcodes(
    { data: img.data, width: img.width, height: img.height, colorSpace: "srgb" },
    { tryHarder: true, tryInvert: true, tryRotate: true, maxNumberOfSymbols: 8 },
  );
  return res
    .filter((r) => r.text)
    .map((r) => {
      const p = r.position as any;
      const pts = p ? [p.topLeft, p.topRight, p.bottomRight, p.bottomLeft].filter(Boolean) : [];
      let box: Found["box"] = null;
      if (pts.length === 4) {
        const xs = pts.map((q: any) => q.x);
        const ys = pts.map((q: any) => q.y);
        const x = Math.min(...xs);
        const y = Math.min(...ys);
        box = { x, y, w: Math.max(...xs) - x, h: Math.max(...ys) - y };
      }
      return { format: String(r.format), text: r.text, box };
    });
}

// ---------------------------------------------------------------- bitmaps

// Empaqueta índices de gris (0..3) en el BMP de 2 bpp que ya lee el aparato.
// Es el mismo formato que arma `toDeviceBmp` en photos.ts, pero acá se entra con
// los píxeles ya decididos: un código de barras NO se difumina.
const LEVELS = [0, 85, 170, 255];

function packDeviceBmp(idx: Uint8Array, w: number, h: number): Uint8Array {
  const rowBytes = Math.ceil((w * 2) / 32) * 4;
  const off = 14 + 40 + 4 * 4;
  const buf = new Uint8Array(off + rowBytes * h);
  const dv = new DataView(buf.buffer);
  buf[0] = 0x42;
  buf[1] = 0x4d;
  dv.setUint32(2, buf.length, true);
  dv.setUint32(10, off, true);
  dv.setUint32(14, 40, true);
  dv.setInt32(18, w, true);
  dv.setInt32(22, h, true);
  dv.setUint16(26, 1, true);
  dv.setUint16(28, 2, true);
  dv.setUint32(34, rowBytes * h, true);
  dv.setUint32(46, 4, true);
  dv.setUint32(50, 4, true);
  for (let i = 0; i < 4; i++) {
    const o = 54 + i * 4;
    buf[o] = LEVELS[i];
    buf[o + 1] = LEVELS[i];
    buf[o + 2] = LEVELS[i];
    buf[o + 3] = 0;
  }
  for (let y = 0; y < h; y++) {
    const row = off + (h - 1 - y) * rowBytes;  // BMP: de abajo hacia arriba
    for (let x = 0; x < w; x++) buf[row + (x >> 2)] |= idx[y * w + x] << (6 - 2 * (x % 4));
  }
  return buf;
}

// Lienzo blanco de 480x800 con la imagen en blanco y negro puro centrada. Nada
// de escalar con interpolación: los módulos del código tienen que quedar con los
// bordes rectos o el escáner no engancha.
function centerMono(gray: Uint8Array, w: number, h: number): Uint8Array {
  const idx = new Uint8Array(SCREEN_W * SCREEN_H).fill(3);  // 3 = blanco
  const x0 = Math.max(0, Math.floor((SCREEN_W - w) / 2));
  const y0 = Math.max(0, Math.floor((SCREEN_H - h) / 2));
  for (let y = 0; y < h && y0 + y < SCREEN_H; y++) {
    for (let x = 0; x < w && x0 + x < SCREEN_W; x++) {
      idx[(y0 + y) * SCREEN_W + (x0 + x)] = gray[y * w + x] < 128 ? 0 : 3;
    }
  }
  return idx;
}

// El bitmap que se va a guardar, vuelto a leer: es la única prueba de que el
// código que ve el escáner dice lo mismo que el original.
async function verifyBmpIdx(idx: Uint8Array, expect: string): Promise<boolean> {
  const rgba = new Uint8ClampedArray(new ArrayBuffer(SCREEN_W * SCREEN_H * 4));
  for (let i = 0; i < SCREEN_W * SCREEN_H; i++) {
    const v = LEVELS[idx[i]];
    rgba[i * 4] = v;
    rgba[i * 4 + 1] = v;
    rgba[i * 4 + 2] = v;
    rgba[i * 4 + 3] = 255;
  }
  try {
    const found = await decodeCodes({ data: rgba, width: SCREEN_W, height: SCREEN_H });
    return found.some((f) => f.text === expect);
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------- códigos

// zxing nombra los formatos como los conoce ZXing; bwip-js los nombra como BWIPP.
const BWIP: Record<string, string> = {
  PDF417: "pdf417",
  MicroPDF417: "micropdf417",
  Aztec: "azteccode",
  QRCode: "qrcode",
  MicroQRCode: "microqrcode",
  DataMatrix: "datamatrix",
  Code128: "code128",
  Code39: "code39",
  Code93: "code93",
  ITF: "interleaved2of5",
  Codabar: "rationalizedCodabar",
  EAN13: "ean13",
  EAN8: "ean8",
  UPCA: "upca",
  UPCE: "upce",
};

async function bwipGray(bcid: string, text: string, scale: number, opts: Record<string, unknown>) {
  const png = await bwipjs.toBuffer({ bcid, text, scale, includetext: false, ...opts } as any);
  const { data, info } = await sharp(png)
    .flatten({ background: { r: 255, g: 255, b: 255 } })  // el PNG de bwip-js viene con fondo transparente
    .greyscale()
    .raw()
    .toBuffer({ resolveWithObject: true });
  return { gray: new Uint8Array(data.buffer, data.byteOffset, data.byteLength), w: info.width, h: info.height };
}

// El código, generado de nuevo, lo más grande que entre en la pantalla.
// Se prueba derecho y girado 90° (a un escáner le da igual la orientación y
// girado entran 800 px de largo en vez de 480), y para PDF417 también se
// prueban distintas cantidades de columnas: el mismo dato en 4 columnas es más
// alto y angosto, y así entra con módulos más gruesos.
async function renderCode(format: string, text: string): Promise<Uint8Array | null> {
  const bcid = BWIP[format];
  if (!bcid) return null;
  const margin = 24;  // zona de silencio alrededor del código
  const maxW = SCREEN_W - 2 * margin;
  const maxH = SCREEN_H - 2 * margin;

  // Se prueba la forma de siempre y, si con esa los módulos quedan finitos,
  // también girado 90° (entran 800 px de largo en vez de 480) y con menos
  // columnas (el mismo dato más alto y angosto). Gana el que deje el módulo más
  // grande; a igual módulo se prefiere la forma normal sin girar, que es la que
  // el lector del mostrador ve todos los días.
  type Try = { opts: Record<string, unknown>; scale: number; rot: boolean; rank: number };
  let best: Try | null = null;
  const shapes: Record<string, unknown>[] =
    bcid === "pdf417"
      ? [{ height: 12 }, { height: 12, columns: 8 }, { height: 12, columns: 6 }, { height: 12, columns: 4 }, { height: 12, columns: 3 }, { height: 12, columns: 2 }]
      : [{}];

  for (let si = 0; si < shapes.length; si++) {
    const opts = shapes[si];
    let base: { w: number; h: number };
    try {
      const one = await bwipGray(bcid, text, 1, opts);
      base = { w: one.w, h: one.h };
    } catch {
      continue;  // esa forma no le entra al codificador (p. ej. muy pocas columnas)
    }
    // Con un módulo de 3 px (0,5 mm en esta pantalla) cualquier lector engancha:
    // no hace falta deformar ni girar el código para ganar tamaño.
    const plain = Math.min(Math.floor(maxW / base.w), Math.floor(maxH / base.h));
    if (si === 0 && plain >= 3) {
      best = { opts, scale: plain, rot: false, rank: 0 };
      break;
    }
    for (const rot of [false, true]) {
      const w1 = rot ? base.h : base.w;
      const h1 = rot ? base.w : base.h;
      const scale = Math.min(Math.floor(maxW / w1), Math.floor(maxH / h1));
      if (scale < 1) continue;
      const rank = si * 2 + (rot ? 1 : 0);  // más chico = más parecido al original
      if (!best || scale > best.scale || (scale === best.scale && rank < best.rank)) best = { opts, scale, rot, rank };
    }
  }
  if (!best) return null;

  // El ancho lo fija el módulo; el alto de las filas de un PDF417 sobra en esta
  // pantalla y una fila más alta es más fácil de leer, así que se estira hasta
  // donde entre (sin tocar el ancho del módulo, que es lo que importa).
  let chosen = best.opts;
  if (bcid === "pdf417" && !best.rot) {
    for (const height of [30, 24, 18]) {
      const opts = { ...best.opts, height };
      try {
        const probe = await bwipGray(bcid, text, 1, opts);
        if (probe.h * best.scale <= Math.min(maxH, 340) && probe.w * best.scale <= maxW) {
          chosen = opts;
          break;
        }
      } catch {}
    }
  }

  const made = await bwipGray(bcid, text, best.scale, chosen);
  let gray = made.gray;
  let w = made.w;
  let h = made.h;
  if (best.rot) {
    // Giro de 90° a mano: es exacto, no interpola nada.
    const out = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) out[x * h + (h - 1 - y)] = gray[y * w + x];
    gray = out;
    const t = w;
    w = h;
    h = t;
  }
  if (w > SCREEN_W || h > SCREEN_H) return null;
  return centerMono(gray, w, h);
}

// Plan B: el recorte de la región del código, a máxima resolución, sin
// suavizado y en blanco y negro puro. No escanea igual de bien, pero se ve.
async function cropMono(png: Uint8Array, box: { x: number; y: number; w: number; h: number }, imgW: number, imgH: number): Promise<Uint8Array> {
  const pad = Math.round(Math.max(box.w, box.h) * 0.06) + 6;
  const left = Math.max(0, Math.round(box.x - pad));
  const top = Math.max(0, Math.round(box.y - pad));
  const width = Math.min(imgW - left, Math.round(box.w + 2 * pad));
  const height = Math.min(imgH - top, Math.round(box.h + 2 * pad));
  const fit = Math.min((SCREEN_W - 20) / width, (SCREEN_H - 20) / height);
  const outW = Math.max(1, Math.min(SCREEN_W, Math.floor(width * fit)));
  const outH = Math.max(1, Math.min(SCREEN_H, Math.floor(height * fit)));
  const { data, info } = await sharp(png)
    .extract({ left, top, width, height })
    .resize(outW, outH, { kernel: "nearest" })  // nearest: sin suavizado, los módulos no se lavan
    .greyscale()
    .threshold(140)
    .raw()
    .toBuffer({ resolveWithObject: true });
  return centerMono(new Uint8Array(data.buffer, data.byteOffset, data.byteLength), info.width, info.height);
}

// ---------------------------------------------------------------- datos útiles

// Un pase de embarque IATA (BCBP, el que va adentro del PDF417) es de ancho
// fijo: se lee sin adivinar nada. Es lo más confiable de todo el adjunto.
export function parseBcbp(raw: string): { label: string; value: string }[] {
  if (!/^M[1-9]/.test(raw) || raw.length < 58) return [];
  const at = (i: number, n: number) => raw.slice(i, i + n).trim();
  const name = at(2, 20).replace(/\s+/g, " ");
  const pnr = at(23, 7);
  const from = at(30, 3);
  const to = at(33, 3);
  const carrier = at(36, 3);
  const flight = at(39, 5).replace(/^0+/, "");
  const julian = at(44, 3);
  const cabin = at(47, 1);
  const seat = at(48, 4).replace(/^0+/, "");
  const seq = at(52, 5).replace(/^0+/, "");
  // Si los campos de ancho fijo no tienen la pinta que manda la norma, esto no
  // es un BCBP y leerlo igual escupe basura ("Trayecto: ADM → AD"): mejor no
  // decir nada y quedarse con lo que se saque del texto del PDF.
  if (!/^[A-Z]{3}$/.test(from) || !/^[A-Z]{3}$/.test(to)) return [];
  if (!/^\d{1,4}[A-Z]?$/.test(flight)) return [];
  const out: { label: string; value: string }[] = [];
  if (name && /^[A-Z\/ .'-]+$/i.test(name)) out.push({ label: "Pasajero", value: name });
  if (carrier && flight) out.push({ label: "Vuelo", value: `${carrier} ${flight}` });
  if (from && to) out.push({ label: "Trayecto", value: `${from} → ${to}` });
  // La fecha viaja como día juliano sin año: se resuelve contra el año que deje
  // la fecha más cerca de hoy (un vuelo de enero comprado en diciembre).
  const day = Number(julian);
  if (day >= 1 && day <= 366) {
    const now = new Date();
    let best = "";
    let bestDiff = Infinity;
    for (const year of [now.getUTCFullYear() - 1, now.getUTCFullYear(), now.getUTCFullYear() + 1]) {
      const d = new Date(Date.UTC(year, 0, day));
      const diff = Math.abs(d.getTime() - now.getTime());
      if (diff < bestDiff) {
        bestDiff = diff;
        best = d.toISOString().slice(0, 10);
      }
    }
    out.push({ label: "Fecha", value: best });
  }
  if (/^\d{1,3}[A-Z]$/.test(seat)) out.push({ label: "Asiento", value: seat + (/^[A-Z]$/.test(cabin) ? ` (${cabin})` : "") });
  if (/^[A-Z0-9]{5,7}$/.test(pnr)) out.push({ label: "Reserva", value: pnr });
  if (/^\d{1,5}$/.test(seq)) out.push({ label: "Secuencia", value: seq });
  return out;
}

// Del texto del PDF, lo que sirve parado en la fila: vuelo, horas, puerta,
// asiento, código de reserva, dirección. Sin LLM: es plata y latencia por algo
// que resuelve una docena de expresiones regulares, y tiene que andar aunque no
// haya proveedor de IA cargado.
const PATTERNS: { label: string; re: RegExp }[] = [
  { label: "Vuelo", re: /\b(?:vuelo|flight|vol|flug|voo|рейс)\b[\s:.#-]*([A-Z0-9]{2}\s?\d{1,4})/i },
  { label: "Reserva", re: /\b(?:reserva|reservation|booking|localizador|confirmaci[oó]n|confirmation|pnr|buchung|r[ée]servation)\b[\s:.#-]*([A-Z0-9]{5,8})\b/i },
  { label: "Asiento", re: /\b(?:asiento|seat|si[eè]ge|sitzplatz|assento)\b[\s:.#-]*(\d{1,3}\s?[A-K])\b/i },
  { label: "Puerta", re: /\b(?:puerta|gate|porte|flugsteig|port[ãa]o)\b[\s:.#-]*([A-Z]?\s?\d{1,3}[A-Z]?)\b/i },
  { label: "Terminal", re: /\bterminal\b[\s:.#-]*([A-Z0-9]{1,3})\b/i },
  { label: "Embarque", re: /\b(?:embarque|boarding|embarquement|einstieg)\b[^\n]{0,20}?(\d{1,2}[:.]\d{2})/i },
  { label: "Sale", re: /\b(?:sale|salida|departure|depart|d[ée]part|abflug|partida)\b[^\n]{0,20}?(\d{1,2}[:.]\d{2})/i },
  { label: "Llega", re: /\b(?:llega|llegada|arrival|arriv[ée]e|ankunft|chegada)\b[^\n]{0,20}?(\d{1,2}[:.]\d{2})/i },
  { label: "Entrada", re: /\b(?:check[\s-]?in|entrada|ingreso|einchecken)\b[^\n]{0,24}?(\d{1,2}[:.]\d{2})/i },
  { label: "Salida del hotel", re: /\b(?:check[\s-]?out|salida del hotel)\b[^\n]{0,24}?(\d{1,2}[:.]\d{2})/i },
  { label: "Habitación", re: /\b(?:habitaci[oó]n|room|chambre|zimmer|quarto)\b[\s:.#-]*([A-Z0-9-]{1,8})\b/i },
  { label: "Tren", re: /\b(?:tren|train|zug|comboio)\b[\s:.#-]*([A-Z0-9]{2,6}\s?\d{1,4})/i },
  { label: "Coche", re: /\b(?:coche|vag[oó]n|car|voiture|wagen)\b[\s:.#-]*(\d{1,3})\b/i },
];

const ADDRESS = /^[^\n]*\b(?:calle|avenida|av\.|c\/|rua|street|st\.|road|strasse|straße|rue|plaza|paseo|address|direcci[oó]n|adresse|endere[çc]o)\b[^\n]{4,80}$/im;

export function extractFields(text: string, codeTexts: string[]): { label: string; value: string }[] {
  const out: { label: string; value: string }[] = [];
  const seen = new Set<string>();
  const push = (label: string, value: string) => {
    const v = value.replace(/\s+/g, " ").trim();
    if (!v || seen.has(label)) return;
    seen.add(label);
    out.push({ label, value: v });
  };
  for (const raw of codeTexts) for (const f of parseBcbp(raw)) push(f.label, f.value);
  const flat = text.replace(/\r/g, "");
  for (const p of PATTERNS) {
    const m = flat.match(p.re);
    if (m && m[1]) push(p.label, m[1]);
  }
  const addr = flat.match(ADDRESS);
  // Sin el prefijo de la línea, si no queda "Dirección: Direccion: Via del Corso".
  if (addr) push("Dirección", addr[0].replace(/^\s*(?:direcci[oó]n|address|adresse|endere[çc]o|dir\.)\s*[:\-]?\s*/i, ""));
  return out;
}

// ---------------------------------------------------------------- rasterizado

function pixmapToRgba(pix: mupdf.Pixmap): RawImage {
  const w = pix.getWidth();
  const h = pix.getHeight();
  const n = pix.getNumberOfComponents();
  const stride = pix.getStride();
  const px = pix.getPixels();
  const data = new Uint8ClampedArray(new ArrayBuffer(w * h * 4));
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const s = y * stride + x * n;
      const d = (y * w + x) * 4;
      if (n >= 3) {
        data[d] = px[s];
        data[d + 1] = px[s + 1];
        data[d + 2] = px[s + 2];
      } else {
        data[d] = data[d + 1] = data[d + 2] = px[s];
      }
      data[d + 3] = 255;
    }
  }
  return { data, width: w, height: h };
}

// dpi que se puede usar sin pasarse del presupuesto de píxeles.
function fitDpi(wPt: number, hPt: number, want: number): number {
  const px = (wPt / 72) * (hPt / 72) * want * want;
  if (px <= MAX_RASTER_PIXELS) return want;
  return Math.max(72, Math.floor(want * Math.sqrt(MAX_RASTER_PIXELS / px)));
}

// Caja que ocupa el contenido de la página. Un pase de embarque son cuatro
// líneas arriba y medio A4 en blanco: recortando el margen, en la pantalla el
// texto entra casi al doble de tamaño.
function contentBox(page: mupdf.Page, bounds: number[]): number[] {
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  const eat = (b: any) => {
    if (!b || b.length !== 4) return;
    x0 = Math.min(x0, b[0]);
    y0 = Math.min(y0, b[1]);
    x1 = Math.max(x1, b[2]);
    y1 = Math.max(y1, b[3]);
  };
  try {
    page.toStructuredText("preserve-whitespace,preserve-images").walk({
      beginTextBlock: (bbox: any) => eat(bbox),
      onImageBlock: (bbox: any) => eat(bbox),
    });
  } catch {
    return bounds;
  }
  if (!Number.isFinite(x0) || x1 <= x0 || y1 <= y0) return bounds;
  const m = 10;  // un margen para que no quede pegado al borde
  return [
    Math.max(bounds[0], x0 - m),
    Math.max(bounds[1], y0 - m),
    Math.min(bounds[2], x1 + m),
    Math.min(bounds[3], y1 + m),
  ];
}

// ---------------------------------------------------------------- proceso

type Built = { idx?: Uint8Array; bmp?: Uint8Array; info: PageInfo };

export type ProcessResult = { attachment: Attachment };

export async function processUpload(
  accountId: number,
  name: string,
  mime: string,
  bytes: Uint8Array,
  tripId?: string,
): Promise<ProcessResult> {
  const DIR = dirFor(accountId);
  const id = Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36).padStart(2, "0");
  const isPdf = bytes[0] === 0x25 && bytes[1] === 0x50 && bytes[2] === 0x44 && bytes[3] === 0x46;

  const codePages: { idx: Uint8Array; code: CodeInfo }[] = [];
  const docPages: Uint8Array[] = [];
  const texts: string[] = [];
  const seenCode = new Set<string>();
  let warn = "";

  // Un código leído: se vuelve a generar limpio y se verifica. Si no se puede
  // generar (formato raro), queda el recorte.
  const addCode = async (
    found: Found,
    fromPage: number,
    pngForCrop: Uint8Array | null,
    imgW: number,
    imgH: number,
  ) => {
    if (found.text && seenCode.has(found.text)) return;
    if (found.text) seenCode.add(found.text);
    let idx = found.text ? await renderCode(found.format, found.text).catch(() => null) : null;
    let copy = false;
    let verified = false;
    if (idx) {
      verified = await verifyBmpIdx(idx, found.text);
      if (!verified) idx = null;  // si no se lee lo que genera, no sirve de nada
    }
    if (!idx && pngForCrop && found.box) {
      idx = await cropMono(pngForCrop, found.box, imgW, imgH).catch(() => null);
      copy = true;
    }
    if (!idx) return;
    const code: CodeInfo = {
      format: found.format,
      text: found.text,
      page: 0,  // se completa al ordenar las páginas
      fromPage,
      verified,
      copy,
    };
    if (copy) code.warn = "Es una copia de la imagen original: puede no escanear. Lleva también el original.";
    codePages.push({ idx, code });
  };

  if (isPdf) {
    const doc = mupdf.Document.openDocument(bytes as any, "application/pdf");
    const count = Math.min(doc.countPages(), MAX_PAGES);
    if (doc.countPages() > MAX_PAGES) warn = `El PDF tiene ${doc.countPages()} páginas; se guardaron las primeras ${MAX_PAGES}.`;
    for (let i = 0; i < count; i++) {
      const page = doc.loadPage(i);
      const bounds = page.getBounds();
      try {
        texts.push(page.toStructuredText("preserve-whitespace").asText());
      } catch {}

      // 1) buscar códigos a 200 dpi y, si no aparece nada, a 400.
      let found: Found[] = [];
      let png: Uint8Array | null = null;
      let rasterW = 0;
      let rasterH = 0;
      for (const want of [200, 400]) {
        const dpi = fitDpi(bounds[2] - bounds[0], bounds[3] - bounds[1], want);
        const pix = page.toPixmap(mupdf.Matrix.scale(dpi / 72, dpi / 72), mupdf.ColorSpace.DeviceGray, false, true);
        const img = pixmapToRgba(pix);
        rasterW = img.width;
        rasterH = img.height;
        found = await decodeCodes(img).catch(() => []);
        png = pix.asPNG();
        if (found.length) break;
        if (want === 400) break;
      }
      for (const f of found) await addCode(f, i, png, rasterW, rasterH);

      // 2) si no salió ningún código pero hay imágenes grandes, la más grande
      //    puede ser un código que no se dejó leer: va como copia, avisando.
      if (!found.length && png) {
        const blocks: { x: number; y: number; w: number; h: number }[] = [];
        try {
          page.toStructuredText("preserve-whitespace,preserve-images").walk({
            onImageBlock: (bbox: any) => {
              const w = bbox[2] - bbox[0];
              const h = bbox[3] - bbox[1];
              if (w > 40 && h > 15) blocks.push({ x: bbox[0], y: bbox[1], w, h });
            },
          });
        } catch {}
        blocks.sort((a, b) => b.w * b.h - a.w * a.h);
        const big = blocks[0];
        if (big) {
          const sx = rasterW / (bounds[2] - bounds[0]);
          const sy = rasterH / (bounds[3] - bounds[1]);
          await addCode(
            { format: "desconocido", text: "", box: { x: (big.x - bounds[0]) * sx, y: (big.y - bounds[1]) * sy, w: big.w * sx, h: big.h * sy } },
            i,
            png,
            rasterW,
            rasterH,
          );
        }
      }

      // 3) la página como se ve, recortada al contenido y difuminada a 4 grises.
      const box = contentBox(page, bounds);
      const bw = box[2] - box[0];
      const bh = box[3] - box[1];
      const zoom = Math.min((SCREEN_W * 2) / bw, (SCREEN_H * 2) / bh);  // el doble y después sharp lo baja: sale más limpio
      const pixDoc = page.toPixmap(mupdf.Matrix.scale(zoom, zoom), mupdf.ColorSpace.DeviceGray, false, true);
      const full = pixDoc.asPNG();
      const left = Math.max(0, Math.round((box[0] - bounds[0]) * zoom));
      const top = Math.max(0, Math.round((box[1] - bounds[1]) * zoom));
      const width = Math.max(1, Math.min(pixDoc.getWidth() - left, Math.round(bw * zoom)));
      const height = Math.max(1, Math.min(pixDoc.getHeight() - top, Math.round(bh * zoom)));
      const cropped = await sharp(full).extract({ left, top, width, height }).png().toBuffer();
      docPages.push(await toDeviceBmp(new Uint8Array(cropped)));
    }
  } else {
    // Una foto o una captura del pase: mismo camino, una sola página. Primero
    // se aplana sobre blanco: un PNG con transparencia (el que baja media web
    // con el QR de la reserva) llega al decodificador como una mancha negra y
    // no se lee nada, y el BMP sale todo negro.
    const flat = new Uint8Array(
      await sharp(bytes).rotate().flatten({ background: { r: 255, g: 255, b: 255 } }).png().toBuffer(),
    );
    const { data, info } = await sharp(flat).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
    const img: RawImage = {
      data: new Uint8ClampedArray(data.buffer as ArrayBuffer, data.byteOffset, data.byteLength),
      width: info.width,
      height: info.height,
    };
    const found = await decodeCodes(img).catch(() => []);
    for (const f of found) await addCode(f, 0, flat, info.width, info.height);
    docPages.push(await toDeviceBmp(flat));
    if (!found.length) warn = "No se encontró ningún código en la imagen; se guardó tal cual.";
  }

  // Los códigos van primero: es lo que se busca corriendo en el aeropuerto.
  await mkdir(`${DIR}/${id}`, { recursive: true });
  const pageList: PageInfo[] = [];
  const codes: CodeInfo[] = [];
  let n = 0;
  for (const cp of codePages) {
    const bmp = packDeviceBmp(cp.idx, SCREEN_W, SCREEN_H);
    if (bmp.byteLength > MAX_PAGE_BYTES) continue;
    await writeBytesAtomic(`${DIR}/${id}/p${n}.bmp`, bmp);
    cp.code.page = n;
    codes.push(cp.code);
    pageList.push({
      n,
      kind: "code",
      label: cp.code.copy ? `Código (copia)` : `Código ${cp.code.format}`,
    });
    n++;
  }
  for (let i = 0; i < docPages.length; i++) {
    if (docPages[i].byteLength > MAX_PAGE_BYTES) continue;
    await writeBytesAtomic(`${DIR}/${id}/p${n}.bmp`, docPages[i]);
    pageList.push({ n, kind: "page", label: `Página ${i + 1}` });
    n++;
  }

  const text = texts.join("\n").replace(/\n{3,}/g, "\n\n").trim();
  const fields = extractFields(text, codes.map((c) => c.text).filter(Boolean));
  const extracted =
    (fields.map((f) => `${f.label}: ${f.value}`).join("\n") + (text ? `\n\n${text.slice(0, 4000)}` : "")).trim();
  if (codes.some((c) => c.copy)) {
    warn = [warn, "Hay un código que no se pudo leer y va como copia de la imagen: puede no escanear."].filter(Boolean).join(" ");
  }

  const att: Attachment = {
    id,
    tripId,
    name: name.slice(0, 120) || "adjunto",
    mime: mime.slice(0, 80) || (isPdf ? "application/pdf" : "image/*"),
    bytes: bytes.byteLength,
    kind: codes.length ? "code" : isPdf ? "pdf" : "image",
    pages: pageList.length,
    pageList,
    extracted,
    fields,
    codes,
    warn,
    at: new Date().toISOString(),
  };

  // El tope se comprueba DENTRO del candado: dos subidas a la vez veían las
  // dos el mismo "hay lugar para uno" y entraban las dos.
  const full = await mutateIndex(accountId, (idx) => {
    if (idx.items.length >= MAX_ATTACHMENTS) return true;
    idx.items.unshift(att);
    return false;
  });
  if (full) {
    await rm(`${DIR}/${id}`, { recursive: true, force: true }).catch(() => {});
    throw new Error(`ya hay ${MAX_ATTACHMENTS} adjuntos guardados: borra alguno antes de subir otro`);
  }
  return { attachment: att };
}

// ---------------------------------------------------------------- rutas

export const attachmentApi = new Hono<AppEnv>();

// El bitmap listo para pintar. Mismo formato que /api/photos/file: BMP de 2 bpp
// de 480x800, que el aparato dibuja con el pipeline de grises que ya tiene.
attachmentApi.get("/", async (c) => {
  const id = safeId(c.req.query("id"));
  const page = Math.max(0, Math.min(MAX_PAGES * 2, Number(c.req.query("page") ?? 0) || 0));
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  try {
    const bytes = await readFile(`${dirFor(accountOf(c))}/${id}/p${page}.bmp`);
    return new Response(new Uint8Array(bytes), {
      headers: { "Content-Type": "image/bmp", "Content-Length": String(bytes.length) },
    });
  } catch {
    return c.json({ ok: false, error: "not found" }, 404);
  }
});

// Una página en PNG, para verla en el teléfono tal como la va a pintar el aparato.
attachmentApi.get("/preview", async (c) => {
  const id = safeId(c.req.query("id"));
  const page = Math.max(0, Math.min(MAX_PAGES * 2, Number(c.req.query("page") ?? 0) || 0));
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  try {
    const bmp = new Uint8Array(await readFile(`${dirFor(accountOf(c))}/${id}/p${page}.bmp`));
    const png = await bmpToPng(bmp);
    return new Response(new Uint8Array(png), { headers: { "Content-Type": "image/png", "Cache-Control": "private, max-age=86400" } });
  } catch {
    return c.json({ ok: false, error: "not found" }, 404);
  }
});

attachmentApi.get("/info", async (c) => {
  const id = safeId(c.req.query("id"));
  const att = id ? await getAttachment(accountOf(c), id) : null;
  if (!att) return c.json({ ok: false, error: "not found" }, 404);
  // `text` y `pages` van también en la raíz: es lo que lee el aparato, que
  // guarda esta misma respuesta al lado del bitmap para poder mostrarla sin WiFi.
  return c.json({ ok: true, text: att.extracted, pages: att.pages, attachment: att });
});

attachmentApi.get("/list", async (c) => {
  const tripId = (c.req.query("trip") ?? "").toString();
  const all = await listAttachments(accountOf(c));
  return c.json({ ok: true, attachments: tripId ? all.filter((a) => a.tripId === tripId) : all });
});

attachmentApi.post("/delete", async (c) => {
  const b = await readBody(c);
  const ok = await deleteAttachment(accountOf(c), safeId(b.id));
  return ok ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
});

// Subida desde /board: llega el archivo tal cual (multipart o el cuerpo crudo).
// Se procesa una sola vez y se devuelve qué se encontró, para que el usuario lo
// vea en el teléfono antes de salir de casa.
export const boardAttachment = new Hono<AppEnv>();

boardAttachment.post("/", async (c) => {
  let name = (c.req.query("name") ?? "").toString().slice(0, 120);
  const tripId = (c.req.query("trip") ?? "").toString().replace(/[^a-z0-9]/gi, "").slice(0, 24) || undefined;
  let mime = c.req.header("content-type") ?? "";
  let bytes: Uint8Array;

  if (mime.startsWith("multipart/form-data")) {
    const form = await c.req.formData();
    const file = form.get("file");
    if (!(file instanceof File)) return c.json({ ok: false, error: "falta el archivo" }, 400);
    bytes = new Uint8Array(await file.arrayBuffer());
    name = name || file.name;
    mime = file.type || "application/octet-stream";
  } else {
    bytes = new Uint8Array(await c.req.arrayBuffer());
  }

  if (bytes.byteLength < 100) return c.json({ ok: false, error: "el archivo está vacío" }, 400);
  if (bytes.byteLength > MAX_UPLOAD_BYTES) {
    return c.json({ ok: false, error: `el archivo pesa más de ${Math.round(MAX_UPLOAD_BYTES / 1048576)} MB` }, 400);
  }

  try {
    const t0 = Date.now();
    const { attachment } = await processUpload(accountOf(c), name || "adjunto", mime, bytes, tripId);
    console.log(
      `adjunto: ${attachment.name} ${bytes.byteLength} B -> ${attachment.pages} páginas, ` +
        `${attachment.codes.length} código(s) en ${Date.now() - t0} ms`,
    );
    return c.json({ ok: true, attachment });
  } catch (err) {
    console.error("adjunto:", err);
    const raw = String(err instanceof Error ? err.message : err);
    // Lo que más pasa: mandar algo que no es ni PDF ni imagen (un .docx, un
    // .eml, un PDF cortado por la mitad). El mensaje crudo de sharp o de mupdf
    // no le dice nada a nadie.
    const friendly = /unsupported image format|no objects found|cannot recognize|not a PDF/i.test(raw)
      ? "el archivo no es un PDF ni una imagen que se pueda leer (si es un correo, guarda el adjunto y sube ese)"
      : `no se pudo procesar (${raw.slice(0, 160)})`;
    return c.json({ ok: false, error: friendly }, 400);
  }
});

// Cuánto ocupa todo en el volumen (se muestra en /board).
export async function attachmentsUsage(accountId: number): Promise<{ count: number; bytes: number }> {
  const idx = await loadIndex(accountId);
  let bytes = 0;
  for (const a of idx.items) {
    try {
      const files = await readdir(`${dirFor(accountId)}/${a.id}`);
      bytes += files.length * 96_070;  // cada página es exactamente ese tamaño
    } catch {}
  }
  return { count: idx.items.length, bytes };
}
