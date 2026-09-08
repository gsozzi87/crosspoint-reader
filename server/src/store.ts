// Datos del asistente en un JSON del volumen de Railway (/data/store.json):
// recordatorios, listas de tareas (varias, por nombre), notas y la pizarra de
// mensajes. Alcanza para un usuario y una casa; si crece, se cambia por SQLite
// sin tocar a quien lo usa (voice.ts, hub.ts).
import { readJsonSafe, writeJsonAtomic } from "./fsjson";
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

export type Memory = { id: number; text: string; createdAt: string };

export type Feed = { id: number; name: string; url: string };

// Ajustes que se cambian desde /board y el aparato aplica al sincronizar.
// `rev` sube con cada cambio: el aparato solo pisa lo suyo cuando ve una
// revisión mayor a la que ya aplicó, así un cambio hecho en el aparato no se
// deshace en la próxima sincronización.
export type Settings = {
  rev: number;
  lang: string;           // idioma de la interfaz del aparato ("es", "en", ...)
  speak: "none" | "short" | "all";
  musicVolume: number;    // 0-100
  translatorLang: string; // el otro idioma del traductor
};

export const DEFAULT_SETTINGS: Settings = { rev: 0, lang: "es", speak: "short", musicVolume: 70, translatorLang: "en" };

export type Store = {
  nextId: number;
  settings?: Settings;  // ajustes del aparato, editables en /board
  memories?: Memory[]; // "acordate que ...": datos que el asistente tiene presentes al contestar
  feeds?: Feed[];      // RSS/Atom para Noticias (se cargan desde /board)
  reminders: Reminder[];
  lists: Record<string, Item[]>; // "Entrada", "Casa", "Trabajo", "Administrativo", "Compras", proyectos...
  notes: Note[];
  messages: Message[];
};

export const DEFAULT_LISTS = ["Entrada", "Casa", "Trabajo", "Administrativo", "Compras"];

export const REPEATS = ["none", "daily", "weekly", "monthly"] as const;

// Lo que diga el modelo (o la web) no manda: cualquier otra cosa es "none".
// Antes una cadena rara caía en el else de advanceRepeat y quedaba mensual.
export function normalizeRepeat(v: unknown): Reminder["repeat"] {
  return (REPEATS as readonly string[]).includes(v as string) ? (v as Reminder["repeat"]) : "none";
}

function emptyStore(): Store {
  return { nextId: 1, reminders: [], lists: {}, notes: [], messages: [] };
}

// El archivo puede venir de una versión vieja o quedar a medias: se acepta solo
// lo que tenga la forma esperada y el resto vuelve al default, así una lista que
// dejó de ser array no tira un 500 en cada pedido.
function normalizeStore(raw: unknown): Store {
  const base = emptyStore();
  const r = (raw && typeof raw === "object" && !Array.isArray(raw) ? raw : {}) as Record<string, any>;
  const arr = <T>(v: unknown): T[] => (Array.isArray(v) ? (v as T[]) : []);
  const store: Store = {
    nextId: Number.isFinite(Number(r.nextId)) && Number(r.nextId) > 0 ? Math.floor(Number(r.nextId)) : base.nextId,
    reminders: arr<Reminder>(r.reminders),
    lists: {},
    notes: arr<Note>(r.notes),
    messages: arr<Message>(r.messages),
    memories: arr<Memory>(r.memories),
    feeds: arr<Feed>(r.feeds),
  };
  for (const rem of store.reminders) rem.repeat = normalizeRepeat(rem.repeat);
  const lists = r.lists && typeof r.lists === "object" && !Array.isArray(r.lists) ? r.lists : {};
  for (const [name, items] of Object.entries(lists)) if (Array.isArray(items)) store.lists[name] = items as Item[];
  for (const name of DEFAULT_LISTS) store.lists[name] ??= [];
  store.settings = { ...DEFAULT_SETTINGS, ...(r.settings && typeof r.settings === "object" ? r.settings : {}) };
  // El nextId tiene que quedar arriba de todo lo que ya existe: si el archivo
  // vino truncado, repetir ids mezcla ítems de listas distintas.
  const ids = [...store.reminders, ...store.notes, ...store.messages, ...(store.memories ?? []), ...(store.feeds ?? []), ...Object.values(store.lists).flat()]
    .map((e: { id?: number }) => Number(e?.id) || 0);
  store.nextId = Math.max(store.nextId, ...ids.map((i) => i + 1), 1);
  return store;
}

let cache: Store | null = null;
let loading: Promise<Store> | null = null;

export function load(): Promise<Store> {
  if (cache) return Promise.resolve(cache);
  // La promesa se cachea, no el resultado: dos pedidos juntos leían el archivo
  // dos veces y se quedaban con dos objetos distintos (lo que guardaba uno lo
  // pisaba el otro).
  loading ??= readJsonSafe<unknown>(FILE, null).then((raw) => {
    cache = normalizeStore(raw);
    loading = null;
    return cache;
  });
  return loading;
}

export async function save(store: Store): Promise<void> {
  cache = store;
  await writeJsonAtomic(FILE, store);
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
  const repeat = normalizeRepeat(r.repeat);
  if (repeat === "none" || !r.dueAt) return false;
  const [date, time] = r.dueAt.split("T");
  const d = new Date(date + "T00:00:00Z");
  if (repeat === "daily") d.setUTCDate(d.getUTCDate() + 1);
  else if (repeat === "weekly") d.setUTCDate(d.getUTCDate() + 7);
  else d.setUTCMonth(d.getUTCMonth() + 1);
  r.dueAt = d.toISOString().slice(0, 10) + (time ? "T" + time : "");
  return true;
}

export function pendingReminders(store: Store): Reminder[] {
  return store.reminders
    .filter((r) => !r.done)
    .sort((a, b) => (a.dueAt ?? "9999").localeCompare(b.dueAt ?? "9999"));
}
