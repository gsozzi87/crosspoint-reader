// Postgres opcional: multiusuario.
//
// REGLA DE ORO: sin `DATABASE_URL` en el entorno, este módulo no hace NADA y el
// servidor se comporta exactamente como siempre (archivos sueltos en /data, un
// solo DEVICE_TOKEN, sin login). Poner `DATABASE_URL` es lo que enciende el modo
// multiusuario. La bifurcación vive acá y en `fsjson.ts` (readDoc/writeDoc); el
// resto del servidor solo recibe un `accountId` y no sabe de dónde salen los
// datos.
//
// Tablas (todas se crean con CREATE TABLE IF NOT EXISTS al arrancar, sin
// herramienta de migraciones):
//
//   accounts   una cuenta de correo + contraseña (la de la web)
//   devices    un aparato vinculado a una cuenta; se guarda el SHA-256 del
//              token, NUNCA el token
//   pairings   códigos de 6 dígitos efímeros para vincular un aparato
//   docs       (account_id, name) -> el MISMO JSON que antes era un archivo:
//              store.json, calendar.json, trips.json, suggest.json,
//              hub-settings.json, hub-data.json, attachments/index.json
//   usage      consumo mensual por cuenta (llamadas al LLM y segundos de audio)
//   server_meta  cositas del operador (hoy: la clave con la que se firman las
//              cookies de sesión, para que un redeploy no eche a todo el mundo)
import { SQL } from "bun";

export const DATABASE_URL = process.env.DATABASE_URL ?? "";

/** true = modo multiusuario (hay Postgres). false = todo como siempre. */
export const multiUser = !!DATABASE_URL;

/** La cuenta del usuario que ya estaba andando antes de que existiera el login. */
export const DEFAULT_ACCOUNT = 1;

let client: SQL | null = null;

/** El cliente. Solo se puede llamar en modo multiusuario. */
export function db(): SQL {
  if (!client) throw new Error("no hay DATABASE_URL: el servidor está en modo de un solo usuario");
  return client;
}

// bigserial vuelve como string en el driver: siempre pasar por acá.
export function num(v: unknown): number {
  return Number(v ?? 0) || 0;
}

export function sha256Hex(s: string): string {
  return new Bun.CryptoHasher("sha256").update(s, "utf8").digest("hex");
}

