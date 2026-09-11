// Datos del asistente: recordatorios, DOS listas (compras y tareas) y notas.
//
// Vive en el documento "store" de la cuenta: sin `DATABASE_URL` eso es el
// /data/store.json de siempre, y con base de datos es la fila
// docs(account_id, "store") con exactamente el mismo contenido. Lo único que
// cambió acá es que hay que decir DE QUÉ CUENTA: `load(accountId)`.
//
// No hay pizarra de mensajes: se sacó del producto. Un store.json viejo que
// todavía traiga "messages" se lee igual y esa clave se ignora.
import { AsyncLocalStorage } from "node:async_hooks";
import { mutateDoc, readDoc, writeDoc } from "./fsjson";
import { LABELS, type Lang } from "./lang";

export type RepeatKind = "none" | "daily" | "weekdays" | "weekly" | "monthly" | "yearly";

// Repetición de un recordatorio (y de un evento del calendario). Es lo que el
// usuario tiene que poder VER y CAMBIAR en el aparato: "¿me despierta mañana y
// pasado, o de lunes a viernes?". Por eso, además del dato, está repeatText(),
// que lo dice en una línea en el idioma del aparato.
//
//   kind     none | daily | weekdays (lunes a viernes) | weekly | monthly | yearly
//   days     0=domingo .. 6=sábado; solo weekly ("los martes y jueves")
//   interval cada N días/semanas/meses/años (default 1)
//   until    epoch UTC en segundos; el último día con ocurrencia (opcional)
export type Repeat = { kind: RepeatKind; days?: number[]; interval?: number; until?: number };

export type Reminder = {
  id: number;
  title: string;
  dueAt: string | null; // ISO local sin zona ("2026-09-07T10:30") o null si no tiene hora
  repeat: Repeat;
  done: boolean;
  createdAt: string;
};
export type Item = { id: number; text: string; done: boolean; dueDate: string | null; createdAt: string };
export type Note = { id: number; text: string; createdAt: string };

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
  lists: Record<string, Item[]>; // solo SHOPPING_LIST y TASK_LIST
  notes: Note[];
};

// Las listas son dos y nada más: la de compras y la de tareas (to-do). El
// nombre guardado es siempre el canónico en español (la clave del archivo, que
// no cambia si el usuario cambia el idioma del aparato); lo que se MUESTRA sale
// de LABELS según el idioma (listLabel()).
export const SHOPPING_LIST = "Compras";
export const TASK_LIST = "Tareas";
export const DEFAULT_LISTS = [SHOPPING_LIST, TASK_LIST];

export const REPEAT_KINDS = ["none", "daily", "weekdays", "weekly", "monthly", "yearly"] as const;
export const NO_REPEAT: Repeat = { kind: "none" };

// Lo que diga el modelo (o la web) no manda: cualquier otra cosa es "none".
// Antes una cadena rara caía en el else de advanceRepeat y quedaba mensual.
//
// MIGRACIÓN: hasta 1.5.x la repetición era una cadena suelta
// ("none"|"daily"|"weekly"|"monthly"). Se sigue aceptando y se convierte al
// objeto nuevo sin perder nada: "weekly" queda {kind:"weekly", interval:1} y,
// como no trae días, repite el mismo día de la semana del dueAt — exactamente
// lo que hacía antes.
export function normalizeRepeat(v: unknown): Repeat {
  if (typeof v === "string") {
    const kind = (REPEAT_KINDS as readonly string[]).includes(v) ? (v as RepeatKind) : "none";
    return kind === "none" ? { kind: "none" } : { kind, interval: 1 };
  }
  if (!v || typeof v !== "object" || Array.isArray(v)) return { kind: "none" };
  const r = v as Record<string, unknown>;
  const raw = String(r.kind ?? "");
  const kind = (REPEAT_KINDS as readonly string[]).includes(raw) ? (raw as RepeatKind) : "none";
  if (kind === "none") return { kind: "none" };
  const interval = Math.floor(Number(r.interval));
  const out: Repeat = { kind, interval: Number.isFinite(interval) && interval >= 1 ? Math.min(interval, 99) : 1 };
  if (kind === "weekly" && Array.isArray(r.days)) {
    const days = Array.from(
      new Set(r.days.map((d) => Math.floor(Number(d))).filter((d) => Number.isInteger(d) && d >= 0 && d <= 6)),
    ).sort((a, b) => a - b);
    // Los siete días marcados son "todos los días": se guarda como daily para
    // que el texto no diga "Los domingos, lunes, martes, ...".
    if (days.length === 7 && out.interval === 1) return { kind: "daily", interval: 1, ...(untilEpoch(r.until) ? { until: untilEpoch(r.until)! } : {}) };
    if (days.length) out.days = days;
  }
  // `until` puede venir como epoch (lo que se guarda) o como fecha suelta
  // "YYYY-MM-DD" (lo que manda un formulario o el clasificador de voz).
  const until = untilEpoch(r.until);
  if (until) out.until = until;
  return out;
}

function untilEpoch(v: unknown): number | null {
  if (typeof v === "string" && /^\d{4}-\d{2}-\d{2}$/.test(v)) return localToEpoch(v + "T23:59") || null;
  return numOrNull(v);
}

function numOrNull(v: unknown): number | null {
  const n = Math.floor(Number(v));
  return Number.isFinite(n) && n > 0 ? n : null;
}

export function sameRepeat(a: Repeat, b: Repeat): boolean {
  return JSON.stringify(normalizeRepeat(a)) === JSON.stringify(normalizeRepeat(b));
}

