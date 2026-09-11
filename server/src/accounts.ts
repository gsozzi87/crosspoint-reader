// Cuentas de la web (correo + contraseña), sesión, y vinculación del aparato.
//
// Nada de esto existe sin `DATABASE_URL`: en modo de un solo usuario los
// endpoints contestan `single_user` y /board sigue pidiendo el token del
// aparato, como siempre.
//
//   POST /auth/register  { email, password }     -> crea la cuenta y abre sesión
//   POST /auth/login     { email, password }     -> cookie de sesión
//   POST /auth/logout                            -> borra la cookie
//   GET  /auth/me                                -> quién soy y qué aparatos tengo
//
// Vinculación (el aparato NO tiene teclado, así que el flujo va al revés: el
// aparato muestra un código de 6 dígitos y la persona lo escribe en la web):
//
//   POST /api/pair/start    (SIN Bearer)  { deviceId, token }  -> { code, expiresIn }
//   POST /api/account/pair  (con sesión)  { code, name }       -> { deviceId }
//   GET  /api/pair/status   (Bearer del aparato)               -> { paired, account }
import { Hono } from "hono";
import { readBody } from "./net";
import { readJsonSafe, legacyFile, DOC_NAMES } from "./fsjson";
import { DEFAULT_ACCOUNT, db, multiUser, num, sha256Hex } from "./db";
import { endSession, sessionAccount, startSession, type AppEnv, accountOf, viaOf } from "./tenant";

const ADMIN_EMAIL = (process.env.ADMIN_EMAIL ?? "").trim().toLowerCase();
const MIN_PASSWORD = 8;
const PAIR_TTL_S = 600;          // 10 minutos
const PAIR_COOLDOWN_MS = 30_000; // un pedido cada 30 s por aparato

const NOT_MULTI = { ok: false, error: "este servidor no tiene cuentas (falta DATABASE_URL)", code: "single_user" } as const;

// ─────────────────────────────────────────────────────── utilidades

function cleanEmail(raw: unknown): string {
  const s = (raw ?? "").toString().trim().toLowerCase().slice(0, 190);
  return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s) ? s : "";
}

function cleanDeviceId(raw: unknown): string {
  // La MAC en hex sin separadores. Se acepta cualquier hex corto por si el
  // firmware manda otra cosa, pero nada más que hex.
  const s = (raw ?? "").toString().trim().toUpperCase().replace(/[^0-9A-F]/g, "");
  return s.length >= 6 && s.length <= 32 ? s : "";
}

function cleanDeviceToken(raw: unknown): string {
  const s = (raw ?? "").toString().trim();
  return /^[0-9a-fA-F]{32,128}$/.test(s) ? s : "";
}

function cleanName(raw: unknown): string {
  return (raw ?? "").toString().replace(/\s+/g, " ").trim().slice(0, 60);
}

export type Account = { id: number; email: string; isAdmin: boolean };

async function accountById(id: number): Promise<Account | null> {
  const rows = (await db()`SELECT id, email, is_admin FROM accounts WHERE id = ${id}`) as any[];
  if (!rows.length) return null;
  return { id: num(rows[0].id), email: String(rows[0].email), isAdmin: rows[0].is_admin === true };
}

export async function devicesOf(accountId: number): Promise<{ deviceId: string; name: string; lastSeen: string | null }[]> {
  const rows = (await db()`
    SELECT device_id, name, last_seen FROM devices WHERE account_id = ${accountId} ORDER BY created_at`) as any[];
  return rows.map((r) => ({
    deviceId: String(r.device_id),
    name: String(r.name ?? ""),
    lastSeen: r.last_seen ? new Date(r.last_seen).toISOString() : null,
  }));
}

// ─────────────────────────────────────────────────────── migración

const MISSING = Symbol("sin archivo");

/**
 * Primera vez que se arranca con `DATABASE_URL`: si no hay ninguna cuenta y hay
 * archivos en /data, se crea la cuenta 1 (la del usuario que ya estaba andando),
 * se vuelca cada JSON a `docs` y se registra el `DEVICE_TOKEN` del entorno como
 * su primer aparato. La contraseña se imprime UNA vez en el log.
 *
 * Idempotente: con la tabla `accounts` ya poblada no toca nada.
 */
