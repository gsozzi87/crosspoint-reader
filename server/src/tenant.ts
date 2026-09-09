// De dónde sale la cuenta de cada pedido.
//
// Tres formas de identificarse, en este orden:
//
//   1. aparato  → `Authorization: Bearer <token>`; se busca sha256(token) en
//                 `devices.token_hash` y sale su `account_id`. Se marca
//                 `devices.last_seen`.
//   2. web      → cookie de sesión firmada (HttpOnly, SameSite=Lax, Secure sobre
//                 https).
//   3. el de siempre → el `DEVICE_TOKEN` del entorno y el `config.deviceToken`
//                 valen SIEMPRE y resuelven a la cuenta 1. Es lo que hace que el
//                 aparato que ya está andando siga andando sin tocarle nada.
//
// Sin `DATABASE_URL` no hay ni cuentas ni login: todo pedido con el token válido
// es la cuenta 1, exactamente como hasta hoy.
//
// El middleware deja la cuenta en el contexto de Hono (`c.set("accountId", ...)`)
// y todos los handlers la leen de ahí con `accountOf(c)`.
import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import type { Context, MiddlewareHandler } from "hono";
import { getCookie, setCookie, deleteCookie } from "hono/cookie";
import { config } from "./config";
import { DEFAULT_ACCOUNT, db, metaGet, metaSetIfAbsent, multiUser, num, sha256Hex } from "./db";

/** Lo que el middleware deja en el contexto. Todo router de la API es `Hono<AppEnv>`. */
export type AppEnv = {
  Variables: {
    accountId: number;
    isAdmin: boolean;
    /** El aparato que hizo el pedido (device_id), si vino con Bearer de un aparato vinculado. */
    deviceId: string | null;
    /** Cómo se identificó: importa para las rutas que solo acepta la web. */
    via: "device" | "session" | "legacy" | "single";
  };
};

export type Tenant = AppEnv["Variables"];

export function accountOf(c: Context<AppEnv>): number {
  return c.get("accountId") ?? DEFAULT_ACCOUNT;
}

export function isAdmin(c: Context<AppEnv>): boolean {
  return c.get("isAdmin") === true;
}

export function viaOf(c: Context<AppEnv>): Tenant["via"] {
  return c.get("via") ?? "single";
}

// ───────────────────────────────────────────── token del aparato

const ENV_TOKEN = process.env.DEVICE_TOKEN ?? "";

export function bearerOf(c: Context<AppEnv>): string {
  const auth = c.req.header("authorization") ?? "";
  return auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
}

/** El token de siempre: el del entorno o el que se puso desde la web. Vale siempre. */
export async function isLegacyToken(token: string): Promise<boolean> {
  if (!token) return false;
  if (ENV_TOKEN && token === ENV_TOKEN) return true;
  const extra = (await config()).deviceToken;
  return !!extra && token === extra;
}

export type DeviceRow = { id: number; accountId: number; deviceId: string; name: string; isAdmin: boolean };

/** Busca el aparato por el hash de su token. Solo en modo multiusuario. */
export async function deviceByToken(token: string): Promise<DeviceRow | null> {
  if (!multiUser || !token) return null;
  const hash = sha256Hex(token);
  const rows = (await db()`
    SELECT d.id, d.account_id, d.device_id, d.name, a.is_admin
    FROM devices d JOIN accounts a ON a.id = d.account_id
    WHERE d.token_hash = ${hash}`) as any[];
  if (!rows.length) return null;
  const r = rows[0];
  return { id: num(r.id), accountId: num(r.account_id), deviceId: String(r.device_id), name: String(r.name ?? ""), isAdmin: r.is_admin === true };
}

async function touchDevice(id: number): Promise<void> {
  try {
    await db()`UPDATE devices SET last_seen = now() WHERE id = ${id}`;
  } catch (err) {
    console.error("devices.last_seen:", err);
  }
}

/** Marca la visita del aparato que entró con el token de siempre, si tiene fila. */
async function touchDeviceByToken(token: string): Promise<void> {
  try {
    await db()`UPDATE devices SET last_seen = now() WHERE token_hash = ${sha256Hex(token)}`;
  } catch (err) {
    console.error("devices.last_seen:", err);
  }
}

async function accountIsAdmin(accountId: number): Promise<boolean> {
  if (!multiUser) return true;  // sin cuentas, el único que hay es el operador
  const rows = (await db()`SELECT is_admin FROM accounts WHERE id = ${accountId}`) as { is_admin: boolean }[];
  return rows.length ? rows[0].is_admin === true : false;
}

// ───────────────────────────────────────────── sesión de la web

const COOKIE = "ws397_session";
const SESSION_DAYS = 30;

let secretPromise: Promise<string> | null = null;

/**
 * La clave con la que se firman las cookies. `SESSION_SECRET` manda; si no está,
 * se genera una y se guarda en la base (`server_meta`) para que un redeploy no
 * eche a todo el mundo. Sin base de datos no hay sesiones y no hace falta.
 */