function emptyStore(): Store {
  return { nextId: 1, reminders: [], lists: {}, notes: [] };
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
    memories: arr<Memory>(r.memories),
    feeds: arr<Feed>(r.feeds),
  };
  for (const rem of store.reminders) rem.repeat = normalizeRepeat(rem.repeat);
  // MIGRACIÓN de las listas por categoría ("Entrada", "Casa", "Trabajo",
  // "Administrativo", proyectos sueltos): quedaron DOS listas. Todo lo que no
  // era de compras se vuelca a la de tareas, en orden, sin perder nada; lo ya
  // hecho no se arrastra. Las listas viejas desaparecen del archivo.
  const rawLists = r.lists && typeof r.lists === "object" && !Array.isArray(r.lists) ? r.lists : {};
  store.lists[SHOPPING_LIST] = [];
  store.lists[TASK_LIST] = [];
  for (const [name, items] of Object.entries(rawLists)) {
    if (!Array.isArray(items)) continue;
    const target = isShoppingName(name) ? SHOPPING_LIST : TASK_LIST;
    for (const it of items as Item[]) {
      if (!it || typeof it !== "object") continue;
      if (target === TASK_LIST && it.done) continue;  // basura vieja de listas que ya no existen
      store.lists[target].push(it);
    }
  }
  store.settings = { ...DEFAULT_SETTINGS, ...(r.settings && typeof r.settings === "object" ? r.settings : {}) };
  // El nextId tiene que quedar arriba de todo lo que ya existe: si el archivo
  // vino truncado, repetir ids mezcla ítems de listas distintas.
  const ids = [...store.reminders, ...store.notes, ...(store.memories ?? []), ...(store.feeds ?? []), ...Object.values(store.lists).flat()]
    .map((e: { id?: number }) => Number(e?.id) || 0);
  store.nextId = Math.max(store.nextId, ...ids.map((i) => i + 1), 1);
  return store;
}

// Caché por cuenta, con tope. Con 1000 aparatos un Map sin límite es una fuga
// de memoria, así que se queda con las últimas MAX_CACHED cuentas (LRU por orden
// de inserción del Map) y el resto vuelve a leerse de la base.
const MAX_CACHED = 64;
const cache = new Map<number, Store>();
const loading = new Map<number, Promise<Store>>();

function remember(accountId: number, store: Store): Store {
  cache.delete(accountId);
  cache.set(accountId, store);
  while (cache.size > MAX_CACHED) {
    const oldest = cache.keys().next().value;
    if (oldest === undefined) break;
    cache.delete(oldest);
  }
  return store;
}

export function load(accountId: number): Promise<Store> {
  const hit = cache.get(accountId);
  if (hit) return Promise.resolve(remember(accountId, hit));
  // La promesa se cachea, no el resultado: dos pedidos juntos leían el archivo
  // dos veces y se quedaban con dos objetos distintos (lo que guardaba uno lo
  // pisaba el otro).
  let pending = loading.get(accountId);
  if (!pending) {
    pending = readDoc<unknown>(accountId, "store", null).then((raw) => {
      loading.delete(accountId);
      return remember(accountId, normalizeStore(raw));
    }, (err) => {
      loading.delete(accountId);
      throw err;
    });
    loading.set(accountId, pending);
  }
  return pending;
}

// Escritura a secas, sin candado: NO usarla para modificar (para eso está
// mutate()). Queda para reemplazar el documento entero, que hoy no hace nadie.
export async function save(accountId: number, store: Store): Promise<void> {
  remember(accountId, store);
  await writeDoc(accountId, "store", store);
}

// TODO lo que modifica el store va por acá.
//
// load() + save() son dos operaciones separadas sobre el documento entero: lo
// que se lee puede ser de la caché de ESTE proceso (que no sabe nada de lo que
// escribió otra réplica ni de lo que se escribió antes del último redeploy) y
// lo que se guarda pisa el documento completo. Dos pedidos que se cruzan —o dos
// instancias— y el último borra lo que hizo el otro: un recordatorio que se
// agregó por voz desaparece porque la web guardó un ajuste medio segundo
// después.
//
// mutateDoc lee y escribe DENTRO del candado (pg_advisory_xact_lock en
// Postgres, serialize() por archivo en el volumen), así que lo que se modifica
// es siempre la versión que hay guardada en ese instante. El store era el único
// documento que quedaba afuera, y es el que más se escribe.
export async function mutate<R>(accountId: number, fn: (store: Store) => R | Promise<R>): Promise<R> {
  try {
    return await mutateDoc(accountId, "store", normalizeStore, async (store) => {
      const out = await fn(store);
      remember(accountId, store);  // la caché se queda con lo recién leído y cambiado
      return out;
    });
  } catch (err) {
    // Pudo quedar a medio cambiar y sin guardarse: mejor que se relea.
    cache.delete(accountId);
    throw err;
  }
}

export function nextId(store: Store): number {
  return store.nextId++;
}

function foldName(s: string): string {
  return s.normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase().trim();
}

// Palabras que quieren decir "esto es una compra", en los seis idiomas. Todo lo
// demás (cualquier lista que invente el modelo o escriba la web) cae en tareas.
const SHOPPING_WORDS = [
  "compra", "compras", "super", "supermercado", "mercado", "mandado", "mandados", "almacen",
  "shopping", "groceries", "grocery", "market",
  "courses", "course", "supermarche", "epicerie",
  "einkauf", "einkaufe", "einkaufen", "einkaufsliste", "supermarkt",
  "mercearia",
  "pokupki", "produkty", "покупки", "продукты", "магазин",
];

