// Cosas de HTTP compartidas: cuerpos JSON, salidas a internet y borrado de
// claves en los mensajes de error.
//
// Por qué existe: el servidor guarda las claves del proveedor de IA y vive
// adentro de la red privada de Railway. Una URL que elige el usuario (el
// baseUrl del proveedor, un feed RSS, el link de un artículo) sale de acá con
// el Bearer puesto o llega a los vecinos del proyecto, así que toda URL pasa
// por el mismo control y todo cuerpo remoto se limpia antes de mostrarse.
import type { Context } from "hono";

export type UrlCheck = { ok: true; url: URL } | { ok: false; error: string };

// Hosts que no se pueden pedir desde afuera: si el usuario los pone, o es un
// error o es alguien buscando la red interna.
function isPrivateHost(hostname: string): boolean {
  const h = hostname.toLowerCase().replace(/^\[|\]$/g, "");
  if (h === "localhost" || h.endsWith(".localhost")) return true;
  if (h.endsWith(".internal") || h.endsWith(".local")) return true;
  if (h === "::1" || h === "::") return true;
  if (/^f[cd][0-9a-f]{2}:/.test(h)) return true;                    // IPv6 privadas (ULA)
  if (/^fe80:/.test(h)) return true;                                // IPv6 link-local
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(h);
  if (!m) return false;
  const a = Number(m[1]), b = Number(m[2]);
  if (a === 0 || a === 127 || a === 10) return true;
  if (a === 192 && b === 168) return true;
  if (a === 169 && b === 254) return true;                          // metadatos de la nube
  if (a === 172 && b >= 16 && b <= 31) return true;
  return false;
}

function isLoopback(hostname: string): boolean {
  const h = hostname.toLowerCase().replace(/^\[|\]$/g, "");
  return h === "localhost" || h === "127.0.0.1" || h === "::1";
}

// allowHttp: acepta http:// a un host público (feeds viejos que no tienen TLS).
// allowLocal: acepta http://localhost (un modelo corriendo en la misma máquina).
export function checkUrl(raw: string, opts: { allowHttp?: boolean; allowLocal?: boolean } = {}): UrlCheck {
  let u: URL;
  try {
    u = new URL(raw);
  } catch {
    return { ok: false, error: "no es una URL válida" };
  }
  if (u.protocol !== "https:" && u.protocol !== "http:") return { ok: false, error: "solo http:// o https://" };
  if (isPrivateHost(u.hostname)) {
    if (opts.allowLocal && isLoopback(u.hostname)) return { ok: true, url: u };
    return { ok: false, error: `no se puede usar un host de red interna (${u.hostname})` };
  }
  if (u.protocol === "http:" && !opts.allowHttp) return { ok: false, error: "tiene que ser https://" };
  return { ok: true, url: u };
}

export function isSafeRemoteUrl(raw: string): boolean {
  return checkUrl(raw, { allowHttp: true }).ok;
}

// Navegador común. Muchos sitios (todo lo que está detrás de Cloudflare)
// devuelven 403 a un User-Agent que no parece un navegador, y así un feed que
// anda perfecto en el teléfono llegaba vacío al aparato.
export const BROWSER_UA =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

// Con `*/*` al final: hay servidores que contestan 406 si el Accept no incluye
// el tipo con el que sirven el feed (a veces text/plain o application/json).
export const FEED_ACCEPT =
  "application/rss+xml, application/atom+xml, application/rdf+xml, application/xml;q=0.9, text/xml;q=0.9, application/json;q=0.8, text/html;q=0.7, */*;q=0.5";

