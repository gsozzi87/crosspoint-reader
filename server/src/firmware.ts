// OTA del firmware. El aparato consulta /firmware/latest (misma forma que un
// release de GitHub, que es lo que entiende el OtaUpdater de CrossPoint) y baja
// el asset firmware-ws397.bin. release.sh / release.ps1 suben el binario con
// PUT /firmware (Bearer OTA_TOKEN, header X-Version). Todo vive en el volumen.
import { Hono } from "hono";
import { mkdir, rename, stat, writeFile } from "node:fs/promises";
import { readJsonSafe, writeJsonAtomic } from "./fsjson";

const TOKEN = process.env.OTA_TOKEN ?? "";
const DIR = process.env.FIRMWARE_DIR ?? "/data/firmware";
const ASSET = "firmware-ws397.bin";
const BIN = `${DIR}/${ASSET}`;
const META = `${DIR}/version.json`;
// De dónde baja el aparato el .bin. Fijo por configuración: /firmware/latest es
// público y armar la URL con el Host del cliente deja que cualquiera le sirva
// al aparato un binario desde otro lado.
const PUBLIC_BASE = (process.env.PUBLIC_BASE_URL ?? "").trim().replace(/\/+$/, "");

type Meta = { version: string; size: number; uploadedAt: string };

async function meta(): Promise<Meta | null> {
  const m = await readJsonSafe<Partial<Meta> | null>(META, null);
  return m && typeof m.version === "string" ? { version: m.version, size: Number(m.size) || 0, uploadedAt: String(m.uploadedAt ?? "") } : null;
}

function origin(c: { req: { header: (n: string) => string | undefined; url: string } }): string {
  if (PUBLIC_BASE) return PUBLIC_BASE;
  const proto = c.req.header("x-forwarded-proto") ?? new URL(c.req.url).protocol.replace(":", "");
  const host = c.req.header("x-forwarded-host") ?? c.req.header("host") ?? new URL(c.req.url).host;
  return `${proto}://${host}`;
}

export const firmware = new Hono();

firmware.get("/latest", async (c) => {
  const m = await meta();
  if (!m) return c.json({ error: "no firmware uploaded yet" }, 404);
  return c.json({
    tag_name: m.version,
    name: `ws397 ${m.version}`,
    published_at: m.uploadedAt,
    assets: [{ name: ASSET, browser_download_url: `${origin(c)}/firmware/${ASSET}`, size: m.size }],
  });
});

firmware.get(`/${ASSET}`, async (c) => {
  try {
    const s = await stat(BIN);
    const file = Bun.file(BIN);
    return new Response(file, {
      headers: { "Content-Type": "application/octet-stream", "Content-Length": String(s.size) },
    });
  } catch {
    return c.json({ error: "no firmware" }, 404);
  }
});

firmware.put("/", async (c) => {
  const auth = c.req.header("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
  if (!TOKEN || token !== TOKEN) return c.json({ ok: false, error: "unauthorized" }, 401);
  const version = (c.req.header("x-version") ?? "").trim();
  if (!/^\d+\.\d+\.\d+$/.test(version)) return c.json({ ok: false, error: "X-Version must be major.minor.patch" }, 400);
  const body = new Uint8Array(await c.req.arrayBuffer());
  if (body.byteLength < 100_000) return c.json({ ok: false, error: "binary too small" }, 400);
  await mkdir(DIR, { recursive: true });
  await writeFile(`${BIN}.tmp`, body);
  await rename(`${BIN}.tmp`, BIN);
  const m: Meta = { version, size: body.byteLength, uploadedAt: new Date().toISOString() };
  await writeJsonAtomic(META, m);  // el .bin ya iba con .tmp+rename; el version.json quedaba a medias
  console.log(`firmware ${version} uploaded (${m.size} bytes)`);
  return c.json({ ok: true, version, size: m.size });
});
