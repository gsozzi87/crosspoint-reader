// JSON del volumen sin perder datos. Todo lo del usuario (recordatorios,
// listas, notas, claves) vive en archivos sueltos de /data:
//   - escribir directo deja el archivo a medias si el contenedor se cae en el
//     medio, y en el próximo arranque ese JSON roto se leía como store vacío,
//     o sea que se borraba todo en silencio;
//   - dos pedidos a la vez se pisaban entre ellos (el último ganaba).
// Acá se escribe a .tmp + rename (atómico en el mismo volumen), las escrituras
// de un archivo se serializan en una cola, y un JSON ilegible se guarda aparte
// como .corrupt-<fecha> en vez de desaparecer.
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const queues = new Map<string, Promise<unknown>>();

// Una promesa encadenada por archivo: nadie escribe mientras otro escribe.
export function serialize<T>(path: string, job: () => Promise<T>): Promise<T> {
  const prev = queues.get(path) ?? Promise.resolve();
  const next = prev.then(job, job);
  queues.set(path, next.then(() => {}, () => {}));
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