// fetch con timeout y saltos controlados: cada Location se vuelve a validar,
// porque un feed público puede redirigir a 169.254.169.254 y ahí están los
// metadatos de la nube. Las redirecciones SE SIGUEN (revalidando el destino):
// casi todo feed rebota al menos una vez (http→https, /feed→/feed/, un CDN).
export async function safeFetchAt(
  raw: string,
  init: RequestInit = {},
  opts: { timeoutMs?: number; maxHops?: number } = {},
): Promise<{ res: Response; url: string }> {
  const timeoutMs = opts.timeoutMs ?? 10_000;
  const maxHops = opts.maxHops ?? 5;
  let url = raw;
  for (let hop = 0; hop <= maxHops; hop++) {
    const check = checkUrl(url, { allowHttp: true });
    if (!check.ok) throw new Error(check.error);
    const res = await fetch(check.url, { ...init, redirect: "manual", signal: AbortSignal.timeout(timeoutMs) });
    if (res.status < 300 || res.status > 399) return { res, url: check.url.toString() };
    const next = res.headers.get("location");
    if (!next) return { res, url: check.url.toString() };
    // El cuerpo del salto no se lee nunca: si no se cierra, la conexión queda
    // colgada hasta el timeout.
    res.body?.cancel().catch(() => {});
    url = new URL(next, check.url).toString();
  }
  throw new Error("demasiadas redirecciones");
}

export async function safeFetch(
  raw: string,
  init: RequestInit = {},
  opts: { timeoutMs?: number; maxHops?: number } = {},
): Promise<Response> {
  return (await safeFetchAt(raw, init, opts)).res;
}

// El cuerpo remoto se corta antes de tenerlo entero en memoria: un "feed" de
// 500 MB no tiene que voltear el contenedor.
export async function bytesCapped(res: Response, maxBytes: number): Promise<Uint8Array> {
  const body = res.body;
  if (!body) return new Uint8Array(0);
  const reader = body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  while (total < maxBytes) {
    const { done, value } = await reader.read();
    if (done) break;
    if (value) {
      chunks.push(value);
      total += value.byteLength;
    }
  }
  await reader.cancel().catch(() => {});
  const buf = new Uint8Array(total);
  let off = 0;
  for (const ch of chunks) {
    buf.set(ch, off);
    off += ch.byteLength;
  }
  return buf.subarray(0, maxBytes);
}

// Tablas de los juegos de caracteres de un byte que el TextDecoder de Bun NO
// trae (solo tiene utf-8, utf-16, windows-1252 y unos pocos más). Cada string
// son los 128 caracteres de la mitad alta (0x80..0xFF).
const HIGH_HALF: Record<string, string> = {
  "iso-8859-2":
    "\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0Ą˘Ł¤ĽŚ§¨ŠŞŤŹ\u00adŽŻ°ą˛ł´ľśˇ¸šşťź˝žżŔÁÂĂÄĹĆÇČÉĘËĚÍÎĎĐŃŇÓÔŐÖ×ŘŮÚŰÜÝŢßŕáâăäĺćçčéęëěíîďđńňóôőö÷řůúűüýţ˙",
  "iso-8859-5":
    "\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0ЁЂЃЄЅІЇЈЉЊЋЌ\u00adЎЏАБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмнопрстуфхцчшщъыьэюя№ёђѓєѕіїјљњћќ§ўџ",
  "iso-8859-7":
    "\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0‘’£€₯¦§¨©ͺ«¬\u00ad\ufffd―°±²³΄΅Ά·ΈΉΊ»Ό½ΎΏΐΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡ\ufffdΣΤΥΦΧΨΩΪΫάέήίΰαβγδεζηθικλμνξοπρςστυφχψωϊϋόύώ\ufffd",
  "iso-8859-9":
    "\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0¡¢£¤¥¦§¨©ª«¬\u00ad®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏĞÑÒÓÔÕÖ×ØÙÚÛÜİŞßàáâãäåæçèéêëìíîïğñòóôõö÷øùúûüışÿ",
  "iso-8859-15":
    "\u0080\u0081\u0082\u0083\u0084\u0085\u0086\u0087\u0088\u0089\u008a\u008b\u008c\u008d\u008e\u008f\u0090\u0091\u0092\u0093\u0094\u0095\u0096\u0097\u0098\u0099\u009a\u009b\u009c\u009d\u009e\u009f\u00a0¡¢£€¥Š§š©ª«¬\u00ad®¯°±²³Žµ¶·ž¹º»ŒœŸ¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ",
  "windows-1250":
    "€\ufffd‚\ufffd„…†‡\ufffd‰Š‹ŚŤŽŹ\ufffd‘’“”•–—\ufffd™š›śťžź\u00adˇ˘Ł¤Ą¦§¨©Ş«¬\u00ad®Ż°±˛ł´µ¶·¸ąş»Ľ˝ľżŔÁÂĂÄĹĆÇČÉĘËĚÍÎĎĐŃŇÓÔŐÖ×ŘŮÚŰÜÝŢßŕáâăäĺćçčéęëěíîďđńňóôőö÷řůúűüýţ˙",
  "windows-1251":
    "ЂЃ‚ѓ„…†‡€‰Љ‹ЊЌЋЏђ‘’“”•–—\ufffd™љ›њќћџ\u00a0ЎўЈ¤Ґ¦§Ё©Є«¬\u00ad®Ї°±Ііґµ¶·ё№є»јЅѕїАБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмнопрстуфхцчшщъыьэюя",
  "koi8-r":
    "─│┌┐└┘├┤┬┴┼▀▄█▌▐░▒▓⌠■∙√≈≤≥\u00a0⌡°²·÷═║╒ё╓╔╕╖╗╘╙╚╛╜╝╞╟╠╡Ё╢╣╤╥╦╧╨╩╪╫╬©юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ",
};

