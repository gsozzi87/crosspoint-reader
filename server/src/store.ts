// Datos del asistente en un JSON del volumen de Railway (/data/store.json):
// recordatorios, listas de tareas (varias, por nombre), notas y la pizarra de
// mensajes. Alcanza para un usuario y una casa; si crece, se cambia por SQLite
// sin tocar a quien lo usa (voice.ts, hub.ts).
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import { LABELS, type Lang } from "./lang";

const FILE = process.env.STORE_FILE ?? "/data/store.json";

export type Reminder = {
  id: number;
  title: string;
  dueAt: string | null; // ISO local sin zona ("2026-09-07T10:30") o null si no tiene hora
  repeat: "none" | "daily" | "weekly" | "monthly";
  done: boolean;
  createdAt: string;
};
export type Item = { id: number; text: string; done: boolean; dueDate: string | null; createdAt: string };
export type Note = { id: number; text: string; createdAt: string };
export type Message = { id: number; from: string; text: string; createdAt: string; read: boolean };

export type Store = {
  nextId: number;
  reminders: Reminder[];
  lists: Record<string, Item[]>; // "Entrada", "Casa", "Trabajo", "Administrativo", "Compras", proyectos...
  notes: Note[];
  messages: Message[];
};

export const DEFAULT_LISTS = ["Entrada", "Casa", "Trabajo", "Administrativo", "Compras"];

let cache: Store | null = null;

export async function load(): Promise<Store> {
  if (cache) return cache;
  try {
    cache = JSON.parse(await readFile(FILE, "utf8")) as Store;
  } catch {
    cache = { nextId: 1, reminders: [], lists: {}, notes: [], messages: [] };
  }
  for (const name of DEFAULT_LISTS) cache.lists[name] ??= [];
  return cache;
}

export async function save(store: Store): Promise<void> {
  cache = store;
  await mkdir(dirname(FILE), { recursive: true });
  await writeFile(FILE, JSON.stringify(store, null, 2));
}

export function nextId(store: Store): number {
  return store.nextId++;
}

// Nombre de lista tal como lo dijo el usuario -> nombre canónico (sin
// distinguir mayúsculas ni acentos). Crea la lista si no existe y `create`.
export function resolveList(store: Store, spoken: string | null | undefined, create: boolean): string {
  const norm = (s: string) => s.normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase().trim();
  const wanted = norm(spoken ?? "");
  if (!wanted) return "Entrada";
  for (const name of Object.keys(store.lists)) if (norm(name) === wanted) return name;
  if (!create) return "Entrada";
  const pretty = spoken!.trim().replace(/^\w/, (c) => c.toUpperCase());
  store.lists[pretty] = [];
  return pretty;
}

// "2026-09-07T10:30" -> texto corto para la pantalla del aparato, relativo a hoy.
export function whenLabel(dueAt: string | null, lang: Lang = "es", now = new Date()): string {
  if (!dueAt) return "";
  const [date, time] = dueAt.split("T");
  const today = now.toISOString().slice(0, 10);
  const tomorrow = new Date(now.getTime() + 86_400_000).toISOString().slice(0, 10);
  const l = LABELS[lang];
  const day = date === today ? l.today : date === tomorrow ? l.tomorrow : date.slice(8, 10) + "/" + date.slice(5, 7);
  return time ? `${day} ${time}` : day;
}

const TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";

// Desfase (ms) de la zona HUB_TZ respecto de UTC en un instante dado.
function tzOffsetMs(at: number): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TZ, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  const asUtc = Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second"));
  return asUtc - Math.floor(at / 1000) * 1000;
}

// "YYYY-MM-DDTHH:MM" (hora local de HUB_TZ) -> epoch UTC en segundos. Sin hora: 09:00.
export function localToEpoch(dueAt: string | null): number {
  if (!dueAt) return 0;
  const m = /^(\d{4})-(\d{2})-(\d{2})(?:T(\d{2}):(\d{2}))?/.exec(dueAt);
  if (!m) return 0;
  const [y, mo, d] = [Number(m[1]), Number(m[2]), Number(m[3])];
  const h = m[4] !== undefined ? Number(m[4]) : 9;
  const mi = m[5] !== undefined ? Number(m[5]) : 0;
  const guess = Date.UTC(y, mo - 1, d, h, mi);
  const first = guess - tzOffsetMs(guess);
  return Math.floor((guess - tzOffsetMs(first)) / 1000);
}

export function epochToLocal(epoch: number): string {
  const ms = epoch * 1000;
  const local = new Date(ms + tzOffsetMs(ms));
  return local.toISOString().slice(0, 16);
}

// Recordatorio con repetición: corre la fecha al próximo ciclo (mantiene la hora).
export function advanceRepeat(r: Reminder): boolean {
  if (r.repeat === "none" || !r.dueAt) return false;
  const [date, time] = r.dueAt.split("T");
  const d = new Date(date + "T00:00:00Z");
  if (r.repeat === "daily") d.setUTCDate(d.getUTCDate() + 1);
  else if (r.repeat === "weekly") d.setUTCDate(d.getUTCDate() + 7);
  else d.setUTCMonth(d.getUTCMonth() + 1);
  r.dueAt = d.toISOString().slice(0, 10) + (time ? "T" + time : "");
  return true;
}

export function pendingReminders(store: Store): Reminder[] {
  return store.reminders
    .filter((r) => !r.done)
    .sort((a, b) => (a.dueAt ?? "9999").localeCompare(b.dueAt ?? "9999"));
}