// Rescate desde Railway: con ADMIN_PASSWORD puesta, al arrancar se le pone esa
// contraseña a la cuenta de administrador.
//
// Existe porque la contraseña provisoria de `seedFromFiles` se imprime UNA sola
// vez en los logs del despliegue, y perderse esa línea deja la cuenta —y con
// ella la web, la vinculación del aparato y el log— inalcanzable para siempre,
// sin forma de recuperarla salvo borrando la base. Quien puede escribir las
// variables de entorno ya es dueño del servidor, así que esto no agrega
// permisos que no tuviera: sólo evita que un descuido sea irreversible.
export async function applyAdminPasswordFromEnv(): Promise<void> {
  if (!multiUser) return;
  const wanted = (process.env.ADMIN_PASSWORD ?? "").trim();
  if (!wanted) return;
  if (wanted.length < MIN_PASSWORD) {
    console.error(`db: ADMIN_PASSWORD tiene menos de ${MIN_PASSWORD} caracteres: no se aplica`);
    return;
  }
  const email = ADMIN_EMAIL;
  if (!email) {
    console.error("db: ADMIN_PASSWORD está puesta pero falta ADMIN_EMAIL: no se sabe a qué cuenta aplicarla");
    return;
  }
  const hash = await Bun.password.hash(wanted, "argon2id");
  const rows = (await db()`UPDATE accounts SET pass_hash = ${hash} WHERE email = ${email} RETURNING id`) as any[];
  if (!rows.length) {
    console.error(`db: ADMIN_PASSWORD: no hay ninguna cuenta con el correo ${email}`);
    return;
  }
  console.log("─".repeat(70));
  console.log(`db: contraseña de ${email} cambiada por ADMIN_PASSWORD`);
  console.log("db: SACÁ esa variable del entorno ahora que ya entraste");
  console.log("─".repeat(70));
}

export async function seedFromFiles(): Promise<void> {
  if (!multiUser) return;
  const rows = (await db()`SELECT count(*)::int AS n FROM accounts`) as { n: number }[];
  if (num(rows[0]?.n) > 0) return;

  const email = ADMIN_EMAIL || "admin@localhost";
  const password = Buffer.from(crypto.getRandomValues(new Uint8Array(12))).toString("base64url");
  const hash = await Bun.password.hash(password, "argon2id");
  const created = (await db()`
    INSERT INTO accounts (email, pass_hash, is_admin) VALUES (${email}, ${hash}, true) RETURNING id`) as any[];
  const accountId = num(created[0].id);

  let moved = 0;
  for (const name of DOC_NAMES) {
    const data = await readJsonSafe<unknown>(legacyFile(name), MISSING);
    if (data === MISSING || data === null || typeof data !== "object") continue;
    await db()`INSERT INTO docs (account_id, name, data) VALUES (${accountId}, ${name}, ${data as any})
               ON CONFLICT (account_id, name) DO NOTHING`;
    moved++;
  }

  const envToken = (process.env.DEVICE_TOKEN ?? "").trim();
  if (envToken) {
    await db()`
      INSERT INTO devices (account_id, device_id, token_hash, name)
      VALUES (${accountId}, ${"legacy"}, ${sha256Hex(envToken)}, ${"El aparato de siempre"})
      ON CONFLICT (device_id) DO NOTHING`;
  }

  console.log("─".repeat(70));
  console.log(`db: cuenta de administrador creada -> ${email}`);
  console.log(`db: contraseña provisoria -> ${password}`);
  console.log("db: CÁMBIALA apenas entres (esta línea no se vuelve a imprimir)");
  console.log(`db: ${moved} documento(s) migrados de /data a la cuenta ${accountId}`);
  console.log("─".repeat(70));
}

// ─────────────────────────────────────────────────────── login: freno

// Freno de fuerza bruta: por IP y por correo, ventana de 15 minutos. En memoria
// a propósito (un reinicio lo limpia y no vale la pena una tabla para esto).
const WINDOW_MS = 15 * 60_000;
const MAX_PER_EMAIL = 10;
const MAX_PER_IP = 40;
const attempts = new Map<string, { n: number; until: number }>();