export function isShoppingName(name: string | null | undefined): boolean {
  const n = foldName(name ?? "");
  if (!n) return false;
  return n.split(/[^a-z0-9Ѐ-ӿ]+/).some((w) => w && SHOPPING_WORDS.includes(w));
}

// Nombre de lista tal como lo dijo el usuario (o como lo mandó el aparato en su
// idioma) -> una de las DOS listas. Ya no se crean listas nuevas: lo que no sea
// claramente de compras va a tareas.
export function resolveList(store: Store, spoken: string | null | undefined): string {
  store.lists[SHOPPING_LIST] ??= [];
  store.lists[TASK_LIST] ??= [];
  return isShoppingName(spoken) ? SHOPPING_LIST : TASK_LIST;
}

// El nombre que se muestra, en el idioma del aparato.
export function listLabel(name: string, lang: Lang = "es"): string {
  return name === SHOPPING_LIST ? LABELS[lang].shopping : LABELS[lang].tasks;
}

// "2026-09-07T10:30" -> texto corto para la pantalla del aparato, relativo a hoy.
export function whenLabel(dueAt: string | null, lang: Lang = "es", now = new Date()): string {
  if (!dueAt) return "";
  const [date, time] = dueAt.split("T");
  // Hoy y mañana en la zona del usuario: con toISOString() a secas, a las 22:00
  // en Buenos Aires ya era "mañana" y un recordatorio de hoy decía la fecha.
  const today = todayLocal(now.getTime());
  const tomorrow = todayLocal(now.getTime() + 86_400_000);
  const l = LABELS[lang];
  const day = date === today ? l.today : date === tomorrow ? l.tomorrow : date.slice(8, 10) + "/" + date.slice(5, 7);
  return time ? `${day} ${time}` : day;
}

// ── Zona horaria ────────────────────────────────────────────────────────────
// La zona sale del LUGAR que eligió el usuario (hub-settings.json, el mismo que
// usa el clima) y HUB_TZ queda de respaldo. Se relee cada minuto en vez de
// cachearse para siempre: un proceso que arrancó antes de que se guardara el
// lugar se quedaba con la zona vieja hasta el próximo deploy.
const ENV_TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";

function tzExists(tz: string): boolean {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone: tz });
    return true;
  } catch {
    return false;
  }
}

// La zona es de CADA CUENTA (sale del lugar que eligió para el clima), pero
// `timeZone()` tiene que ser sincrónica: localToEpoch, expandRepeat y media
// docena de funciones más la llaman desde todos lados y no pueden ser async ni
// recibir la cuenta por parámetro sin arrastrar el accountId hasta el último
// rincón. Por eso la zona del pedido en curso viaja en un AsyncLocalStorage que
// arma el middleware de la API (`withTimeZone`): es lo único implícito de todo
// el multiusuario, y es a propósito.
type TzScope = { tz: string };
const tzScope = new AsyncLocalStorage<TzScope>();

// Zona por cuenta, releída como mucho una vez por minuto (antes era una sola
// variable global y un proceso que arrancó sin lugar se quedaba con la zona
// vieja hasta el próximo deploy).
const tzCache = new Map<number, { tz: string; at: number }>();

export function timeZone(): string {
  return tzScope.getStore()?.tz ?? ENV_TZ;
}

/** La zona de una cuenta, del lugar que eligió para el clima; HUB_TZ de respaldo. */
export async function timeZoneOf(accountId: number): Promise<string> {
  const hit = tzCache.get(accountId);
  if (hit && Date.now() - hit.at < 60_000) return hit.tz;
  let tz = ENV_TZ;
  try {
    const raw = await readDoc<Record<string, unknown> | null>(accountId, "hub-settings", null);
    const saved = raw && typeof raw === "object" ? String(raw.timezone ?? "") : "";
    if (saved && tzExists(saved)) tz = saved;
  } catch (err) {
    console.error("timeZoneOf:", err);
  }
  if (tzCache.size > 512) tzCache.clear();
  tzCache.set(accountId, { tz, at: Date.now() });
  return tz;
}

/** Corre `fn` con la zona de esa cuenta puesta para todo lo sincrónico de adentro. */
export async function withTimeZone<T>(accountId: number, fn: () => Promise<T>): Promise<T> {
  return tzScope.run({ tz: await timeZoneOf(accountId) }, fn);
}

/**
 * Relee la zona de la cuenta y la deja puesta en el pedido en curso. La llaman
 * el calendario y el hub antes de hacer cuentas con fechas; con el middleware
 * ya puesta, es redundante pero barata (caché de un minuto).
 */
export async function refreshTimeZone(accountId: number): Promise<string> {
  const tz = await timeZoneOf(accountId);
  const scope = tzScope.getStore();
  if (scope) scope.tz = tz;
  return tz;
}

/** Cuando cambia el lugar, la zona vieja no vale más. */
export function forgetTimeZone(accountId: number): void {
  tzCache.delete(accountId);
}

// Desfase (ms) de la zona en uso respecto de UTC en un instante dado.
function tzOffsetMs(at: number): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: timeZone(), hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  const asUtc = Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second"));
  return asUtc - Math.floor(at / 1000) * 1000;
}

// "YYYY-MM-DDTHH:MM" (hora local del lugar) -> epoch UTC en segundos. Sin hora: 09:00.
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

