// Trabajos de las apps de Lua: lo que tarda minutos no cabe en un POST.
//
// Generar un librito son seis u ocho llamadas al modelo de un minuto cada una,
// y el aparato corta a los 40 s (ya vimos lo que pasa cuando espera más). Así
// que un servicio largo devuelve un `jobId` en el acto y el aparato pregunta
// `job.status` cada 5 s desde `on_tick`, con la pantalla viva y el reposo
// andando; si se va, la próxima vez que entre a la app retoma por el id que
// guardó en su estado.
//
// Cada cuenta tiene su documento `apps-jobs` (con base de datos, una fila por
// cuenta; sin ella, /data/apps-jobs.json) y sus archivos en
// /data/apps-files/<cuenta>/<jobId>/<nombre>. El accountId es un número, así
// que ninguna ruta puede escaparse del directorio.
//
// Un trabajo NO sobrevive a un reinicio del servidor: la promesa que lo corría
// murió con el proceso y lo escrito a medias no se retoma (un capítulo a medio
// escribir no vale nada). Al leerlo se lo marca `failed` con "el servidor se
// reinició" y la app lo vuelve a pedir. Todo se poda a las 24 h: el aparato ya
// se bajó el archivo, y si no, lo genera de nuevo.
import { mkdir, readdir, rm, stat } from "node:fs/promises";
import { randomBytes } from "node:crypto";
import { DEFAULT_ACCOUNT } from "./db";
import { mutateDoc, readDoc, writeBytesAtomic } from "./fsjson";

const FILES_DIR = process.env.APPS_FILES_DIR ?? "/data/apps-files";
const MAX_AGE_MS = 24 * 60 * 60 * 1000;
const PRUNE_EVERY_MS = 60 * 60 * 1000;
const MAX_JOBS_PER_ACCOUNT = 40;
const MAX_FILE_BYTES = 8 * 1024 * 1024;

export const RESTARTED_MSG = "el servidor se reinició";

export type JobState = "running" | "done" | "failed";
export type JobFile = { id: string; name: string; bytes: number };

export type Job = {
  id: string;
  accountId: number;
  label: string;
  state: JobState;
  step: number;
  total: number;
  files: JobFile[];
  error: string;
  createdAt: number;
  updatedAt: number;
};

type Doc = { jobs: Job[] };

function shape(raw: unknown): Doc {
  const obj = raw && typeof raw === "object" ? (raw as Partial<Doc>) : {};
  const jobs = Array.isArray(obj.jobs) ? obj.jobs.filter((j) => j && typeof j === "object" && typeof (j as Job).id === "string") : [];
  return { jobs: jobs as Job[] };
}

// Lo que un `run` puede tocar mientras trabaja.
export type JobHandle = {
  id: string;
  accountId: number;
  /** "Capítulo 3 de 6: título" — lo que el aparato muestra mientras espera. */
  setProgress(step: number, total: number, label: string): Promise<void>;
  /** Guarda un archivo del trabajo y devuelve su ficha para `files`. */
  saveFile(name: string, bytes: Uint8Array): Promise<JobFile>;
};

// Los que este proceso está corriendo de verdad. Un `running` que no esté acá
// es de un proceso anterior: se reinició el servidor.
const active = new Set<string>();
// Cuentas vistas por este proceso: son las que se podan cada hora. Las demás
// se podan al primer pedido (`jobStatus` poda antes de contestar).
const seen = new Set<number>([DEFAULT_ACCOUNT]);

const FILE_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$/;

function newId(): string {
  return randomBytes(8).toString("hex");
}

function jobDir(accountId: number, jobId: string): string {
  return `${FILES_DIR}/${accountId | 0}/${jobId}`;
}

async function patch(accountId: number, id: string, fn: (job: Job) => void): Promise<void> {
  await mutateDoc(accountId, "apps-jobs", shape, (doc) => {
    const job = doc.jobs.find((j) => j.id === id);
    if (!job) return;
    fn(job);
    job.updatedAt = Date.now();
  });
}

/**
 * Arranca un trabajo y devuelve su id en el acto. `run` corre en segundo plano
 * y devuelve los archivos que dejó; si tira, el trabajo queda `failed` con el
 * mensaje de la excepción (corto: vuelve al aparato tal cual).
 */