export function sessionSecret(): Promise<string> {
  secretPromise ??= (async () => {
    const fromEnv = (process.env.SESSION_SECRET ?? "").trim();
    if (fromEnv) return fromEnv;
    if (!multiUser) return randomBytes(32).toString("hex");
    const made = randomBytes(32).toString("hex");
    const stored = await metaSetIfAbsent("session_secret", made);
    if (stored === made) console.log("sesiones: clave generada y guardada (pon SESSION_SECRET para fijarla desde el entorno)");
    return (await metaGet("session_secret")) ?? stored;
  })();
  return secretPromise;
}

function b64url(s: string): string {
  return Buffer.from(s, "utf8").toString("base64url");
}

async function sign(payload: string): Promise<string> {
  return createHmac("sha256", await sessionSecret()).update(payload).digest("base64url");
}

/** Cookie de sesión: `<accountId>.<vence>` firmado. No guarda nada del lado del servidor. */
export async function makeSessionValue(accountId: number): Promise<string> {
  const exp = Math.floor(Date.now() / 1000) + SESSION_DAYS * 86_400;
  const payload = b64url(`${accountId}.${exp}`);
  return `${payload}.${await sign(payload)}`;
}

export async function readSessionValue(value: string): Promise<number | null> {
  const dot = value.lastIndexOf(".");
  if (dot < 1) return null;
  const payload = value.slice(0, dot);
  const givenSig = value.slice(dot + 1);
  const wantSig = await sign(payload);
  const a = Buffer.from(givenSig);
  const b = Buffer.from(wantSig);
  if (a.length !== b.length || !timingSafeEqual(a, b)) return null;
  let decoded = "";
  try {
    decoded = Buffer.from(payload, "base64url").toString("utf8");
  } catch {
    return null;
  }
  const [idRaw, expRaw] = decoded.split(".");
  const id = Number(idRaw);
  const exp = Number(expRaw);
  if (!Number.isInteger(id) || id <= 0 || !Number.isFinite(exp)) return null;
  if (exp * 1000 < Date.now()) return null;
  return id;
}

function secureCookie(c: Context<AppEnv>): boolean {
  const proto = c.req.header("x-forwarded-proto") ?? "";
  if (proto) return proto.split(",")[0].trim() === "https";
  try {
    return new URL(c.req.url).protocol === "https:";
  } catch {
    return false;
  }
}

export async function startSession(c: Context<AppEnv>, accountId: number): Promise<void> {
  setCookie(c, COOKIE, await makeSessionValue(accountId), {
    httpOnly: true,
    secure: secureCookie(c),
    sameSite: "Lax",
    path: "/",
    maxAge: SESSION_DAYS * 86_400,
  });
}

export function endSession(c: Context<AppEnv>): void {
  deleteCookie(c, COOKIE, { path: "/" });
}

/** La cuenta de la cookie, o null. Devuelve null sin base de datos (no hay sesiones). */
export async function sessionAccount(c: Context<AppEnv>): Promise<number | null> {
  if (!multiUser) return null;
  const raw = getCookie(c, COOKIE);
  if (!raw) return null;
  const id = await readSessionValue(raw);
  if (!id) return null;
  const rows = (await db()`SELECT id FROM accounts WHERE id = ${id}`) as { id: unknown }[];
  return rows.length ? num(rows[0].id) : null;
}

// ───────────────────────────────────────────── resolución

/** Quién hizo este pedido, o null si nadie válido. */
export async function resolveTenant(c: Context<AppEnv>): Promise<Tenant | null> {
  const token = bearerOf(c);

  if (token) {
    // El token de siempre gana: nunca se puede quedar afuera el aparato que ya
    // estaba andando.
    if (await isLegacyToken(token)) {
      if (multiUser) void touchDeviceByToken(token);
      return {
        accountId: DEFAULT_ACCOUNT,
        isAdmin: await accountIsAdmin(DEFAULT_ACCOUNT),
        deviceId: null,
        via: multiUser ? "legacy" : "single",
      };
    }
    const dev = await deviceByToken(token);
    if (dev) {
      void touchDevice(dev.id);
      return { accountId: dev.accountId, isAdmin: dev.isAdmin, deviceId: dev.deviceId, via: "device" };
    }
    return null;  // un Bearer que no sirve no cae a la cookie: es un token equivocado
  }

  const fromCookie = await sessionAccount(c);
  if (fromCookie) {
    return { accountId: fromCookie, isAdmin: await accountIsAdmin(fromCookie), deviceId: null, via: "session" };
  }
  return null;
}

/** Middleware: resuelve la cuenta y la deja en el contexto, o 401. */
export const requireTenant: MiddlewareHandler<AppEnv> = async (c, next) => {
  const t = await resolveTenant(c);
  if (!t) return c.json({ ok: false, error: "unauthorized" }, 401);
  c.set("accountId", t.accountId);
  c.set("isAdmin", t.isAdmin);
  c.set("deviceId", t.deviceId);
  c.set("via", t.via);
  await next();
};
