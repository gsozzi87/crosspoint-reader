// Log del aparato. En cada sincronización sube el final de su log de la SD
// (`POST /api/log`, texto plano) y acá queda guardado con la fecha; se lee en
// el navegador desde /board/log, sin cable ni monitor serie.
import { Hono } from "hono";
import { readFile } from "node:fs/promises";
import { deviceLogFile, serialize, writeAtomicNow } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";

// El log es POR CUENTA y nunca se mezcla: adentro están los nombres de las
// redes WiFi de la casa y todo lo que se dicta por voz.
const MAX_BYTES = 512 * 1024;

export const deviceLog = new Hono<AppEnv>();

// GET /api/log (con el Bearer del aparato): el texto del log, para la página.
deviceLog.get("/", async (c) => {
  let text = "";
  try {
    text = await readFile(deviceLogFile(accountOf(c)), "utf8");
  } catch {
    text = "(todavía no subió ningún log; sincronizá el hub)";
  }
  return c.text(text.slice(-200_000));
});

deviceLog.post("/", async (c) => {
  const text = await c.req.text();
  if (!text.trim()) return c.json({ ok: false, error: "empty" }, 400);
  // Leer y escribir dentro de la misma cola: dos subidas juntas leían las dos
  // el archivo viejo y la segunda se comía el log de la primera.
  const FILE = deviceLogFile(accountOf(c));
  const size = await serialize(FILE, async () => {
    let previous = "";
    try {
      previous = await readFile(FILE, "utf8");
    } catch {}
    const stamp = `\n===== ${new Date().toISOString()} =====\n`;
    let out = previous + stamp + text.slice(-MAX_BYTES);
    if (out.length > MAX_BYTES) out = out.slice(-MAX_BYTES);
    await writeAtomicNow(FILE, out);
    return out.length;
  });
  console.log(`device log: +${text.length} bytes`);
  return c.json({ ok: true, size });
});

// DELETE /api/log: vaciar. Hasta 1.5.49 no había forma de borrarlo, así que el
// log de hace tres días seguía arriba de todo y para leer lo de recién había que
// bajar media pantalla. Borrar es lo que permite "vacío el log, reproduzco el
// problema, miro" — que es la única forma cómoda de diagnosticar sin cable.
deviceLog.delete("/", async (c) => {
  const FILE = deviceLogFile(accountOf(c));
  await serialize(FILE, async () => {
    await writeAtomicNow(FILE, "");
    return 0;
  });
  return c.json({ ok: true });
});

// La página en sí no lleva datos: pide el log con la sesión de /board (modo
// multiusuario) o con el token guardado en el navegador (como siempre). Antes
// servía el log entero sin token y ahí están los nombres de las redes WiFi de la
// casa y todo lo que se dictó por voz.
export const logPage = new Hono<AppEnv>();

logPage.get("/", (c) =>
  c.html(
    `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Log del aparato</title><style>body{margin:0;background:#111;color:#ddd;font:12px/1.45 ui-monospace,Menlo,Consolas,monospace}
header{position:sticky;top:0;background:#000;padding:8px 12px;display:flex;gap:12px;align-items:center}
a{color:#7cf}pre{margin:0;padding:12px;white-space:pre-wrap;word-break:break-word}
input,button{font:inherit;padding:6px 8px;border-radius:6px;border:1px solid #555;background:#222;color:#ddd}</style></head>
<body><header><strong>Log del aparato</strong><span id="size"></span><a href="/board">&larr; Pizarra</a>
<a href="javascript:location.reload()">Actualizar</a><button id="copy">Copiar</button><button id="clear">Vaciar</button></header>
<pre id="l"></pre>
<script>
const token = localStorage.getItem("deviceToken") || "";
async function load() {
  // Primero la cookie de sesión (modo multiusuario); si el servidor no tiene
  // cuentas, el token guardado del aparato.
  const headers = token ? { Authorization: "Bearer " + token } : {};
  const r = await fetch("/api/log", { headers: headers, credentials: "same-origin" });
  if (r.status === 401) { ask(); return; }
  const t = await r.text();
  document.getElementById("l").textContent = t;
  document.getElementById("size").textContent = Math.round(t.length / 1024) + " KB";
  window.scrollTo(0, document.body.scrollHeight);
}
async function ask() {
  let multi = false;
  try { multi = (await (await fetch("/auth/me", { credentials: "same-origin" })).json()).multi === true; } catch (e) {}
  if (multi) {
    document.getElementById("l").innerHTML =
      "<p>Hay que entrar con tu cuenta: <a href='/board'>ir a la Pizarra</a>.</p>";
    return;
  }
  document.getElementById("l").innerHTML =
    "<p>Token del aparato:</p><p><input id='t' style='width:60%'> <button id='go'>Entrar</button></p>";
  document.getElementById("go").addEventListener("click", () => {
    const v = document.getElementById("t").value.trim();
    if (!v) return;
    localStorage.setItem("deviceToken", v);
    location.reload();
  });
}
// Copiar: el navegador no deja usar el portapapeles sin un gesto del usuario, y
// clipboard.writeText no existe fuera de HTTPS, así que hay un camino de
// respaldo con un textarea y execCommand para cuando falta.
document.getElementById("copy").addEventListener("click", async () => {
  const t = document.getElementById("l").textContent || "";
  const boton = document.getElementById("copy");
  try {
    await navigator.clipboard.writeText(t);
  } catch (e) {
    const a = document.createElement("textarea");
    a.value = t;
    document.body.appendChild(a);
    a.select();
    try { document.execCommand("copy"); } catch (e2) {}
    document.body.removeChild(a);
  }
  boton.textContent = "Copiado";
  setTimeout(() => { boton.textContent = "Copiar"; }, 1500);
});

document.getElementById("clear").addEventListener("click", async () => {
  if (!confirm("¿Vaciar el log del aparato?")) return;
  const headers = token ? { Authorization: "Bearer " + token } : {};
  await fetch("/api/log", { method: "DELETE", headers: headers, credentials: "same-origin" });
  load();
});

load();
</script></body></html>`,
  ),
);