// Nombre normalizado: "ISO-8859-1", "iso8859-1", "latin1", "utf8" -> el mismo.
function canonCharset(raw: string): string {
  const s = raw.trim().toLowerCase().replace(/^["']|["']$/g, "").replace(/[_\s]/g, "-");
  const alias: Record<string, string> = {
    "utf8": "utf-8", "utf-8": "utf-8", "usascii": "utf-8", "us-ascii": "utf-8", "ascii": "utf-8",
    "latin1": "windows-1252", "latin-1": "windows-1252", "l1": "windows-1252",
    "iso88591": "windows-1252", "iso-8859-1": "windows-1252", "iso8859-1": "windows-1252",
    "cp1252": "windows-1252", "win-1252": "windows-1252",
    "iso885915": "iso-8859-15", "iso8859-15": "iso-8859-15", "latin9": "iso-8859-15",
    "iso88592": "iso-8859-2", "iso8859-2": "iso-8859-2",
    "iso88595": "iso-8859-5", "iso8859-5": "iso-8859-5",
    "iso88597": "iso-8859-7", "iso8859-7": "iso-8859-7",
    "iso88599": "iso-8859-9", "iso8859-9": "iso-8859-9",
    "cp1250": "windows-1250", "cp1251": "windows-1251", "koi8r": "koi8-r", "koi8-u": "koi8-r",
  };
  return alias[s] ?? s;
}

// Decodifica con el juego que digan; si el TextDecoder de Bun no lo conoce,
// con la tabla de arriba; y si tampoco está, utf-8.
function decodeWith(bytes: Uint8Array, charset: string): string {
  const cs = canonCharset(charset);
  const table = HIGH_HALF[cs];
  if (table) {
    let out = "";
    // De a pedazos: armar un string de 4 MB con += byte a byte es lentísimo.
    const CHUNK = 8192;
    for (let i = 0; i < bytes.length; i += CHUNK) {
      const part = bytes.subarray(i, Math.min(i + CHUNK, bytes.length));
      let s = "";
      for (let j = 0; j < part.length; j++) {
        const b = part[j];
        s += b < 0x80 ? String.fromCharCode(b) : table[b - 0x80];
      }
      out += s;
    }
    return out;
  }
  try {
    return new TextDecoder(cs).decode(bytes);
  } catch {
    return new TextDecoder("utf-8").decode(bytes);
  }
}

// ¿Los bytes son UTF-8 legal? Sirve de desempate: un feed que dice utf-8 y no
// lo es (o que no dice nada) se decodifica como windows-1252 en vez de perder
// todas las tildes.
export function looksUtf8(bytes: Uint8Array): boolean {
  const n = Math.min(bytes.length, 65536);
  let i = 0;
  let sawMultibyte = false;
  while (i < n) {
    const b = bytes[i];
    if (b < 0x80) { i++; continue; }
    let extra = 0;
    if (b >= 0xc2 && b <= 0xdf) extra = 1;
    else if (b >= 0xe0 && b <= 0xef) extra = 2;
    else if (b >= 0xf0 && b <= 0xf4) extra = 3;
    else return false;
    if (i + extra >= n) break;  // cortado por el tope: no cuenta como error
    for (let k = 1; k <= extra; k++) {
      const c = bytes[i + k];
      if (c < 0x80 || c > 0xbf) return false;
    }
    sawMultibyte = true;
    i += extra + 1;
  }
  return sawMultibyte || true;
}

// El juego de caracteres declarado adentro del documento: <?xml encoding="...">,
// <meta charset="..."> o <meta http-equiv="Content-Type" content="...charset=...">.
// Se mira solo el principio, que es donde la declaración tiene que estar.
export function charsetFromBody(bytes: Uint8Array): string {
  let head = "";
  const n = Math.min(bytes.length, 2048);
  for (let i = 0; i < n; i++) head += String.fromCharCode(bytes[i]);
  const xml = /<\?xml[^>]*\bencoding\s*=\s*["']([\w.:-]+)["']/i.exec(head);
  if (xml) return xml[1];
  const meta = /<meta[^>]*\bcharset\s*=\s*["']?([\w.:-]+)/i.exec(head);
  if (meta) return meta[1];
  const http = /<meta[^>]*content\s*=\s*["'][^"']*charset\s*=\s*([\w.:-]+)/i.exec(head);
  if (http) return http[1];
  return "";
}

// Bytes -> texto, eligiendo el juego de caracteres como manda el mundo real:
// BOM, luego el charset del Content-Type, luego la declaración del documento y
// al final utf-8. Por qué existe: `res.text()` asume utf-8 SIEMPRE, y medio
// diario latinoamericano todavía sirve el RSS en iso-8859-1; los bytes
// inválidos se tiraban y "México" llegaba al aparato como "Mxico".
export function decodeBody(bytes: Uint8Array, contentType?: string | null): string {
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
    return new TextDecoder("utf-8").decode(bytes.subarray(3));
  }
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) return decodeWith(bytes.subarray(2), "utf-16le");
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) return decodeWith(bytes.subarray(2), "utf-16be");
  const fromHeader = /charset\s*=\s*["']?([\w.:-]+)/i.exec(contentType ?? "")?.[1] ?? "";
  const declared = canonCharset(fromHeader || charsetFromBody(bytes));
  if (declared && declared !== "utf-8") return decodeWith(bytes, declared);
  // Dice utf-8 (o no dice nada) pero no lo es: casi siempre es windows-1252.
  if (!looksUtf8(bytes)) return decodeWith(bytes, "windows-1252");
  return new TextDecoder("utf-8").decode(bytes);
}

// Igual que antes para quien solo quiere texto utf-8 (ICS, JSON, la Biblia).
export async function textCapped(res: Response, maxBytes: number): Promise<string> {
  return new TextDecoder().decode(await bytesCapped(res, maxBytes));
}

// Cuerpo remoto respetando el juego de caracteres que declara (feeds y páginas).
export async function textCappedSmart(res: Response, maxBytes: number): Promise<string> {
  return decodeBody(await bytesCapped(res, maxBytes), res.headers.get("content-type"));
}

// Algunos proveedores repiten la clave que les mandaste en el error de auth, y
// ese error vuelve al aparato y a la web. Antes de mostrar o loguear cualquier
// cuerpo remoto pasa por acá.
export function redactSecrets(text: string, ...keys: (string | null | undefined)[]): string {
  let out = String(text ?? "");
  for (const k of keys) {
    const key = (k ?? "").trim();
    if (key.length >= 8) out = out.split(key).join("***");
  }
  return out
    .replace(/\b(?:sk|pk|gsk|xai|api|key)[-_][A-Za-z0-9_-]{8,}/gi, "***")
    .replace(/\bBearer\s+[A-Za-z0-9._-]{8,}/gi, "Bearer ***");
}

// `await c.req.json().catch(() => ({}))` devuelve null con un cuerpo literal
// "null" (el catch no salta) y la primera propiedad que se lea tira un 500.
export async function readBody(c: Context): Promise<Record<string, any>> {
  try {
    const b = await c.req.json();
    return b && typeof b === "object" && !Array.isArray(b) ? b : {};
  } catch {
    return {};
  }
}
