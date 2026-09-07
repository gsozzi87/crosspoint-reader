// Log del aparato. En cada sincronización sube el final de su log de la SD
// (`POST /api/log`, texto plano) y acá queda guardado con la fecha; se lee en
// el navegador desde /board/log, sin cable ni monitor serie.
import { Hono } from "hono";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const FILE = process.env.DEVICE_LOG_FILE ?? "/data/device.log";
const MAX_BYTES = 512 * 1024;

export const deviceLog = new Hono();

deviceLog.post("/", async (c) => {
  const text = await c.req.text();
  if (!text.trim()) return c.json({ ok: false, error: "empty" }, 400);
  let previous = "";
  try {
    previous = await readFile(FILE, "utf8");
  } catch {}
  const stamp = `\n===== ${new Date().toISOString()} =====\n`;
  let out = previous + stamp + text.slice(-MAX_BYTES);
  if (out.length > MAX_BYTES) out = out.slice(-MAX_BYTES);
  await mkdir(dirname(FILE), { recursive: true });
  await writeFile(FILE, out);
  console.log(`device log: +${text.length} bytes`);
  return c.json({ ok: true, size: out.length });
});

// Sin token: es solo texto de diagnóstico y así se abre de una desde el
// teléfono. Si algún día molesta, se le agrega el Bearer como al resto.
export const logPage = new Hono();

logPage.get("/", async (c) => {
  let text = "";
  try {
    text = await readFile(FILE, "utf8");
  } catch {
    text = "(todavía no subió ningún log; sincronizá el hub)";
  }
  const tail = text.slice(-200_000);
  return c.html(
    `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Log del aparato</title><style>body{margin:0;background:#111;color:#ddd;font:12px/1.45 ui-monospace,Menlo,Consolas,monospace}
header{position:sticky;top:0;background:#000;padding:8px 12px;display:flex;gap:12px;align-items:center}
a{color:#7cf}pre{margin:0;padding:12px;white-space:pre-wrap;word-break:break-word}</style></head>
<body><header><strong>Log del aparato</strong><span>${(tail.length / 1024).toFixed(0)} KB</span><a href="/board">← Pizarra</a><a href="javascript:location.reload()">Actualizar</a></header>
<pre id="l"></pre><script>document.getElementById('l').textContent=${JSON.stringify(tail)};window.scrollTo(0,document.body.scrollHeight);</script></body></html>`,
  );
});
