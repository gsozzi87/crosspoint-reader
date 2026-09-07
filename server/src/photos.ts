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