// Comienzo (y fin) del día local en epoch UTC. NO alcanza con localToEpoch de
// las 00:00: hay días en los que la medianoche NO EXISTE (Chile adelanta el
// reloj justo a las 00:00 del domingo de septiembre) y la cuenta cae en el día
// anterior — un evento de todo el día aparecía un día antes. Se busca la
// primera (o la última) hora que sí existe en ese día.
export function startOfLocalDay(date: string): number {
  for (let h = 0; h < 4; h++) {
    const e = localToEpoch(`${date}T${String(h).padStart(2, "0")}:00`);
    if (epochToLocal(e).slice(0, 10) === date) return e;
  }
  return localToEpoch(date + "T12:00");
}

export function endOfLocalDay(date: string): number {
  for (let h = 23; h > 19; h--) {
    const e = localToEpoch(`${date}T${String(h).padStart(2, "0")}:59`);
    if (epochToLocal(e).slice(0, 10) === date) return e;
  }
  return localToEpoch(date + "T12:00");
}

// Hoy en la zona del usuario, como "YYYY-MM-DD". No sirve `toISOString()` a
// secas: a las 22:00 en Buenos Aires ya es mañana en UTC.
export function todayLocal(now = Date.now()): string {
  return new Date(now + tzOffsetMs(now)).toISOString().slice(0, 10);
}

// ── Fechas civiles ──────────────────────────────────────────────────────────
// TODA la cuenta de repeticiones se hace sobre fechas civiles "YYYY-MM-DD", sin
// zona y sin epoch. Pasar a epoch antes de sumar días es lo que rompe los
// calendarios cuando cambia el horario de verano (un "todos los días a las 8"
// se corre a las 7 o a las 9) y lo que hace que un evento de todo el día
// aparezca el día anterior. El epoch se calcula recién al final, con
// localToEpoch, cuando ya se sabe el día y la hora local.
const DAY_MS = 86_400_000;

export function isDateStr(s: unknown): s is string {
  return typeof s === "string" && /^\d{4}-\d{2}-\d{2}$/.test(s);
}

function civilMs(date: string): number {
  return Date.UTC(Number(date.slice(0, 4)), Number(date.slice(5, 7)) - 1, Number(date.slice(8, 10)));
}

function mkDate(y: number, m: number, d: number): string {
  return `${String(y).padStart(4, "0")}-${String(m).padStart(2, "0")}-${String(d).padStart(2, "0")}`;
}

export function addDays(date: string, n: number): string {
  return new Date(civilMs(date) + n * DAY_MS).toISOString().slice(0, 10);
}

export function diffDays(a: string, b: string): number {
  return Math.round((civilMs(a) - civilMs(b)) / DAY_MS);
}

// 0 = domingo .. 6 = sábado (igual que Repeat.days).
export function weekdayOf(date: string): number {
  return new Date(civilMs(date)).getUTCDay();
}

function daysInMonth(y: number, m: number): number {
  return new Date(Date.UTC(y, m, 0)).getUTCDate();
}

function untilDate(until: number): string {
  return epochToLocal(until).slice(0, 10);
}

// ── Repeticiones ────────────────────────────────────────────────────────────

// La primera fecha que cumple la repetición desde `date` (incluida). Sirve al
// guardar: si el usuario elige "los martes y jueves" un lunes, el recordatorio
// arranca el martes y no el lunes.
export function alignToRepeat(date: string, repeat: Repeat): string {
  const r = normalizeRepeat(repeat);
  if (!isDateStr(date)) return date;
  if (r.kind === "weekdays") {
    let d = date;
    for (let i = 0; i < 7 && (weekdayOf(d) === 0 || weekdayOf(d) === 6); i++) d = addDays(d, 1);
    return d;
  }
  if (r.kind === "weekly" && r.days?.length) {
    let d = date;
    for (let i = 0; i < 7 && !r.days.includes(weekdayOf(d)); i++) d = addDays(d, 1);
    return d;
  }
  return date;
}

// Próxima fecha de la repetición después de `date`, o null si no hay más
// (repeat none, o `until` ya pasado). El 31 y el 29 de febrero SALTEAN el mes o
// el año que no los tiene (igual que Google Calendar y el RFC 5545): un
// "todos los 31" no se corre al 28 de febrero por su cuenta.
export function nextOccurrence(date: string, repeat: Repeat): string | null {
  const r = normalizeRepeat(repeat);
  if (r.kind === "none" || !isDateStr(date)) return null;
  const step = r.interval ?? 1;
  let next: string | null = null;
  if (r.kind === "daily") {
    next = addDays(date, step);
  } else if (r.kind === "weekdays") {
    let d = addDays(date, 1);
    for (let i = 0; i < 7 && (weekdayOf(d) === 0 || weekdayOf(d) === 6); i++) d = addDays(d, 1);
    next = d;
  } else if (r.kind === "weekly") {
    const days = r.days?.length ? r.days : [weekdayOf(date)];
    const wd = weekdayOf(date);
    const rest = days.filter((d) => d > wd);
    next = rest.length ? addDays(date, rest[0] - wd) : addDays(date, 7 * step - wd + days[0]);
  } else if (r.kind === "monthly") {
    const [y, m, d] = [Number(date.slice(0, 4)), Number(date.slice(5, 7)), Number(date.slice(8, 10))];
    for (let i = 1; i <= 48 && !next; i++) {
      const t = m - 1 + step * i;
      const yy = y + Math.floor(t / 12);
      const mm = (t % 12) + 1;
      if (d <= daysInMonth(yy, mm)) next = mkDate(yy, mm, d);
    }
  } else {
    const [y, m, d] = [Number(date.slice(0, 4)), Number(date.slice(5, 7)), Number(date.slice(8, 10))];
    for (let i = 1; i <= 12 && !next; i++) {
      const yy = y + step * i;
      if (d <= daysInMonth(yy, m)) next = mkDate(yy, m, d);
    }
  }
  if (!next) return null;
  if (r.until && next > untilDate(r.until)) return null;
  return next;
}

