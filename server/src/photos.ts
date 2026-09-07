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
import { mkdir, readdir, readFile, stat, unlink, writeFile } from "node:fs/promises";
import sharp from "sharp";

const DIR = process.env.PHOTOS_DIR ?? "/data/photos";
const MAX_BYTES = 400_000;
const MAX_PHOTOS = 60;

type Photo = { id: string; name: string; size: number; at: string };

async function index(): Promise<Photo[]> {
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

export async function savePhoto(name: string, bytes: Uint8Array): Promise<string> {
  await mkdir(DIR, { recursive: true });
  const id = Date.now().toString(36);
  await writeFile(`${DIR}/${id}.bmp`, bytes);
  await writeFile(`${DIR}/${id}.txt`, name.slice(0, 80));
  // Keep the album bounded: drop the oldest beyond MAX_PHOTOS.
  const all = await index();
  for (const old of all.slice(MAX_PHOTOS)) {
    await unlink(`${DIR}/${old.id}.bmp`).catch(() => {});
    await unlink(`${DIR}/${old.id}.txt`).catch(() => {});
  }
  return id;
}

export const photos = new Hono();

photos.get("/", async (c) => c.json({ ok: true, photos: await index() }));

photos.get("/file", async (c) => {
  const id = (c.req.query("id") ?? "").replace(/[^a-z0-9]/gi, "");
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  try {
    const bytes = await readFile(`${DIR}/${id}.bmp`);
    return new Response(bytes, { headers: { "Content-Type": "image/bmp", "Content-Length": String(bytes.length) } });
  } catch {
    return c.json({ ok: false, error: "not found" }, 404);
  }
});

photos.post("/delete", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const id = (b.id ?? "").toString().replace(/[^a-z0-9]/gi, "");
  if (!id) return c.json({ ok: false, error: "id required" }, 400);
  await unlink(`${DIR}/${id}.bmp`).catch(() => {});
  await unlink(`${DIR}/${id}.txt`).catch(() => {});
  return c.json({ ok: true });
});

export const MAX_PHOTO_BYTES = MAX_BYTES;
// Lo que sube el teléfono antes de convertir: una foto de 12 MP entra holgada.
export const MAX_UPLOAD_BYTES = 12 * 1024 * 1024;
