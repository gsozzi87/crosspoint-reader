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
// Cuánto se guarda. El aparato manda sólo lo nuevo desde 1.5.92, así que esto
// es historia de verdad y no la misma tanda repetida — pero historia de hace
// una semana, con tres versiones de firmware adentro, no sirve para diagnosticar
// lo de anoche: tapa lo que importa. Un día es lo que se pide ("no más de 24 hs").
const KEEP_MS = 24 * 60 * 60 * 1000;

const STAMP = /^===== (\d{4}-\d{2}-\d{2}T[^ ]+) =====$/;

// Poda por los sellos que escribe cada subida. Todo lo que está DEBAJO de un
// sello pertenece a esa subida, así que se corta en el primer sello que entra en
// la ventana y se tira lo de arriba.
//
// DOS CASOS BORDE QUE IMPORTAN, los dos del lado de no dejar al usuario sin nada:
//  - ningún sello cae en la ventana (el aparato no sincroniza hace tres días):
//    se deja LA ÚLTIMA subida igual. Ahí la página muestra algo viejo, sí, pero
//    "viejo" es un diagnóstico y una pantalla vacía no es ninguno;
//  - no hay sellos (formato inesperado, o el archivo se escribió a mano): se
//    devuelve tal cual. Borrar a ciegas el log del usuario es peor que guardarlo
//    de más.
export function prune(text: string, nowMs: number, keepMs = KEEP_MS): string {
  const lines = text.split("\n");
  let cut = -1;
  let last = -1;
  for (let i = 0; i < lines.length; i++) {
    const m = STAMP.exec(lines[i]);
    if (!m) continue;
    const t = Date.parse(m[1]);
    if (!Number.isFinite(t)) continue;
    last = i;
    if (cut < 0 && nowMs - t <= keepMs) cut = i;  // el primero que entra: de acá abajo se conserva
  }
  if (cut < 0) cut = last;   // nada dentro de la ventana: queda la última subida
  if (cut <= 0) return text;  // sin sellos, o ya empieza dentro de la ventana
  return lines.slice(cut).join("\n");
}

export const deviceLog = new Hono<AppEnv>();

// Lo que la web muestra en la tarjeta del aparato sin bajar el log entero:
// cuándo subió por última vez, qué versión de firmware corre (la del último
// "=== 1.5.x-ws397 | arranque por ... ===" que se ve) y por qué arrancó.
export async function logMeta(accountId: number): Promise<{ at: string; bytes: number; firmware: string; wake: string }> {
  let text = "";
  try {
    text = await readFile(deviceLogFile(accountId), "utf8");
  } catch {
    return { at: "", bytes: 0, firmware: "", wake: "" };
  }
  text = prune(text, Date.now());
  const stamps = text.match(/===== (\d{4}-\d{2}-\d{2}T[^ ]+) =====/g) ?? [];
  const at = stamps.length ? stamps[stamps.length - 1].slice(6, -6) : "";
  const boots = [...text.matchAll(/=== (\d+\.\d+\.\d+[^ |]*) \| arranque por ([^=]+?) ===/g)];
  const last = boots.length ? boots[boots.length - 1] : null;
  return { at, bytes: text.length, firmware: last ? last[1] : "", wake: last ? last[2].trim() : "" };
}

deviceLog.get("/meta", async (c) => c.json({ ok: true, ...(await logMeta(accountOf(c))) }));

// GET /api/log (con el Bearer del aparato): el texto del log, para la página.
// Se poda también acá, y no sólo al subir: si el aparato dejó de sincronizar
// (que es justo cuando uno abre esta página), lo guardado sigue siendo lo de
// hace días y la promesa de "24 horas" no la cumpliría nadie.
deviceLog.get("/", async (c) => {
  let text = "";
  try {
    text = prune(await readFile(deviceLogFile(accountOf(c)), "utf8"), Date.now());
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
    const now = Date.now();
    const stamp = `\n===== ${new Date(now).toISOString()} =====\n`;
    let out = prune(previous, now) + stamp + text.slice(-MAX_BYTES);
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