// Todas las fechas de la repetición dentro de [from, to] (inclusive), a partir
// de `start`. Tope de `cap` ocurrencias: un diario de diez años no puede tirar
// abajo el calendario del aparato.
export function expandRepeat(start: string, repeat: Repeat, from: string, to: string, cap = 500): string[] {
  const out: string[] = [];
  if (!isDateStr(start) || !isDateStr(from) || !isDateStr(to) || from > to) return out;
  const r = normalizeRepeat(repeat);
  if (r.kind === "none") {
    if (start >= from && start <= to) out.push(start);
    return out;
  }
  const stop = r.until ? (untilDate(r.until) < to ? untilDate(r.until) : to) : to;
  let d = alignToRepeat(start, r);
  if (d > stop) return out;
  // Salto rápido hasta el principio del rango: un recordatorio diario de hace
  // tres años son mil vueltas de bucle si se avanza de a un día.
  if (d < from) {
    const step = r.interval ?? 1;
    const gap = diffDays(from, d);
    if (r.kind === "daily") d = addDays(d, Math.floor(gap / step) * step);
    else if (r.kind === "weekdays") d = addDays(d, Math.floor(gap / 7) * 7);
    else if (r.kind === "weekly") d = addDays(d, Math.floor(gap / (7 * step)) * (7 * step));
  }
  let guard = 0;
  let cur: string | null = d;
  while (cur && cur <= stop && out.length < cap && guard++ < 6000) {
    if (cur >= from) out.push(cur);
    cur = nextOccurrence(cur, r);
  }
  return out;
}

// Recordatorio con repetición: corre la fecha al próximo ciclo (mantiene la hora).
export function advanceRepeat(r: Reminder): boolean {
  const repeat = normalizeRepeat(r.repeat);
  if (repeat.kind === "none" || !r.dueAt) return false;
  const [date, time] = r.dueAt.split("T");
  const next = nextOccurrence(date, repeat);
  if (!next) return false;  // se acabó la repetición (until): el recordatorio se cierra
  r.dueAt = next + (time ? "T" + time : "");
  return true;
}

// ── La repetición en una línea de texto ─────────────────────────────────────
// Esto es lo que el usuario ve en el aparato para saber cuándo lo va a
// despertar, sin tener que interpretar días de la semana ni intervalos:
// "Todos los días", "De lunes a viernes", "Los martes y jueves",
// "Cada 2 semanas", "El 15 de cada mes", "Todos los años el 3 de mayo".
type RepeatWords = {
  once: string;
  daily: string;
  everyDays: (n: number) => string;
  weekdays: string;
  weekend: string;
  onDays: (list: string) => string;
  weekly: string;
  everyWeeks: (n: number) => string;
  everyWeeksOn: (n: number, list: string) => string;
  monthly: string;
  monthlyOn: (day: number) => string;
  everyMonthsOn: (n: number, day: number) => string;
  yearly: string;
  yearlyOn: (date: string) => string;
  everyYearsOn: (n: number, date: string) => string;
  until: (date: string) => string;
  days: string[];  // 0=domingo, en plural/adverbial ("los martes", "dienstags")
  and: string;
};

const LOCALE: Record<Lang, string> = { es: "es", en: "en-US", fr: "fr", de: "de", pt: "pt-BR", ru: "ru" };

function cap1(s: string): string {
  return s ? s[0].toUpperCase() + s.slice(1) : s;
}

// Plural ruso: 1 день, 2 дня, 5 дней.
function ru(n: number, one: string, few: string, many: string): string {
  const m10 = n % 10;
  const m100 = n % 100;
  if (m10 === 1 && m100 !== 11) return one;
  if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
  return many;
}

function ordinalEn(n: number): string {
  const s = ["th", "st", "nd", "rd"];
  const v = n % 100;
  return n + (s[(v - 20) % 10] ?? s[v] ?? s[0]);
}

