// Álbum de fotos. La conversión pesada (escalar, pasar a grises, difuminar a
// 4 niveles y armar el BMP) la hace el navegador del teléfono en /board, así
// que acá no hay dependencias nativas ni CPU: el servidor solo guarda el BMP
// de 2 bpp (96 KB para 800x480) y se lo sirve al aparato, que lo baja a la SD
// y lo dibuja con el lector de BMP que ya tiene.
//
//   POST /api/board/photo?name=...   (body: image/bmp)  -> { ok, id }
//   GET  /api/photos                 -> { ok, photos: [{ id, name, size, at }] }
//   GET  /api/photos/file?id=...     -> image/bmp
//   POST /api/photos/delete {id}     -> { ok }
import { Hono } from "hono";
import { mkdir, readdir, readFile, stat, unlink } from "node:fs/promises";
import sharp from "sharp";
import { photosDir, writeBytesAtomic, writeTextAtomic } from "./fsjson";
import { readBody } from "./net";
import { accountOf, type AppEnv } from "./tenant";

// Las fotos son archivos: cada cuenta tiene su directorio (la cuenta 1, la que
// ya venía andando, se queda en /data/photos).
const MAX_BYTES = 400_000;
const MAX_PHOTOS = 60;

type Photo = { id: string; name: string; size: number; at: string };

async function index(accountId: number): Promise<Photo[]> {
  const DIR = photosDir(accountId);
  try {
    const files = await readdir(DIR);
    const out: Photo[] = [];
    for (const f of files) {
      if (!f.endsWith(".bmp")) continue;
      const s = await stat(`${DIR}/${f}`);
      const id = f.slice(0, -4);
      let name = id;
      try {
        name = (await readFile(`${DIR}/${id}.txt`, "utf8")).trim() || id;
      } catch {}
      out.push({ id, name, size: s.size, at: s.mtime.toISOString() });
    }
    out.sort((a, b) => b.at.localeCompare(a.at));
    return out;
  } catch {
    return [];
  }
}

// La pantalla del aparato: 480x800 y cuatro grises (0, 85, 170, 255).
const SCREEN_W = 480;
const SCREEN_H = 800;
const LEVELS = [0, 85, 170, 255];

// Foto de teléfono -> BMP de 2 bpp listo para el panel. Lo hace el servidor (antes
// lo hacía el navegador con un canvas): acá se puede rotar por EXIF, escalar bien
// y difuminar con Floyd-Steinberg sin depender del teléfono que suba la foto.
export async function toDeviceBmp(input: Uint8Array): Promise<Uint8Array> {
  const { data, info } = await sharp(input)
    .rotate()  // respeta la orientación EXIF: si no, las verticales salen acostadas
    .resize(SCREEN_W, SCREEN_H, { fit: "contain", background: { r: 255, g: 255, b: 255 } })
    .greyscale()
    .raw()
    .toBuffer({ resolveWithObject: true });
  const w = info.width;
  const h = info.height;

  // Difuminado a 4 niveles (el error se reparte a los vecinos: sin esto una foto
  // queda en cuatro manchas planas).
  const gray = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) gray[i] = data[i];
  const idx = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const p = y * w + x;
      const old = gray[p];
      const q = Math.max(0, Math.min(3, Math.round(old / 85)));
      idx[p] = q;
      const err = old - LEVELS[q];
      if (x + 1 < w) gray[p + 1] += (err * 7) / 16;
      if (y + 1 < h) {
        if (x > 0) gray[p + w - 1] += (err * 3) / 16;
        gray[p + w] += (err * 5) / 16;
        if (x + 1 < w) gray[p + w + 1] += (err * 1) / 16;
      }
    }
  }

  const rowBytes = Math.ceil((w * 2) / 32) * 4;  // 2 bpp, filas alineadas a 4
  const off = 14 + 40 + 4 * 4;                   // cabeceras + paleta de 4 colores
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

