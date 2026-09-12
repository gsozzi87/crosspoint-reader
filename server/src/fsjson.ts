// JSON del volumen sin perder datos. Todo lo del usuario (recordatorios,
// listas, notas, claves) vive en archivos sueltos de /data:
//   - escribir directo deja el archivo a medias si el contenedor se cae en el
//     medio, y en el próximo arranque ese JSON roto se leía como store vacío,
//     o sea que se borraba todo en silencio;
//   - dos pedidos a la vez se pisaban entre ellos (el último ganaba).
// Acá se escribe a .tmp + rename (atómico en el mismo volumen), las escrituras
// de un archivo se serializan en una cola, y un JSON ilegible se guarda aparte
// como .corrupt-<fecha> en vez de desaparecer.
//
// Además, acá está la ÚNICA bifurcación entre el modo de siempre (archivos) y
// el modo multiusuario (Postgres): `readDoc` / `writeDoc` / `mutateDoc`. Los
// módulos de datos (store, calendar, trips, suggest, hub) reciben un
// `accountId` y llaman a estas tres; no saben ni les importa dónde termina el
// JSON. Sin `DATABASE_URL` el `accountId` se ignora y todo va a los mismos
// archivos de /data que hasta hoy.
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import { DEFAULT_ACCOUNT, dbMutateDoc, dbReadDoc, dbWriteDoc, multiUser } from "./db";

const queues = new Map<string, Promise<unknown>>();

// Una promesa encadenada por archivo: nadie escribe mientras otro escribe.
export function serialize<T>(path: string, job: () => Promise<T>): Promise<T> {
  const prev = queues.get(path) ?? Promise.resolve();
  const next = prev.then(job, job);
  const settled = next.then(() => {}, () => {});
  queues.set(path, settled);
  void settled.then(() => {
    if (queues.get(path) === settled) queues.delete(path);
  });
  return next;
}

// Sin cola: solo para usar adentro de un serialize() del mismo archivo (si no,
// encolar dentro de la cola del mismo archivo se traba solo).
export async function writeAtomicNow(path: string, data: string | Uint8Array): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  const tmp = `${path}.tmp`;
  await writeFile(tmp, data);
  await rename(tmp, path);
}

export function writeJsonAtomic(path: string, obj: unknown): Promise<void> {
  const data = JSON.stringify(obj, null, 2);  // se arma antes de la cola: la vista de ahora, no la de después
  return serialize(path, () => writeAtomicNow(path, data));
}

export function writeTextAtomic(path: string, text: string): Promise<void> {
  return serialize(path, () => writeAtomicNow(path, text));
}

export function writeBytesAtomic(path: string, bytes: Uint8Array): Promise<void> {
  return serialize(path, () => writeAtomicNow(path, bytes));
}

// Devuelve `fallback` si el archivo no está; si está pero no parsea, lo aparta
// con nombre nuevo y lo dice en el log (borrarlo sería tirar los datos del
// usuario sin que se entere).
export async function readJsonSafe<T>(path: string, fallback: T): Promise<T> {
  let raw: string;
  try {
    raw = await readFile(path, "utf8");
  } catch {
    return fallback;
  }
  try {
    return JSON.parse(raw) as T;
  } catch (err) {
    const kept = `${path}.corrupt-${Date.now()}`;
    await rename(path, kept).catch(() => {});
    console.error(`json roto en ${path}: guardado como ${kept}`, err);
    return fallback;
  }
}

// ─────────────────────────────────────── documentos por cuenta

// Cada JSON que antes era un archivo suelto de /data es ahora un documento con
// nombre. En modo multiusuario es una fila `docs(account_id, name)`; sin base de
// datos es el archivo de siempre (y el accountId se ignora).
export type DocName =
  | "store"
  | "calendar"
  | "trips"
  | "suggest"
  | "hub-settings"
  | "hub-data"
  | "attachments";

const ATTACHMENTS_DIR = process.env.ATTACHMENTS_DIR ?? "/data/attachments";

const LEGACY_FILE: Record<DocName, string> = {
  store: process.env.STORE_FILE ?? "/data/store.json",
  calendar: process.env.CALENDAR_FILE ?? "/data/calendar.json",
  trips: process.env.TRIPS_FILE ?? "/data/trips.json",
  suggest: process.env.SUGGEST_FILE ?? "/data/suggest.json",
  "hub-settings": process.env.HUB_SETTINGS_FILE ?? "/data/hub-settings.json",
  "hub-data": process.env.HUB_DATA_FILE ?? "/data/hub-data.json",
  attachments: `${ATTACHMENTS_DIR}/index.json`,
};

export const DOC_NAMES = Object.keys(LEGACY_FILE) as DocName[];

/** El archivo de /data donde vivía (y sigue viviendo sin base de datos) un documento. */
export function legacyFile(name: DocName): string {
  return LEGACY_FILE[name];
}

export async function readDoc<T>(accountId: number, name: DocName, fallback: T): Promise<T> {
  if (multiUser) return dbReadDoc<T>(accountId, name, fallback);
  return readJsonSafe<T>(LEGACY_FILE[name], fallback);
}

export async function writeDoc(accountId: number, name: DocName, obj: unknown): Promise<void> {
  if (multiUser) return dbWriteDoc(accountId, name, obj);
  return writeJsonAtomic(LEGACY_FILE[name], obj);
}

/**
 * Leer-modificar-escribir sin carreras. Con archivos es la cola de `serialize()`
 * de siempre; con base de datos es una transacción con SELECT ... FOR UPDATE
 * (que además ordena a dos procesos, cosa que la cola no puede).
 *
 * `shape` normaliza lo que se leyó (cada módulo tiene su normalizador) y `fn` lo
 * modifica in place; lo que devuelve `fn` es lo que devuelve esta función.
 */
export async function mutateDoc<D, R>(
  accountId: number,
  name: DocName,
  shape: (raw: unknown) => D,
  fn: (doc: D) => R | Promise<R>,
): Promise<R> {
  if (multiUser) return dbMutateDoc(accountId, name, shape, fn);
  const file = LEGACY_FILE[name];
  return serialize(file, async () => {
    const doc = shape(await readJsonSafe<unknown>(file, null));
    const out = await fn(doc);
    await writeAtomicNow(file, JSON.stringify(doc, null, 2));
    return out;
  });
}

// ─────────────────────────────────────── archivos por cuenta

// Fotos, adjuntos y el log del aparato no son JSON: son archivos. La cuenta 1
// (la que ya venía andando) se queda en las rutas de siempre para no tener que
// mover nada; las cuentas nuevas viven bajo /data/accounts/<id>/.
// El accountId es un número, así que ninguna ruta puede escaparse del directorio.
const PHOTOS_DIR = process.env.PHOTOS_DIR ?? "/data/photos";
const DEVICE_LOG_FILE = process.env.DEVICE_LOG_FILE ?? "/data/device.log";
const ACCOUNTS_DIR = process.env.ACCOUNTS_DIR ?? "/data/accounts";

export function photosDir(accountId: number): string {
  return accountId === DEFAULT_ACCOUNT ? PHOTOS_DIR : `${ACCOUNTS_DIR}/${accountId | 0}/photos`;
}

export function attachmentsDir(accountId: number): string {
  return accountId === DEFAULT_ACCOUNT ? ATTACHMENTS_DIR : `${ACCOUNTS_DIR}/${accountId | 0}/attachments`;
}

export function deviceLogFile(accountId: number): string {
  return accountId === DEFAULT_ACCOUNT ? DEVICE_LOG_FILE : `${ACCOUNTS_DIR}/${accountId | 0}/device.log`;
}
