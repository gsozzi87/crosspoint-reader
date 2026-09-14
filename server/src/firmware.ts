// OTA del firmware. El manifiesto es el punto de commit: cada versión se
// guarda en su propio archivo y version.json pasa a señalarla solo cuando
// ambos están completos.
import { randomUUID } from "node:crypto";
import { mkdir, rename, rm, stat, writeFile } from "node:fs/promises";
import { Hono } from "hono";
import { limitBody } from "./net";
import { readJsonSafe, serialize, writeAtomicNow } from "./fsjson";

const TOKEN = process.env.OTA_TOKEN ?? "";
const DIR = process.env.FIRMWARE_DIR ?? "/data/firmware";
const ASSET = "firmware-ws397.bin";
const LEGACY_BIN = `${DIR}/${ASSET}`;
const META = `${DIR}/version.json`;
const PUBLIC_BASE = (process.env.PUBLIC_BASE_URL ?? "").trim().replace(/\/+$/, "");

type Meta = { version: string; size: number; uploadedAt: string; file?: string };

async function meta(): Promise<Meta | null> {
  const m = await readJsonSafe<Partial<Meta> | null>(META, null);
  if (!m || typeof m.version !== "string") return null;
  const file = typeof m.file === "string" && /^firmware-ws397\.bin\.\d+\.\d+\.\d+$/.test(m.file) ? m.file : undefined;
  return { version: m.version, size: Number(m.size) || 0, uploadedAt: String(m.uploadedAt ?? ""), file };
}

function binaryPath(m: Meta): string {
  return m.file ? `${DIR}/${m.file}` : LEGACY_BIN;
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
    const m = await meta();
    if (!m) return c.json({ error: "no firmware" }, 404);
    const path = binaryPath(m);
    const s = await stat(path);
    return new Response(Bun.file(path), {
      headers: { "Content-Type": "application/octet-stream", "Content-Length": String(s.size) },
    });
  } catch {
    return c.json({ error: "no firmware" }, 404);
  }
});

firmware.put("/", limitBody(32 * 1024 * 1024), async (c) => {
  const auth = c.req.header("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
  if (!TOKEN || token !== TOKEN) return c.json({ ok: false, error: "unauthorized" }, 401);
  const version = (c.req.header("x-version") ?? "").trim();
  if (!/^\d+\.\d+\.\d+$/.test(version)) return c.json({ ok: false, error: "X-Version must be major.minor.patch" }, 400);
  const body = new Uint8Array(await c.req.arrayBuffer());
  if (body.byteLength < 100_000) return c.json({ ok: false, error: "binary too small" }, 400);

  const m: Meta = {
    version,
    size: body.byteLength,
    uploadedAt: new Date().toISOString(),
    file: `${ASSET}.${version}`,
  };

  await serialize(META, async () => {
    await mkdir(DIR, { recursive: true });
    const tmp = `${DIR}/.${ASSET}.${randomUUID()}.tmp`;
    try {
      await writeFile(tmp, body);
      await rename(tmp, binaryPath(m));
      await writeAtomicNow(META, JSON.stringify(m, null, 2));
    } finally {
      await rm(tmp, { force: true }).catch(() => {});
    }
  });

  console.log(`firmware ${version} uploaded (${m.size} bytes)`);
  return c.json({ ok: true, version, size: m.size });
});