const WORDS: Record<Lang, RepeatWords> = {
  es: {
    once: "Una sola vez",
    daily: "Todos los días",
    everyDays: (n) => `Cada ${n} días`,
    weekdays: "De lunes a viernes",
    weekend: "Sábados y domingos",
    onDays: (l) => `Los ${l}`,
    weekly: "Todas las semanas",
    everyWeeks: (n) => `Cada ${n} semanas`,
    everyWeeksOn: (n, l) => `Cada ${n} semanas, los ${l}`,
    monthly: "Todos los meses",
    monthlyOn: (d) => `El ${d} de cada mes`,
    everyMonthsOn: (n, d) => `Cada ${n} meses, el día ${d}`,
    yearly: "Todos los años",
    yearlyOn: (d) => `Todos los años el ${d}`,
    everyYearsOn: (n, d) => `Cada ${n} años, el ${d}`,
    until: (d) => `hasta el ${d}`,
    days: ["domingos", "lunes", "martes", "miércoles", "jueves", "viernes", "sábados"],
    and: "y",
  },
  en: {
    once: "Once",
    daily: "Every day",
    everyDays: (n) => `Every ${n} days`,
    weekdays: "Monday to Friday",
    weekend: "Saturdays and Sundays",
    onDays: (l) => cap1(l),
    weekly: "Every week",
    everyWeeks: (n) => `Every ${n} weeks`,
    everyWeeksOn: (n, l) => `Every ${n} weeks, on ${l}`,
    monthly: "Every month",
    monthlyOn: (d) => `On the ${ordinalEn(d)} of every month`,
    everyMonthsOn: (n, d) => `Every ${n} months, on day ${d}`,
    yearly: "Every year",
    yearlyOn: (d) => `Every year on ${d}`,
    everyYearsOn: (n, d) => `Every ${n} years, on ${d}`,
    until: (d) => `until ${d}`,
    days: ["Sundays", "Mondays", "Tuesdays", "Wednesdays", "Thursdays", "Fridays", "Saturdays"],
    and: "and",
  },
  fr: {
    once: "Une seule fois",
    daily: "Tous les jours",
    everyDays: (n) => `Tous les ${n} jours`,
    weekdays: "Du lundi au vendredi",
    weekend: "Les samedis et dimanches",
    onDays: (l) => `Les ${l}`,
    weekly: "Toutes les semaines",
    everyWeeks: (n) => `Toutes les ${n} semaines`,
    everyWeeksOn: (n, l) => `Toutes les ${n} semaines, les ${l}`,
    monthly: "Tous les mois",
    monthlyOn: (d) => `Le ${d} de chaque mois`,
    everyMonthsOn: (n, d) => `Tous les ${n} mois, le ${d}`,
    yearly: "Tous les ans",
    yearlyOn: (d) => `Tous les ans le ${d}`,
    everyYearsOn: (n, d) => `Tous les ${n} ans, le ${d}`,
    until: (d) => `jusqu'au ${d}`,
    days: ["dimanches", "lundis", "mardis", "mercredis", "jeudis", "vendredis", "samedis"],
    and: "et",
  },
  de: {
    once: "Einmalig",
    daily: "Jeden Tag",
    everyDays: (n) => `Alle ${n} Tage`,
    weekdays: "Montag bis Freitag",
    weekend: "Samstags und sonntags",
    onDays: (l) => cap1(l),
    weekly: "Jede Woche",
    everyWeeks: (n) => `Alle ${n} Wochen`,
    everyWeeksOn: (n, l) => `Alle ${n} Wochen, ${l}`,
    monthly: "Jeden Monat",
    monthlyOn: (d) => `Am ${d}. jedes Monats`,
    everyMonthsOn: (n, d) => `Alle ${n} Monate, am ${d}.`,
    yearly: "Jedes Jahr",
    yearlyOn: (d) => `Jedes Jahr am ${d}`,
    everyYearsOn: (n, d) => `Alle ${n} Jahre, am ${d}`,
    until: (d) => `bis ${d}`,
    days: ["sonntags", "montags", "dienstags", "mittwochs", "donnerstags", "freitags", "samstags"],
    and: "und",
  },
  pt: {
    once: "Uma só vez",
    daily: "Todos os dias",
    everyDays: (n) => `A cada ${n} dias`,
    weekdays: "De segunda a sexta",
    weekend: "Sábados e domingos",
    onDays: (l) => cap1(l),
    weekly: "Toda semana",
    everyWeeks: (n) => `A cada ${n} semanas`,
    everyWeeksOn: (n, l) => `A cada ${n} semanas, ${l}`,
    monthly: "Todo mês",
    monthlyOn: (d) => `Dia ${d} de cada mês`,
    everyMonthsOn: (n, d) => `A cada ${n} meses, no dia ${d}`,
    yearly: "Todo ano",
    yearlyOn: (d) => `Todo ano em ${d}`,
    everyYearsOn: (n, d) => `A cada ${n} anos, em ${d}`,
    until: (d) => `até ${d}`,
    days: ["domingos", "segundas-feiras", "terças-feiras", "quartas-feiras", "quintas-feiras", "sextas-feiras", "sábados"],
    and: "e",
  },
  ru: {
    once: "Один раз",
    daily: "Каждый день",
    everyDays: (n) => `Каждые ${n} ${ru(n, "день", "дня", "дней")}`,
    weekdays: "С понедельника по пятницу",
    weekend: "По субботам и воскресеньям",
    onDays: (l) => `По ${l}`,
    weekly: "Каждую неделю",
    everyWeeks: (n) => `Каждые ${n} ${ru(n, "неделю", "недели", "недель")}`,
    everyWeeksOn: (n, l) => `Каждые ${n} ${ru(n, "неделю", "недели", "недель")}, по ${l}`,
    monthly: "Каждый месяц",
    monthlyOn: (d) => `${d}-го числа каждого месяца`,
    everyMonthsOn: (n, d) => `Каждые ${n} ${ru(n, "месяц", "месяца", "месяцев")}, ${d}-го числа`,
    yearly: "Каждый год",
    yearlyOn: (d) => `Каждый год ${d}`,
    everyYearsOn: (n, d) => `Каждые ${n} ${ru(n, "год", "года", "лет")}, ${d}`,
    until: (d) => `до ${d}`,
    days: ["воскресеньям", "понедельникам", "вторникам", "средам", "четвергам", "пятницам", "субботам"],
    and: "и",
  },
};

function joinDays(days: number[], w: RepeatWords): string {
  const names = days.map((d) => w.days[d] ?? "");
  if (names.length <= 1) return names[0] ?? "";
  return names.slice(0, -1).join(", ") + ` ${w.and} ` + names[names.length - 1];
}