// ─────────────────────────────────────────────────────────── esquema

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS accounts (
     id         bigserial PRIMARY KEY,
     email      text UNIQUE NOT NULL,
     pass_hash  text NOT NULL,
     created_at timestamptz DEFAULT now(),
     is_admin   boolean DEFAULT false
   )`,
  `CREATE TABLE IF NOT EXISTS devices (
     id         bigserial PRIMARY KEY,
     account_id bigint REFERENCES accounts(id) ON DELETE CASCADE,
     device_id  text UNIQUE NOT NULL,
     token_hash text UNIQUE NOT NULL,
     name       text,
     last_seen  timestamptz,
     created_at timestamptz DEFAULT now()
   )`,
  `CREATE INDEX IF NOT EXISTS devices_account_idx ON devices(account_id)`,
  `CREATE TABLE IF NOT EXISTS pairings (
     code       text PRIMARY KEY,
     device_id  text NOT NULL,
     token_hash text NOT NULL,
     created_at timestamptz DEFAULT now()
   )`,
  `CREATE INDEX IF NOT EXISTS pairings_device_idx ON pairings(device_id)`,
  `CREATE TABLE IF NOT EXISTS docs (
     account_id bigint REFERENCES accounts(id) ON DELETE CASCADE,
     name       text NOT NULL,
     data       jsonb NOT NULL,
     updated_at timestamptz DEFAULT now(),
     PRIMARY KEY (account_id, name)
   )`,
  `CREATE TABLE IF NOT EXISTS usage (
     account_id  bigint,
     month       date,
     llm_calls   int DEFAULT 0,
     stt_seconds int DEFAULT 0,
     PRIMARY KEY (account_id, month)
   )`,
  `CREATE TABLE IF NOT EXISTS server_meta (
     key   text PRIMARY KEY,
     value text NOT NULL
   )`,
];

let ready: Promise<void> | null = null;

/**
 * Conecta, crea lo que falte y migra los archivos de /data a la cuenta 1 la
 * primera vez. Sin DATABASE_URL no hace nada y devuelve false.
 * Idempotente: correrlo de nuevo no duplica ni pisa nada.
 */
export function initDb(): Promise<void> {
  ready ??= (async () => {
    if (!multiUser) return;
    client = new SQL(DATABASE_URL);
    for (const stmt of SCHEMA) await client.unsafe(stmt);
    console.log("db: esquema al día (modo multiusuario)");
  })();
  return ready;
}

// ─────────────────────────────────────────────────────────── server_meta

export async function metaGet(key: string): Promise<string | null> {
  const rows = (await db()`SELECT value FROM server_meta WHERE key = ${key}`) as { value: string }[];
  return rows.length ? rows[0].value : null;
}

/** Guarda solo si no estaba: dos procesos que arrancan juntos no se pisan. */
export async function metaSetIfAbsent(key: string, value: string): Promise<string> {
  await db()`INSERT INTO server_meta (key, value) VALUES (${key}, ${value}) ON CONFLICT (key) DO NOTHING`;
  return (await metaGet(key)) ?? value;
}

// ─────────────────────────────────────────────────────────── docs

/**
 * Un documento de una cuenta (lo que antes era un archivo JSON de /data).
 * Devuelve `fallback` si no existe.
 */
export async function dbReadDoc<T>(accountId: number, name: string, fallback: T): Promise<T> {
  const rows = (await db()`SELECT data FROM docs WHERE account_id = ${accountId} AND name = ${name}`) as { data: unknown }[];
  if (!rows.length) return fallback;
  const data = rows[0].data;
  return (data === null || data === undefined ? fallback : (data as T));
}

/** INSERT ... ON CONFLICT DO UPDATE: atómico de verdad, incluso entre procesos. */
export async function dbWriteDoc(accountId: number, name: string, obj: unknown): Promise<void> {
  const value = obj === undefined || obj === null ? {} : obj;
  await db()`
    INSERT INTO docs (account_id, name, data, updated_at)
    VALUES (${accountId}, ${name}, ${value as any}, now())
    ON CONFLICT (account_id, name) DO UPDATE SET data = EXCLUDED.data, updated_at = now()`;
}

/**
 * Leer-modificar-escribir de un documento en una transacción con SELECT ... FOR
 * UPDATE: es el equivalente de la cola de `serialize()` de los archivos, pero
 * sirve además entre procesos (dos réplicas de Railway).
 *
 * `shape` normaliza lo que vino de la base (cada módulo tiene su normalizador);
 * `fn` recibe ese objeto y lo modifica in place. Lo que devuelva `fn` es lo que
 * devuelve esta función.
 */
export async function dbMutateDoc<D, R>(
  accountId: number,
  name: string,
  shape: (raw: unknown) => D,
  fn: (doc: D) => R | Promise<R>,
): Promise<R> {
  const out = await db().begin(async (tx) => {
    // Un candado por (cuenta, documento) que dura lo que dura la transacción.
    // Con SELECT ... FOR UPDATE solo no alcanza: la PRIMERA escritura de un
    // documento no tiene fila que bloquear, y dos pedidos a la vez terminaban
    // los dos en el INSERT y uno se comía al otro. El candado es de Postgres, o
    // sea que ordena también a dos réplicas.
    await tx`SELECT pg_advisory_xact_lock(hashtextextended(${`${accountId}:${name}`}, 0))`;
    const rows = (await tx`SELECT data FROM docs WHERE account_id = ${accountId} AND name = ${name} FOR UPDATE`) as { data: unknown }[];
    const doc = shape(rows.length ? rows[0].data : null);
    const result = await fn(doc);
    await tx`
      INSERT INTO docs (account_id, name, data, updated_at)
      VALUES (${accountId}, ${name}, ${doc as any}, now())
      ON CONFLICT (account_id, name) DO UPDATE SET data = EXCLUDED.data, updated_at = now()`;
    return { result };
  });
  return (out as unknown as { result: R }).result;
}