function hit(key: string, max: number): boolean {
  const now = Date.now();
  if (attempts.size > 5_000) {
    for (const [k, v] of attempts) if (v.until < now) attempts.delete(k);
  }
  const cur = attempts.get(key);
  if (!cur || cur.until < now) {
    attempts.set(key, { n: 1, until: now + WINDOW_MS });
    return true;
  }
  cur.n++;
  return cur.n <= max;
}

function forget(key: string): void {
  attempts.delete(key);
}

function ipOf(c: { req: { header: (k: string) => string | undefined } }): string {
  const fwd = c.req.header("x-forwarded-for") ?? "";
  return (fwd.split(",")[0] || c.req.header("x-real-ip") || "?").trim().slice(0, 45);
}

// ─────────────────────────────────────────────────────── /auth

export const auth = new Hono<AppEnv>();

// /auth no exige nada (hay que poder entrar sin estar adentro), pero si ya hay
// cookie válida la deja en el contexto para que /auth/me la use.
auth.use("*", async (c, next) => {
  const id = await sessionAccount(c);
  if (id) c.set("accountId", id);
  await next();
});

auth.post("/register", async (c) => {
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const email = cleanEmail(b.email);
  const password = (b.password ?? "").toString();
  if (!email) return c.json({ ok: false, error: "correo inválido", code: "bad_email" }, 400);
  if (password.length < MIN_PASSWORD) {
    return c.json({ ok: false, error: `la contraseña necesita al menos ${MIN_PASSWORD} caracteres`, code: "weak_password" }, 400);
  }
  const count = (await db()`SELECT count(*)::int AS n FROM accounts`) as { n: number }[];
  const first = num(count[0]?.n) === 0;
  const isAdmin = first || (!!ADMIN_EMAIL && email === ADMIN_EMAIL);
  const hash = await Bun.password.hash(password, "argon2id");
  let id = 0;
  try {
    const rows = (await db()`
      INSERT INTO accounts (email, pass_hash, is_admin) VALUES (${email}, ${hash}, ${isAdmin})
      ON CONFLICT (email) DO NOTHING RETURNING id`) as any[];
    if (!rows.length) {
      // El correo ya está tomado. No se dice: si no, cualquiera averigua qué
      // correos están registrados probándolos de a uno.
      return c.json({ ok: false, error: "no se pudo crear la cuenta con esos datos", code: "register_failed" }, 400);
    }
    id = num(rows[0].id);
  } catch (err) {
    console.error("register:", err);
    return c.json({ ok: false, error: "no se pudo crear la cuenta con esos datos", code: "register_failed" }, 400);
  }
  await startSession(c, id);
  console.log(`cuenta nueva #${id} ${email}${isAdmin ? " (admin)" : ""}`);
  return c.json({ ok: true, email, isAdmin });
});

auth.post("/login", async (c) => {
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const email = cleanEmail(b.email);
  const password = (b.password ?? "").toString();
  const ip = ipOf(c);
  const bad = { ok: false, error: "correo o contraseña incorrectos", code: "bad_login" } as const;
  if (!hit(`ip:${ip}`, MAX_PER_IP) || !hit(`em:${email}`, MAX_PER_EMAIL)) {
    return c.json({ ok: false, error: "demasiados intentos, espera unos minutos", code: "rate_limited" }, 429);
  }
  if (!email || !password) return c.json(bad, 401);
  const rows = (await db()`SELECT id, pass_hash, is_admin FROM accounts WHERE email = ${email}`) as any[];
  if (!rows.length) return c.json(bad, 401);
  const ok = await Bun.password.verify(password, String(rows[0].pass_hash)).catch(() => false);
  if (!ok) return c.json(bad, 401);
  forget(`em:${email}`);
  forget(`ip:${ip}`);
  const id = num(rows[0].id);
  await startSession(c, id);
  return c.json({ ok: true, email, isAdmin: rows[0].is_admin === true });
});

auth.post("/logout", (c) => {
  endSession(c);
  return c.json({ ok: true });
});