// "3 de mayo" / "May 3" / "3 мая" (sin año) y con año para el "hasta".
function dayMonth(date: string, lang: Lang, withYear = false): string {
  const d = new Date(civilMs(date));
  return new Intl.DateTimeFormat(LOCALE[lang], {
    timeZone: "UTC", day: "numeric", month: "long", ...(withYear ? { year: "numeric" } : {}),
  }).format(d);
}

// La repetición en una línea, en el idioma del aparato. `dueAt` (la fecha del
// recordatorio o del evento) es lo que le da sentido a monthly y yearly: sin
// ella no se puede decir "el 15 de cada mes".
export function repeatText(repeat: unknown, dueAt: string | null, lang: Lang = "es"): string {
  const r = normalizeRepeat(repeat);
  const w = WORDS[lang] ?? WORDS.es;
  const n = r.interval ?? 1;
  const date = dueAt && isDateStr(dueAt.slice(0, 10)) ? dueAt.slice(0, 10) : "";
  let text: string;
  switch (r.kind) {
    case "none":
      return w.once;
    case "daily":
      text = n === 1 ? w.daily : w.everyDays(n);
      break;
    case "weekdays":
      text = w.weekdays;
      break;
    case "weekly": {
      const days = r.days?.length ? r.days : date ? [weekdayOf(date)] : [];
      if (!days.length) text = n === 1 ? w.weekly : w.everyWeeks(n);
      else if (n === 1) {
        const set = days.join(",");
        text = set === "1,2,3,4,5" ? w.weekdays : set === "0,6" ? w.weekend : w.onDays(joinDays(days, w));
      } else text = w.everyWeeksOn(n, joinDays(days, w));
      break;
    }
    case "monthly": {
      const day = date ? Number(date.slice(8, 10)) : 0;
      text = !day ? w.monthly : n === 1 ? w.monthlyOn(day) : w.everyMonthsOn(n, day);
      break;
    }
    default: {
      text = !date ? w.yearly : n === 1 ? w.yearlyOn(dayMonth(date, lang)) : w.everyYearsOn(n, dayMonth(date, lang));
      break;
    }
  }
  if (r.until) text += ", " + w.until(dayMonth(untilDate(r.until), lang, true));
  return text;
}

// ── El formato del aparato ──────────────────────────────────────────────────
// El firmware (AgendaActivity / HubStore) no maneja el objeto: manda y espera
// la repetición PLANA — un código suelto más `weekday` e `interval` aparte, y
// su día de la semana arranca en LUNES (0 = lunes .. 6 = domingo), al revés
// del `days` de acá (0 = domingo). Estas dos funciones son la traducción; el
// modelo de adentro sigue siendo el objeto, que es más rico (varios días).
//   once | daily | weekdays | weekly (+weekday) | weeks (+interval) | monthly | yearly
export type RepeatWire = { repeat: string; weekday: number; interval: number };

export function repeatToWire(repeat: unknown, dueAt: string | null): RepeatWire {
  const r = normalizeRepeat(repeat);
  const n = r.interval ?? 1;
  const date = dueAt && isDateStr(dueAt.slice(0, 10)) ? dueAt.slice(0, 10) : "";
  if (r.kind === "none") return { repeat: "once", weekday: -1, interval: 0 };
  if (r.kind === "weekly") {
    if (n > 1) return { repeat: "weeks", weekday: -1, interval: n };
    // Varios días no entran en el editor del aparato: se manda el primero (el
    // texto de arriba, repeatText, sí los dice todos).
    const day = r.days?.length ? r.days[0] : date ? weekdayOf(date) : 1;
    return { repeat: "weekly", weekday: (day + 6) % 7, interval: 0 };
  }
  return { repeat: r.kind, weekday: -1, interval: n > 1 ? n : 0 };
}

// Lo que manda el aparato al guardar -> Repeat. Un objeto (la web, la voz)
// pasa derecho.
export function repeatFromWire(body: { repeat?: unknown; weekday?: unknown; interval?: unknown }): unknown {
  const r = body.repeat;
  if (typeof r !== "string") return r;
  const interval = Math.floor(Number(body.interval));
  if (r === "weeks") return { kind: "weekly", interval: Number.isFinite(interval) && interval > 1 ? interval : 2 };
  const weekday = Math.floor(Number(body.weekday));
  if (r === "weekly" && Number.isInteger(weekday) && weekday >= 0 && weekday <= 6) {
    return { kind: "weekly", days: [(weekday + 1) % 7] };  // lunes=0 -> domingo=0
  }
  return r;  // once (= none), daily, weekdays, monthly, yearly
}

// Alta o edición de un recordatorio desde el aparato o desde /board.
export type UpsertResult = { ok: true; reminder: Reminder; created: boolean } | { ok: false; error: "not_found" | "title" };