export async function startJob(accountId: number, label: string, run: (job: JobHandle) => Promise<JobFile[]>): Promise<string> {
  seen.add(accountId);
  const id = newId();
  const now = Date.now();
  const job: Job = { id, accountId, label, state: "running", step: 0, total: 0, files: [], error: "", createdAt: now, updatedAt: now };
  await mutateDoc(accountId, "apps-jobs", shape, (doc) => {
    doc.jobs.push(job);
    // Tope por cuenta: lo más viejo se va (y sus archivos, en la poda).
    while (doc.jobs.length > MAX_JOBS_PER_ACCOUNT) doc.jobs.shift();
  });
  active.add(id);

  const handle: JobHandle = {
    id,
    accountId,
    setProgress: (step, total, text) => patch(accountId, id, (j) => {
      j.step = step;
      j.total = total;
      j.label = text;
    }),
    saveFile: async (name, bytes) => {
      if (!FILE_NAME.test(name)) throw new Error(`nombre de archivo inválido: ${name.slice(0, 40)}`);
      if (bytes.byteLength > MAX_FILE_BYTES) throw new Error("el archivo generado es demasiado grande");
      const dir = jobDir(accountId, id);
      await mkdir(dir, { recursive: true });
      await writeBytesAtomic(`${dir}/${name}`, bytes);
      return { id: newId(), name, bytes: bytes.byteLength };
    },
  };

  void run(handle)
    .then((files) => patch(accountId, id, (j) => {
      j.state = "done";
      j.files = files;
      j.step = j.total;
    }))
    .catch((err) => {
      const msg = (err instanceof Error ? err.message : String(err)).slice(0, 200);
      console.error(`apps job ${id} (${label}):`, msg);
      return patch(accountId, id, (j) => {
        j.state = "failed";
        j.error = msg || "falló";
      });
    })
    .finally(() => active.delete(id));

  return id;
}

/** El estado de un trabajo, o null si no es de esta cuenta. */
export async function jobStatus(accountId: number, id: string): Promise<Job | null> {
  if (!seen.has(accountId)) {
    seen.add(accountId);
    await pruneAccount(accountId).catch((err) => console.error("apps jobs prune:", err));
  }
  const doc = await readDoc<Doc | null>(accountId, "apps-jobs", null);
  const job = shape(doc).jobs.find((j) => j.id === id);
  if (!job) return null;
  if (job.state === "running" && !active.has(id)) {
    // De un proceso anterior: no hay quién lo termine.
    await patch(accountId, id, (j) => {
      j.state = "failed";
      j.error = RESTARTED_MSG;
    });
    return { ...job, state: "failed", error: RESTARTED_MSG };
  }
  return job;
}

/** La ruta en disco de un archivo generado, si es de esta cuenta y está. */
export async function jobFile(accountId: number, fileId: string): Promise<{ path: string; name: string; bytes: number } | null> {
  if (!/^[0-9a-f]{16}$/.test(fileId)) return null;
  const doc = shape(await readDoc<Doc | null>(accountId, "apps-jobs", null));
  for (const job of doc.jobs) {
    if (job.state !== "done") continue;
    const f = job.files.find((x) => x.id === fileId);
    if (f) return { path: `${jobDir(accountId, job.id)}/${f.name}`, name: f.name, bytes: f.bytes };
  }
  return null;
}

// ── Poda ────────────────────────────────────────────────────────────────────

async function pruneAccount(accountId: number): Promise<void> {
  const cutoff = Date.now() - MAX_AGE_MS;
  const gone = await mutateDoc(accountId, "apps-jobs", shape, (doc) => {
    const old = doc.jobs.filter((j) => j.createdAt < cutoff && !active.has(j.id));
    doc.jobs = doc.jobs.filter((j) => !old.includes(j));
    return old.map((j) => j.id);
  });
  for (const id of gone) await rm(jobDir(accountId, id), { recursive: true, force: true }).catch(() => {});
}

// Los directorios de archivos de CUALQUIER cuenta, por fecha de modificación:
// cubre las cuentas que este proceso no vio (con base de datos no se pueden
// enumerar sin recorrer la tabla) y lo que quedó de un trabajo borrado a
// medias. Un directorio sin dueño de más de un día no lo va a pedir nadie.
async function pruneDirs(): Promise<void> {
  const cutoff = Date.now() - MAX_AGE_MS;
  let accounts: string[];
  try {
    accounts = await readdir(FILES_DIR);
  } catch {
    return;
  }
  for (const acc of accounts) {
    if (!/^\d+$/.test(acc)) continue;
    let jobs: string[] = [];
    try {
      jobs = await readdir(`${FILES_DIR}/${acc}`);
    } catch {
      continue;
    }
    for (const id of jobs) {
      if (active.has(id)) continue;
      const dir = `${FILES_DIR}/${acc}/${id}`;
      try {
        const st = await stat(dir);
        if (st.mtimeMs < cutoff) await rm(dir, { recursive: true, force: true });
      } catch {
        // desapareció en el medio: no hay nada que podar
      }
    }
  }
}

export async function pruneJobs(): Promise<void> {
  for (const acc of seen) await pruneAccount(acc).catch((err) => console.error("apps jobs prune:", err));
  await pruneDirs().catch((err) => console.error("apps files prune:", err));
}

let timer: ReturnType<typeof setInterval> | null = null;

/** Al arrancar y una vez por hora: trabajos y archivos de más de 24 h. */
export function startJobsJanitor(): void {
  if (timer) return;
  void pruneJobs();
  timer = setInterval(() => void pruneJobs(), PRUNE_EVERY_MS);
}