// Con esto la página decide todo: si hay cuentas, si hay sesión y si es admin.
auth.get("/me", async (c) => {
  if (!multiUser) {
    return c.json({ ok: true, multi: false, email: "", isAdmin: true, devices: [] });
  }
  const id = c.get("accountId");
  if (!id) return c.json({ ok: false, multi: true, code: "no_session", error: "hay que entrar" }, 401);
  const acc = await accountById(id);
  if (!acc) return c.json({ ok: false, multi: true, code: "no_session", error: "hay que entrar" }, 401);
  return c.json({ ok: true, multi: true, email: acc.email, isAdmin: acc.isAdmin, devices: await devicesOf(acc.id) });
});

// ─────────────────────────────────────────────────────── vinculación

function sixDigits(): string {
  return String(100_000 + Math.floor(Math.random() * 900_000));
}

async function dropExpired(): Promise<void> {
  await db()`DELETE FROM pairings WHERE created_at < now() - make_interval(secs => ${PAIR_TTL_S})`;
}

export type PairStart =
  | { ok: true; code: string; expiresIn: number }
  | { ok: false; status: 429 | 400 | 501; error: string; code: string };

/** El aparato pide un código. No tiene cuenta todavía, así que no hay Bearer. */
export async function startPairing(deviceIdRaw: unknown, tokenRaw: unknown): Promise<PairStart> {
  if (!multiUser) return { ok: false, status: 501, error: NOT_MULTI.error, code: NOT_MULTI.code };
  const deviceId = cleanDeviceId(deviceIdRaw);
  const token = cleanDeviceToken(tokenRaw);
  if (!deviceId) return { ok: false, status: 400, error: "deviceId inválido", code: "bad_device" };
  if (!token) return { ok: false, status: 400, error: "token inválido (64 hex)", code: "bad_token" };

  await dropExpired();

  const live = (await db()`SELECT code, created_at FROM pairings WHERE device_id = ${deviceId}`) as any[];
  if (live.length) {
    const age = Date.now() - new Date(live[0].created_at).getTime();
    // Un pedido cada 30 s: mientras tanto se devuelve el MISMO código, así el
    // aparato que reintenta no le cambia el número al usuario en la cara.
    if (age < PAIR_COOLDOWN_MS) {
      return { ok: true, code: String(live[0].code), expiresIn: Math.max(1, PAIR_TTL_S - Math.floor(age / 1000)) };
    }
    await db()`DELETE FROM pairings WHERE device_id = ${deviceId}`;
  }

  const hash = sha256Hex(token);
  for (let i = 0; i < 20; i++) {
    const code = sixDigits();
    const rows = (await db()`
      INSERT INTO pairings (code, device_id, token_hash) VALUES (${code}, ${deviceId}, ${hash})
      ON CONFLICT (code) DO NOTHING RETURNING code`) as any[];
    if (rows.length) return { ok: true, code, expiresIn: PAIR_TTL_S };
  }
  return { ok: false, status: 400, error: "no se pudo generar un código, prueba de nuevo", code: "no_code" };
}

/** Lo que consulta el aparato cada pocos segundos mientras muestra el código. */
export async function pairStatus(token: string): Promise<{ paired: boolean; account: string | null; single?: boolean }> {
  if (!multiUser) {
    // Sin cuentas no hay nada que vincular: si el token sirve, ya está listo.
    return { paired: true, account: null, single: true };
  }
  if (!token) return { paired: false, account: null };
  const rows = (await db()`
    SELECT a.email FROM devices d JOIN accounts a ON a.id = d.account_id
    WHERE d.token_hash = ${sha256Hex(token)}`) as any[];
  if (!rows.length) return { paired: false, account: null };
  return { paired: true, account: String(rows[0].email) };
}

// ─────────────────────────────────────────────────────── /api/account

// Cuelga de /api, así que ya pasó por `requireTenant`: hay cuenta sí o sí.
export const accountApi = new Hono<AppEnv>();

function webOnly(c: any) {
  // Vincular, renombrar y desvincular son cosas de la web: un aparato con su
  // Bearer no puede reasignarse solo.
  return viaOf(c) === "session" ? null : c.json({ ok: false, error: "hay que entrar desde la web", code: "no_session" }, 403);
}