export function upsertReminder(
  store: Store,
  // `dueAt` es lo que manda la web; el aparato manda `date` y `time` por
  // separado (con time vacío cuando no tiene hora), y la repetición plana.
  body: {
    id?: number | null; title?: string; dueAt?: string | null; repeat?: unknown; done?: boolean;
    date?: string; time?: string; weekday?: unknown; interval?: unknown;
  },
): UpsertResult {
  const id = Number(body.id);
  const existing = Number.isFinite(id) && id > 0 ? store.reminders.find((r) => r.id === id) : undefined;
  if (Number.isFinite(id) && id > 0 && !existing) return { ok: false, error: "not_found" };
  const title = (body.title ?? existing?.title ?? "").toString().trim().slice(0, 200);
  if (!title) return { ok: false, error: "title" };
  const fromParts = isDateStr(body.date)
    ? body.date + (/^\d{2}:\d{2}$/.test(String(body.time ?? "")) ? "T" + body.time : "")
    : undefined;
  const dueRaw = body.dueAt !== undefined ? body.dueAt : fromParts !== undefined ? fromParts : existing?.dueAt ?? null;
  let dueAt = typeof dueRaw === "string" && /^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2})?$/.test(dueRaw) ? dueRaw : null;
  const repeat = normalizeRepeat(
    body.repeat !== undefined ? repeatFromWire(body) : existing?.repeat ?? NO_REPEAT,
  );
  // "Los martes y jueves" empezando un lunes arranca el martes, no el lunes.
  if (dueAt) {
    const [d, t] = dueAt.split("T");
    dueAt = alignToRepeat(d, repeat) + (t ? "T" + t : "");
  }
  if (existing) {
    existing.title = title;
    existing.dueAt = dueAt;
    existing.repeat = repeat;
    if (typeof body.done === "boolean") existing.done = body.done;
    return { ok: true, reminder: existing, created: false };
  }
  const rem: Reminder = { id: nextId(store), title, dueAt, repeat, done: body.done === true, createdAt: new Date().toISOString() };
  store.reminders.push(rem);
  return { ok: true, reminder: rem, created: true };
}

export function pendingReminders(store: Store): Reminder[] {
  return store.reminders
    .filter((r) => !r.done)
    .sort((a, b) => (a.dueAt ?? "9999").localeCompare(b.dueAt ?? "9999"));
}

// ── Memoria del asistente ───────────────────────────────────────────────────
// "Acordate que soy vegetariano", "mi hija se llama Ana": datos sobre el usuario
// que entran en el system prompt de TODA pregunta (ask.ts y voice.ts). Hasta
// ahora se guardaban y no los leía nadie.
//
// Topes: 40 hechos o 2 KB, lo que se cumpla primero, quedándose con los más
// nuevos. Es un bloque estable entre consultas, así que va con cache_control en
// Anthropic y al principio del system en las compatibles con OpenAI (ver
// llm.ts): sin eso se pagan esos tokens enteros en cada consulta.
export const MEMORY_MAX = 40;
export const MEMORY_BYTES = 2048;

export function memoryLines(store: Store): string[] {
  const all = (store.memories ?? []).map((m) => m.text.trim()).filter(Boolean);
  const out: string[] = [];
  let bytes = 0;
  // De atrás para adelante: si hay que recortar, se van los más viejos.
  for (let i = all.length - 1; i >= 0 && out.length < MEMORY_MAX; i--) {
    const b = new TextEncoder().encode(all[i]).length + 3;
    if (bytes + b > MEMORY_BYTES) break;
    bytes += b;
    out.push(all[i]);
  }
  return out.reverse();
}

function foldFact(s: string): string {
  return s
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "")
    .toLowerCase()
    .replace(/[^\p{Letter}\p{Number}\s]+/gu, " ")
    .replace(/\s+/g, " ")
    .trim();
}

// Palabras sin contenido: si no se sacan, "mi hija se llama Ana" y "mi perro se
// llama Ana" parecen la misma memoria.
const STOP = new Set([
  "el", "la", "los", "las", "un", "una", "de", "del", "que", "y", "o", "a", "en", "es", "son", "mi", "mis",
  "me", "se", "su", "sus", "por", "para", "con", "no", "si", "lo", "al", "yo", "soy", "esta", "este",
  "the", "a", "an", "of", "to", "in", "is", "am", "my", "i", "and", "for", "with", "not",
]);

function words(s: string): string[] {
  return foldFact(s).split(" ").filter((w) => w.length > 1 && !STOP.has(w));
}

// Parecido por palabras (Jaccard). Alcanza para "vivo en México" contra "ya no
// vivo en México": lo importante es que la nueva pise a la vieja en vez de
// dejar dos memorias que se contradicen.
export function factSimilarity(a: string, b: string): number {
  const A = new Set(words(a));
  const B = new Set(words(b));
  if (!A.size || !B.size) return 0;
  let inter = 0;
  for (const w of A) if (B.has(w)) inter++;
  const union = A.size + B.size - inter;
  const jaccard = union ? inter / union : 0;
  // Una memoria nueva que contiene entera a la vieja ("vivo en México" ->
  // "ya no vivo en México, ahora en Chile") también la reemplaza.
  const contained = inter / Math.min(A.size, B.size);
  return Math.max(jaccard, contained >= 1 ? 0.8 : 0);
}

// Guarda un hecho nuevo. Si ya hay uno parecido, lo REEMPLAZA (el usuario está
// corrigiendo, no acumulando: "ya no vivo en México" no puede convivir con
// "vivo en México"). Devuelve si pisó algo.
export function rememberFact(store: Store, text: string): { replaced: string | null } {
  store.memories ??= [];
  const fact = text.trim();
  if (!fact) return { replaced: null };
  let bestIdx = -1;
  let best = 0;
  for (let i = 0; i < store.memories.length; i++) {
    const sim = factSimilarity(fact, store.memories[i].text);
    if (sim > best) { best = sim; bestIdx = i; }
  }
  if (bestIdx >= 0 && best >= 0.5) {
    const replaced = store.memories[bestIdx].text;
    store.memories.splice(bestIdx, 1);
    store.memories.push({ id: nextId(store), text: fact, createdAt: new Date().toISOString() });
    return { replaced };
  }
  store.memories.push({ id: nextId(store), text: fact, createdAt: new Date().toISOString() });
  while (store.memories.length > 100) store.memories.shift();
  return { replaced: null };
}
