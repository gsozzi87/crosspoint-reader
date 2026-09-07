// Log del aparato. En cada sincronización sube el final de su log de la SD
// (`POST /api/log`, texto plano) y acá queda guardado con la fecha; se lee en
// el navegador desde /board/log, sin cable ni monitor serie.
import { Hono } from "hono";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const FILE = process.env.DEVICE_LOG_FILE ?? "/data/device.log";
const MAX_BYTES = 512 * 1024;

export const deviceLog = new Hono();

// GET /api/log (con el Bearer del aparato): el texto del log, para la página.
deviceLog.get("/", async (c) => {
  let text = "";
  try {
    text = await readFile(FILE, "utf8");
  } catch {
    text = "(todavía no subió ningún log; sincronizá el hub)";
  }
  return c.text(text.slice(-200_000));
});

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

// La página en sí no lleva datos: pide el log con el token guardado en el
// navegador (el mismo de /board). Antes servía el log entero sin token y ahí
// están los nombres de las redes WiFi de la casa y todo lo que se dictó por voz.
export const logPage = new Hono();

logPage.get("/", (c) =>
  c.html(
    `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Log del aparato</title><style>body{margin:0;background:#111;color:#ddd;font:12px/1.45 ui-monospace,Menlo,Consolas,monospace}
header{position:sticky;top:0;background:#000;padding:8px 12px;display:flex;gap:12px;align-items:center}
a{color:#7cf}pre{margin:0;padding:12px;white-space:pre-wrap;word-break:break-word}
input,button{font:inherit;padding:6px 8px;border-radius:6px;border:1px solid #555;background:#222;color:#ddd}</style></head>
<body><header><strong>Log del aparato</strong><span id="size"></span><a href="/board">&larr; Pizarra</a><a href="javascript:location.reload()">Actualizar</a></header>
<pre id="l"></pre>
<script>
const token = localStorage.getItem("deviceToken") || "";
async function load() {
  if (!token) { ask(); return; }
  const r = await fetch("/api/log", { headers: { Authorization: "Bearer " + token } });
  if (r.status === 401) { localStorage.removeItem("deviceToken"); ask(); return; }
  const t = await r.text();
  document.getElementById("l").textContent = t;
  document.getElementById("size").textContent = Math.round(t.length / 1024) + " KB";
  window.scrollTo(0, document.body.scrollHeight);
}
function ask() {
  document.getElementById("l").innerHTML =
    "<p>Token del aparato:</p><p><input id='t' style='width:60%'> <button id='go'>Entrar</button></p>";
  document.getElementById("go").addEventListener("click", () => {
    const v = document.getElementById("t").value.trim();
    if (!v) return;
    localStorage.setItem("deviceToken", v);
    location.reload();
  });
}
load();
</script></body></html>`,
  ),
);