accountApi.post("/pair", async (c) => {
  const deny = webOnly(c);
  if (deny) return deny;
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const code = (b.code ?? "").toString().replace(/\D/g, "").slice(0, 6);
  const name = cleanName(b.name);
  if (code.length !== 6) return c.json({ ok: false, error: "el código son 6 dígitos", code: "bad_code" }, 400);
  await dropExpired();
  const rows = (await db()`SELECT device_id, token_hash FROM pairings WHERE code = ${code}`) as any[];
  if (!rows.length) return c.json({ ok: false, error: "ese código no existe o venció", code: "not_found" }, 404);
  const deviceId = String(rows[0].device_id);
  const tokenHash = String(rows[0].token_hash);
  const accountId = accountOf(c);
  // Si el aparato ya estaba en otra cuenta, se MUEVE a esta.
  await db()`
    INSERT INTO devices (account_id, device_id, token_hash, name, last_seen)
    VALUES (${accountId}, ${deviceId}, ${tokenHash}, ${name || null}, now())
    ON CONFLICT (device_id) DO UPDATE
      SET account_id = EXCLUDED.account_id,
          token_hash = EXCLUDED.token_hash,
          name = COALESCE(EXCLUDED.name, devices.name),
          last_seen = now()`;
  await db()`DELETE FROM pairings WHERE code = ${code}`;
  console.log(`aparato ${deviceId} vinculado a la cuenta ${accountId}`);
  return c.json({ ok: true, deviceId });
});

accountApi.get("/devices", async (c) => {
  if (!multiUser) return c.json({ ok: true, devices: [] });
  return c.json({ ok: true, devices: await devicesOf(accountOf(c)) });
});

accountApi.post("/device/rename", async (c) => {
  const deny = webOnly(c);
  if (deny) return deny;
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const deviceId = cleanDeviceId(b.deviceId) || (b.deviceId ?? "").toString().slice(0, 32);
  const name = cleanName(b.name);
  const rows = (await db()`
    UPDATE devices SET name = ${name} WHERE device_id = ${deviceId} AND account_id = ${accountOf(c)} RETURNING device_id`) as any[];
  return rows.length ? c.json({ ok: true }) : c.json({ ok: false, error: "no es tuyo", code: "not_found" }, 404);
});

accountApi.post("/device/delete", async (c) => {
  const deny = webOnly(c);
  if (deny) return deny;
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const deviceId = cleanDeviceId(b.deviceId) || (b.deviceId ?? "").toString().slice(0, 32);
  const rows = (await db()`
    DELETE FROM devices WHERE device_id = ${deviceId} AND account_id = ${accountOf(c)} RETURNING device_id`) as any[];
  return rows.length ? c.json({ ok: true }) : c.json({ ok: false, error: "no es tuyo", code: "not_found" }, 404);
});

// Cambiar la contraseña desde la web.
accountApi.post("/password", async (c) => {
  const deny = webOnly(c);
  if (deny) return deny;
  if (!multiUser) return c.json(NOT_MULTI, 501);
  const b = await readBody(c);
  const current = (b.current ?? "").toString();
  const next = (b.password ?? "").toString();
  if (next.length < MIN_PASSWORD) {
    return c.json({ ok: false, error: `la contraseña necesita al menos ${MIN_PASSWORD} caracteres`, code: "weak_password" }, 400);
  }
  const id = accountOf(c);
  const rows = (await db()`SELECT pass_hash FROM accounts WHERE id = ${id}`) as any[];
  if (!rows.length) return c.json({ ok: false, error: "no existe", code: "not_found" }, 404);
  const ok = await Bun.password.verify(current, String(rows[0].pass_hash)).catch(() => false);
  if (!ok) return c.json({ ok: false, error: "la contraseña actual no es esa", code: "bad_login" }, 401);
  await db()`UPDATE accounts SET pass_hash = ${await Bun.password.hash(next, "argon2id")} WHERE id = ${id}`;
  return c.json({ ok: true });
});

export { DEFAULT_ACCOUNT };