// El BMP de 2 bpp del aparato -> PNG para verlo en el teléfono. Es el inverso
// exacto de toDeviceBmp: paleta de 4 grises, filas de abajo hacia arriba,
// alineadas a 4 bytes. Sirve también para las páginas de los adjuntos, que
// tienen el mismo formato.
export async function bmpToPng(bmp: Uint8Array): Promise<Buffer> {
  if (bmp.length < 70 || bmp[0] !== 0x42 || bmp[1] !== 0x4d) throw new Error("no es un BMP");
  const dv = new DataView(bmp.buffer, bmp.byteOffset, bmp.byteLength);
  const off = dv.getUint32(10, true);
  const w = dv.getInt32(18, true);
  const hRaw = dv.getInt32(22, true);
  const bpp = dv.getUint16(28, true);
  const h = Math.abs(hRaw);
  if (bpp !== 2 || w <= 0 || w > 4096 || h <= 0 || h > 4096) throw new Error("BMP raro");
  const palette: number[] = [];
  for (let i = 0; i < 4; i++) palette.push(bmp[54 + i * 4 + 1] ?? LEVELS[i]);  // el verde, da igual
  const rowBytes = Math.ceil((w * 2) / 32) * 4;
  const gray = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    const row = off + (hRaw > 0 ? h - 1 - y : y) * rowBytes;
    for (let x = 0; x < w; x++) {
      const byte = bmp[row + (x >> 2)] ?? 0;
      gray[y * w + x] = palette[(byte >> (6 - 2 * (x % 4))) & 3];
    }
  }
  return sharp(Buffer.from(gray), { raw: { width: w, height: h, channels: 1 } }).png().toBuffer();
}

export async function savePhoto(accountId: number, name: string, bytes: Uint8Array): Promise<string> {
  const DIR = photosDir(accountId);
  await mkdir(DIR, { recursive: true });
  const id = Date.now().toString(36);
  // .tmp + rename: si se corta a la mitad, el aparato bajaba un BMP truncado y
  // dibujaba basura.
  await writeBytesAtomic(`${DIR}/${id}.bmp`, bytes);
  await writeTextAtomic(`${DIR}/${id}.txt`, name.slice(0, 80));
  // Keep the album bounded: drop the oldest beyond MAX_PHOTOS.
  const all = await index(accountId);
  for (const old of all.slice(MAX_PHOTOS)) {
    await unlink(`${DIR}/${old.id}.bmp`).catch(() => {});
    await unlink(`${DIR}/${old.id}.txt`).catch(() => {});
  }
  return id;
}

export const photos = new Hono<AppEnv>();

photos.get("/", async (c) => c.json({ ok: true, photos: await index(accountOf(c)) }));

photos.get("/file", async (c) => {
  // El id se limpia a [a-z0-9]: no hay forma de que se escape del directorio
  // de la cuenta con "..", una barra o una ruta absoluta.
  const id = (c.req.query("id") ?? "").replace(/[^a-z0-9]/gi, "");
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  try {
    const bytes = await readFile(`${photosDir(accountOf(c))}/${id}.bmp`);
    return new Response(bytes, { headers: { "Content-Type": "image/bmp", "Content-Length": String(bytes.length) } });
  } catch {
    return c.json({ ok: false, error: "not found" }, 404);
  }
});

// La foto como la va a ver el aparato, en PNG para el navegador.
photos.get("/preview", async (c) => {
  const id = (c.req.query("id") ?? "").replace(/[^a-z0-9]/gi, "");
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  try {
    const bmp = new Uint8Array(await readFile(`${photosDir(accountOf(c))}/${id}.bmp`));
    const png = await bmpToPng(bmp);
    return new Response(new Uint8Array(png), { headers: { "Content-Type": "image/png", "Cache-Control": "private, max-age=86400" } });
  } catch {
    return c.json({ ok: false, error: "not found" }, 404);
  }
});

photos.post("/delete", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString().replace(/[^a-z0-9]/gi, "");
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  const DIR = photosDir(accountOf(c));
  await unlink(`${DIR}/${id}.bmp`).catch(() => {});
  await unlink(`${DIR}/${id}.txt`).catch(() => {});
  return c.json({ ok: true });
});

export const MAX_PHOTO_BYTES = MAX_BYTES;
// Lo que sube el teléfono antes de convertir: una foto de 12 MP entra holgada.
export const MAX_UPLOAD_BYTES = 12 * 1024 * 1024;
