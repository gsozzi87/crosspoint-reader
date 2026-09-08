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

// fetch con timeout y saltos controlados: cada Location se vuelve a validar,
// porque un feed público puede redirigir a 169.254.169.254 y ahí están los
// metadatos de la nube.
export async function safeFetch(
  raw: string,
  init: RequestInit = {},
  opts: { timeoutMs?: number; maxHops?: number } = {},
): Promise<Response> {
  const timeoutMs = opts.timeoutMs ?? 10_000;
  const maxHops = opts.maxHops ?? 3;
  let url = raw;
  for (let hop = 0; hop <= maxHops; hop++) {
    const check = checkUrl(url, { allowHttp: true });
    if (!check.ok) throw new Error(check.error);
    const res = await fetch(check.url, { ...init, redirect: "manual", signal: AbortSignal.timeout(timeoutMs) });
    if (res.status < 300 || res.status > 399) return res;
    const next = res.headers.get("location");
    if (!next) return res;
    url = new URL(next, check.url).toString();
  }
  throw new Error("demasiadas redirecciones");
}

// El cuerpo remoto se corta antes de tenerlo entero en memoria: un "feed" de
// 500 MB no tiene que voltear el contenedor.
export async function textCapped(res: Response, maxBytes: number): Promise<string> {
  const body = res.body;
  if (!body) return "";
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
  return new TextDecoder().decode(buf.subarray(0, maxBytes));
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
