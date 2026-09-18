// Viajes: agenda día por día, papeles (texto), lista para llevar y la guía POR
// DÍA, para la app de Lua `viajes` y para la pestaña Viajes de /board.
//
// Resucitado de `d13923b^:server/src/trips.ts` con lo que dice
// docs/ws397/VIAJES_CONTRATO.md (v2): SIN adjuntos (los papeles son texto
// pegado con título, fecha, tipo y campos), SIN diario, SIN guía general. Un
// viaje son VARIOS lugares (Roma, crucero, islas, Madrid), así que **cada día
// tiene su lugar y su hotel**, y la guía es de UN día y sólo a pedido: el
// servidor mira dónde se está ese día y qué hay cargado, y busca qué hay cerca,
// qué se está perdiendo uno, cómo moverse, dónde comer y lo práctico de ESA
// fecha. Se guarda en `day.guide = {at, answers, text}`.
//
// La fuente de verdad es el documento `trips` (/data/trips.json sin base de
// datos). Lo guardado por la v1 (hotel, guía, clima y coordenadas a nivel de
// viaje) se migra AL LEER en `normalizeTrip`: el hotel del viaje pasa a cada
// día que no tenga el suyo y lo demás se descarta (una guía general no tiene
// día al que ir). El calendario muestra como "trip" cualquier evento con
// `tripId`, así que los ítems CON HORA se espejan ahí con `syncCalendar()`: el
// espejo se rehace entero en cada cambio y nunca se edita a mano.
//
// Rutas de la web (montadas en api.ts, heredan el Bearer / la sesión):
//   GET  /api/trips?lang=                 -> lista de viajes
//   GET  /api/trip?id=&lang=              -> un viaje entero
//   POST /api/trip                        -> crear o editar {id?, name, place, timezone, start, end, notes, active}
//   POST /api/trip/delete                 -> {id}
//   POST /api/trip/active                 -> {id}
//   POST /api/trip/day                    -> {tripId, date, place?, hotel?, note?} (sólo lo que viene se toca)
//   POST /api/trip/day/item               -> crear o editar un ítem del día
//   POST /api/trip/day/item/delete        -> {tripId, date, id}
//   POST /api/trip/packing                -> {tripId, id?, text?, done?, action?}
//   POST /api/trip/paper                  -> crear o editar un papel {tripId, id?, title, date, kind, text, fields, itemId}
//   POST /api/trip/paper/delete           -> {tripId, id}
//   GET  /api/trip/:id/day/:date/guide/questions?lang= -> las preguntas que faltan para ESE día
//   POST /api/trip/:id/day/:date/guide?lang=           -> {answers} -> {jobId}
//   GET  /api/trip/:id/day/:date/guide                 -> {text, at} (404 sin guía)
//   POST /api/trip/sync                   -> rehacer el espejo del calendario
//
// Servicios de la app (`POST /api/apps/call`, app "viajes"): `VIAJES_SERVICES`
// al final, con las formas exactas del contrato.
import { Hono } from "hono";
import type Anthropic from "@anthropic-ai/sdk";
import { mutateDoc, readDoc } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";
import { readBody } from "./net";
import { LANGUAGE_NAME, normalizeLang, type Lang } from "./lang";
import { appsJson, appsProse, appsProseSearch, AppsLlmError, NO_KEY_MSG } from "./appsLlm";
import { config } from "./config";
import { startJob, type JobFile } from "./appsJobs";
import { epochToLocal, mutate as mutateStore, nextId, NO_REPEAT, timeZoneOf } from "./store";
import { asksForSearch } from "./websearch";
import type { Service } from "./apps";

// ---------------------------------------------------------------- tipos

export type ItemKind = "flight" | "train" | "hotel" | "ticket" | "meal" | "visit" | "other";
export const KINDS: ItemKind[] = ["flight", "train", "hotel", "ticket", "meal", "visit", "other"];

export type TripItem = {
  id: string;
  at?: string;          // "HH:MM"; sin hora, es algo del día sin horario
  title: string;
  kind: ItemKind;
  place?: string;
  code?: string;        // localizador, número de reserva: lo que se muestra en el mostrador
  note?: string;
  paperId?: string;
  reminderId?: number;  // el recordatorio de "2 h antes" en store.ts, si está puesto
};

// La guía de UN día: texto plano para el visor, con las respuestas que la
// motivaron (se vuelven a mostrar al rehacerla).
export type DayGuide = { at: number; answers: Record<string, string>; text: string };

export type TripDay = {
  date: string;
  place?: string;       // dónde se está ese día ("Barcelona", "crucero", "Santorini")
  hotel?: string;       // dónde se duerme ESA noche
  note?: string;
  guide?: DayGuide;
  // Mientras se genera la guía del día: el trabajo en curso, para que la web y
  // la app puedan retomar el `job.status` si se fueron en el medio.
  guideJob?: { id: string; at: number };
  items: TripItem[];
};
export type PackItem = { id: string; text: string; done: boolean };
export type Paper = {
  id: string;
  title: string;
  date?: string;
  kind: ItemKind;
  text: string;                     // el correo pegado tal cual
  fields?: Record<string, string>;  // "Localizador": "ABC123"
  itemId?: string;
};

export type Trip = {
  id: string;
  name: string;
  place: string;        // resumen ("Roma · crucero · Madrid"); vacío = se arma con los lugares de los días
  timezone?: string;
  start: string;        // "YYYY-MM-DD"
  end: string;
  notes?: string;
  active?: boolean;
  days: TripDay[];
  packing: PackItem[];
  papers: Paper[];
};

// Lo que la v1 guardaba a nivel de viaje y ya no existe: se lee para migrar y
// se borra al escribir.
type LegacyTrip = Trip & { hotel?: unknown; guide?: unknown; guideJob?: unknown; weather?: unknown; lat?: unknown; lon?: unknown };

type Store = { version: number; trips: Trip[] };

const STORE_VERSION = 2;
const EMPTY: Store = { version: STORE_VERSION, trips: [] };
const MAX_DAYS = 90;
const MAX_TRIPS = 40;
const MAX_PACKING = 200;
const MAX_PAPERS = 60;
const MAX_PAPER_TEXT = 24 * 1024;
const MAX_GUIDE_TEXT = 24 * 1024;
const MAX_VIEW_BYTES = 40 * 1024;
const REMIND_BEFORE_S = 2 * 3600;

// ---------------------------------------------------------------- storage

function str(v: unknown, max: number): string {
  return typeof v === "string" ? v.trim().slice(0, max) : "";
}

function kindOf(v: unknown): ItemKind {
  return KINDS.includes(v as ItemKind) ? (v as ItemKind) : "other";
}

function isObj(v: unknown): v is Record<string, unknown> {
  return !!v && typeof v === "object" && !Array.isArray(v);
}

// Un viaje tal como está en el archivo, a la forma v2. Nunca tira: lo que
// falte se completa y lo que sobre (v1) se migra o se descarta.
function normalizeTrip(raw: LegacyTrip): void {
  const t = raw;
  if (typeof t.id !== "string") t.id = newId();
  if (typeof t.name !== "string") t.name = "";
  if (typeof t.place !== "string") t.place = "";
  if (typeof t.start !== "string") t.start = "";
  if (typeof t.end !== "string") t.end = t.start;
  if (typeof t.timezone !== "string" || !t.timezone) delete t.timezone;
  if (typeof t.notes !== "string" || !t.notes) delete t.notes;
  if (!Array.isArray(t.days)) t.days = [];
  t.days = t.days.filter((d) => isObj(d) && typeof (d as TripDay).date === "string");
  for (const d of t.days) {
    if (!Array.isArray(d.items)) d.items = [];
    d.items = d.items.filter((it) => isObj(it) && typeof (it as TripItem).id === "string");
    for (const it of d.items) {
      if (typeof it.title !== "string") it.title = "";
      it.kind = kindOf(it.kind);
    }
    if (typeof d.place !== "string" || !d.place) delete d.place;
    if (typeof d.hotel !== "string" || !d.hotel) delete d.hotel;
    if (typeof d.note !== "string" || !d.note) delete d.note;
    const g = d.guide as Partial<DayGuide> | undefined;
    if (!isObj(g) || typeof g.text !== "string" || !g.text) delete d.guide;
    else {
      if (typeof g.at !== "number" || !Number.isFinite(g.at)) g.at = 0;
      if (!isObj(g.answers)) g.answers = {};
    }
    if (d.guideJob && (!isObj(d.guideJob) || typeof d.guideJob.id !== "string")) delete d.guideJob;
  }
  if (!Array.isArray(t.packing)) t.packing = [];
  if (!Array.isArray(t.papers)) t.papers = [];
  // v1 → v2: el hotel del viaje pasa a cada día que no tenga el suyo (el
  // usuario lo cargó una vez y valía para todas las noches); la guía general,
  // el clima y las coordenadas no tienen a dónde ir.
  const hotel = typeof t.hotel === "string" ? t.hotel.trim().slice(0, 120) : "";
  if (hotel && !t.days.some((d) => d.hotel)) for (const d of t.days) d.hotel = hotel;
  delete t.hotel;
  delete t.guide;
  delete t.guideJob;
  delete t.weather;
  delete t.lat;
  delete t.lon;
}

function shapeTrips(raw: unknown): Store {
  const store = (isObj(raw) ? raw : structuredClone(EMPTY)) as Store;
  if (!Array.isArray(store.trips)) store.trips = [];
  store.trips = store.trips.filter(isObj) as Trip[];
  for (const t of store.trips) normalizeTrip(t as LegacyTrip);
  store.version = STORE_VERSION;
  return store;
}

function update<T>(accountId: number, fn: (store: Store) => T | Promise<T>): Promise<T> {
  return mutateDoc(accountId, "trips", shapeTrips, fn);
}

export async function loadTrips(accountId: number): Promise<Trip[]> {
  return shapeTrips(await readDoc<unknown>(accountId, "trips", null)).trips;
}

// El viaje activo de la cuenta. Sin ninguno marcado, el que está en curso hoy,
// y si no, el próximo (o el último que hubo).
export function pickActive(trips: Trip[], today: string): Trip | null {
  if (!trips.length) return null;
  const marked = trips.find((t) => t.active);
  if (marked) return marked;
  const sorted = trips.slice().sort((a, b) => a.start.localeCompare(b.start));
  return sorted.find((t) => t.start <= today && today <= t.end) ?? sorted.find((t) => t.start > today) ?? sorted[sorted.length - 1];
}

async function getTrip(accountId: number, id: string): Promise<Trip | null> {
  const trips = await loadTrips(accountId);
  if (id) return trips.find((t) => t.id === id) ?? null;
  return pickActive(trips, todayIn(await timeZoneOf(accountId)));
}

function newId(): string {
  return Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36).padStart(2, "0");
}

// ---------------------------------------------------------------- fechas

const DATE_RE = /^\d{4}-\d{2}-\d{2}$/;
const TIME_RE = /^([01]\d|2[0-3]):[0-5]\d$/;

export function isDate(s: unknown): s is string {
  if (typeof s !== "string" || !DATE_RE.test(s)) return false;
  const d = new Date(`${s}T00:00:00Z`);
  return !Number.isNaN(d.getTime()) && d.toISOString().slice(0, 10) === s;
}

export function addDays(date: string, n: number): string {
  const d = new Date(`${date}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() + n);
  return d.toISOString().slice(0, 10);
}

export function daysBetween(a: string, b: string): number {
  return Math.round((Date.parse(`${b}T00:00:00Z`) - Date.parse(`${a}T00:00:00Z`)) / 86_400_000);
}

function tzOk(tz: string): boolean {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone: tz });
    return true;
  } catch {
    return false;
  }
}

// La fecha civil de hoy en una zona ("" si la zona no existe).
export function todayIn(tz: string): string {
  if (!tz || !tzOk(tz)) return "";
  const parts = new Intl.DateTimeFormat("en-CA", { timeZone: tz, year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(new Date());
  const get = (t: string) => parts.find((p) => p.type === t)?.value ?? "";
  return `${get("year")}-${get("month")}-${get("day")}`;
}

// Un instante local de una zona ("2026-09-14" + "10:40" en Europe/Lisbon) a
// epoch UTC en segundos. Dos pasadas: la primera con el desfase de la zona en
// el instante aproximado, la segunda con el desfase en el resultado (por si
// cae justo en un cambio de horario).
export function zonedToEpoch(date: string, hhmm: string, tz: string): number {
  const [y, mo, d] = date.split("-").map(Number);
  const [h, mi] = hhmm.split(":").map(Number);
  const wall = Date.UTC(y, mo - 1, d, h, mi, 0);
  const offsetAt = (at: number): number => {
    const parts = new Intl.DateTimeFormat("en-US", {
      timeZone: tz, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit",
    }).formatToParts(new Date(at));
    const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
    return Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second")) - Math.floor(at / 1000) * 1000;
  };
  let guess = wall - offsetAt(wall);
  guess = wall - offsetAt(guess);
  return Math.floor(guess / 1000);
}

// Un día con algo cargado no se tira aunque quede fuera del rango.
function dayHasContent(d: TripDay): boolean {
  return !!(d.items.length || d.note || d.place || d.hotel || d.guide);
}

// Los días del viaje, uno por fecha, conservando lo que ya estaba cargado
// (cambiar las fechas no puede borrar los ítems de los días que siguen adentro).
function rebuildDays(trip: Trip): void {
  const old = new Map(trip.days.map((d) => [d.date, d]));
  const out: TripDay[] = [];
  const span = Math.min(daysBetween(trip.start, trip.end), MAX_DAYS - 1);
  for (let i = 0; i <= span; i++) {
    const date = addDays(trip.start, i);
    out.push(old.get(date) ?? { date, items: [] });
    old.delete(date);
  }
  // Lo que quedó afuera del rango pero tiene cosas cargadas no se tira: se
  // arrastra para que el usuario lo vea y decida.
  for (const leftover of old.values()) if (dayHasContent(leftover)) out.push(leftover);
  out.sort((a, b) => a.date.localeCompare(b.date));
  trip.days = out;
}

function sortItems(day: TripDay): void {
  day.items.sort((a, b) => (a.at ?? "99:99").localeCompare(b.at ?? "99:99") || a.title.localeCompare(b.title));
}

function findItem(trip: Trip, itemId: string): { day: TripDay; item: TripItem } | null {
  for (const day of trip.days) {
    const item = day.items.find((i) => i.id === itemId);
    if (item) return { day, item };
  }
  return null;
}

function findDay(trip: Trip, date: string): { day: TripDay; index: number } | null {
  const index = trip.days.findIndex((d) => d.date === date);
  return index >= 0 ? { day: trip.days[index], index } : null;
}

// El resumen de lugares del viaje: lo que el usuario escribió o, si no, los
// lugares distintos de los días en orden ("Roma · crucero · Madrid").
export function placeSummary(trip: Trip): string {
  if (trip.place) return trip.place;
  const seen: string[] = [];
  for (const d of trip.days) {
    const p = (d.place ?? "").trim();
    if (p && !seen.some((s) => norm(s) === norm(p))) seen.push(p);
  }
  return seen.join(" · ");
}

// ---------------------------------------------------------------- idiomas

const KIND_LABEL: Record<Lang, Record<ItemKind, string>> = {
  es: { flight: "Vuelo", train: "Tren", hotel: "Hotel", ticket: "Entrada", meal: "Comida", visit: "Visita", other: "Otro" },
  en: { flight: "Flight", train: "Train", hotel: "Hotel", ticket: "Ticket", meal: "Meal", visit: "Visit", other: "Other" },
  fr: { flight: "Vol", train: "Train", hotel: "Hôtel", ticket: "Billet", meal: "Repas", visit: "Visite", other: "Autre" },
  de: { flight: "Flug", train: "Zug", hotel: "Hotel", ticket: "Ticket", meal: "Essen", visit: "Besuch", other: "Sonstiges" },
  pt: { flight: "Voo", train: "Trem", hotel: "Hotel", ticket: "Ingresso", meal: "Refeição", visit: "Visita", other: "Outro" },
  ru: { flight: "Рейс", train: "Поезд", hotel: "Отель", ticket: "Билет", meal: "Еда", visit: "Экскурсия", other: "Другое" },
};

export function kindLabel(kind: ItemKind, lang: Lang): string {
  return KIND_LABEL[lang]?.[kind] ?? KIND_LABEL.es[kind];
}

// Las preguntas que la app hace por voz antes de generar la guía de un día,
// los rótulos del progreso, y las etiquetas que van en el texto de un papel.
// Español neutro.
const T: Record<Lang, {
  qHotel: (when: string) => string;
  qArrival: (place: string) => string;
  qInterests: string;
  guideStart: (when: string) => string;
  searching: (near: string) => string;
  writing: (words: number) => string;
  saving: string;
  yourDay: string;
  noGuide: string;
  noDay: string;
  code: string; date: string; noKeyList: string;
  reminder: (title: string, at: string) => string;
}> = {
  es: {
    qHotel: (w) => `¿Dónde duermes la noche del ${w}?`,
    qArrival: (p) => p ? `¿Cómo y a qué hora llegas a ${p} ese día?` : "¿Cómo y a qué hora llegas ese día?",
    qInterests: "¿Qué te interesa para ese día? Por ejemplo comida, historia, arte, naturaleza, salir de noche.",
    guideStart: (w) => `Preparando la guía del ${w}…`,
    searching: (n) => `Buscando cerca de ${n}…`,
    writing: (n) => `Escribiendo la guía… ${n} palabras`,
    saving: "Guardando la guía…",
    yourDay: "tu día",
    noGuide: "sin guía",
    noDay: "ese día no está en el viaje",
    code: "Código", date: "Fecha", noKeyList: "Carga la clave de las apps en la web para separar lo dicho; mientras tanto se agregó tal cual.",
    reminder: (t, at) => `En 2 h: ${t} (${at})`,
  },
  en: {
    qHotel: (w) => `Where do you sleep on the night of ${w}?`,
    qArrival: (p) => p ? `How and at what time do you arrive in ${p} that day?` : "How and at what time do you arrive that day?",
    qInterests: "What interests you for that day? For example food, history, art, nature, nightlife.",
    guideStart: (w) => `Preparing the guide for ${w}…`,
    searching: (n) => `Searching near ${n}…`,
    writing: (n) => `Writing the guide… ${n} words`,
    saving: "Saving the guide…",
    yourDay: "your day",
    noGuide: "no guide",
    noDay: "that day is not in the trip",
    code: "Code", date: "Date", noKeyList: "Add the apps key on the web to split what you said; it was added as is for now.",
    reminder: (t, at) => `In 2 h: ${t} (${at})`,
  },
  fr: {
    qHotel: (w) => `Où dors-tu la nuit du ${w} ?`,
    qArrival: (p) => p ? `Comment et à quelle heure arrives-tu à ${p} ce jour-là ?` : "Comment et à quelle heure arrives-tu ce jour-là ?",
    qInterests: "Qu'est-ce qui t'intéresse pour ce jour-là ? Par exemple la cuisine, l'histoire, l'art, la nature, les sorties.",
    guideStart: (w) => `Préparation du guide du ${w}…`,
    searching: (n) => `Recherche autour de ${n}…`,
    writing: (n) => `Rédaction du guide… ${n} mots`,
    saving: "Enregistrement du guide…",
    yourDay: "ta journée",
    noGuide: "pas de guide",
    noDay: "ce jour n'est pas dans le voyage",
    code: "Code", date: "Date", noKeyList: "Ajoute la clé des apps sur le web pour séparer ce qui a été dit ; ajouté tel quel pour l'instant.",
    reminder: (t, at) => `Dans 2 h : ${t} (${at})`,
  },
  de: {
    qHotel: (w) => `Wo übernachtest du in der Nacht vom ${w}?`,
    qArrival: (p) => p ? `Wie und um wie viel Uhr kommst du an dem Tag in ${p} an?` : "Wie und um wie viel Uhr kommst du an dem Tag an?",
    qInterests: "Was interessiert dich an dem Tag? Zum Beispiel Essen, Geschichte, Kunst, Natur, Nachtleben.",
    guideStart: (w) => `Der Reiseführer für den ${w} wird vorbereitet…`,
    searching: (n) => `Suche in der Nähe von ${n}…`,
    writing: (n) => `Der Reiseführer wird geschrieben… ${n} Wörter`,
    saving: "Der Reiseführer wird gespeichert…",
    yourDay: "dein Tag",
    noGuide: "kein Reiseführer",
    noDay: "dieser Tag gehört nicht zur Reise",
    code: "Code", date: "Datum", noKeyList: "Trag den Apps-Schlüssel im Web ein, damit das Gesagte aufgeteilt wird; vorerst wurde es so übernommen.",
    reminder: (t, at) => `In 2 h: ${t} (${at})`,
  },
  pt: {
    qHotel: (w) => `Onde você dorme na noite de ${w}?`,
    qArrival: (p) => p ? `Como e a que horas você chega a ${p} nesse dia?` : "Como e a que horas você chega nesse dia?",
    qInterests: "O que te interessa nesse dia? Por exemplo comida, história, arte, natureza, vida noturna.",
    guideStart: (w) => `Preparando o guia de ${w}…`,
    searching: (n) => `Buscando perto de ${n}…`,
    writing: (n) => `Escrevendo o guia… ${n} palavras`,
    saving: "Salvando o guia…",
    yourDay: "seu dia",
    noGuide: "sem guia",
    noDay: "esse dia não está na viagem",
    code: "Código", date: "Data", noKeyList: "Carregue a chave dos apps na web para separar o que foi dito; por enquanto foi adicionado como está.",
    reminder: (t, at) => `Em 2 h: ${t} (${at})`,
  },
  ru: {
    qHotel: (w) => `Где вы ночуете ${w}?`,
    qArrival: (p) => p ? `Как и во сколько вы прибываете в ${p} в этот день?` : "Как и во сколько вы прибываете в этот день?",
    qInterests: "Что вам интересно в этот день? Например еда, история, искусство, природа, ночная жизнь.",
    guideStart: (w) => `Готовится путеводитель на ${w}…`,
    searching: (n) => `Ищу рядом с ${n}…`,
    writing: (n) => `Пишу путеводитель… ${n} слов`,
    saving: "Сохраняю путеводитель…",
    yourDay: "ваш день",
    noGuide: "нет путеводителя",
    noDay: "этого дня нет в поездке",
    code: "Код", date: "Дата", noKeyList: "Добавьте ключ приложений на сайте, чтобы разделить сказанное; пока добавлено как есть.",
    reminder: (t, at) => `Через 2 ч: ${t} (${at})`,
  },
};

// Fechas para las pantallas. Se arman en UTC a propósito: la fecha del viaje
// es una fecha civil, no un instante, y pasarla por una zona la corre un día.
function fmt(date: string, lang: Lang, opts: Intl.DateTimeFormatOptions): string {
  try {
    return new Intl.DateTimeFormat(lang, { ...opts, timeZone: "UTC" }).format(new Date(`${date}T12:00:00Z`));
  } catch {
    return date;
  }
}

export function dayLabel(date: string, lang: Lang): string {
  return fmt(date, lang, { weekday: "long", day: "numeric", month: "long" }).replace(",", "");
}

export function dayShort(date: string, lang: Lang): string {
  return fmt(date, lang, { weekday: "short", day: "numeric" }).replace(",", "").replace(/\.$/, "");
}

export function shortDate(date: string, lang: Lang): string {
  return fmt(date, lang, { day: "numeric", month: "short" }).replace(/\.$/, "");
}

// "16 de septiembre", "September 16": para las preguntas del día.
export function longDate(date: string, lang: Lang): string {
  return fmt(date, lang, { day: "numeric", month: "long" });
}

// "14 – 22 de septiembre" (mismo mes) o "28 de septiembre – 3 de octubre".
export function rangeText(start: string, end: string, lang: Lang): string {
  if (start === end) return fmt(start, lang, { day: "numeric", month: "long" });
  try {
    const f = new Intl.DateTimeFormat(lang, { day: "numeric", month: "long", timeZone: "UTC" });
    if (typeof f.formatRange === "function") return f.formatRange(new Date(`${start}T12:00:00Z`), new Date(`${end}T12:00:00Z`)).replace(/\s?[-–—]\s?/, " – ");
  } catch { /* abajo */ }
  return `${fmt(start, lang, { day: "numeric", month: "long" })} – ${fmt(end, lang, { day: "numeric", month: "long" })}`;
}

function paperLine(p: Paper, lang: Lang): string {
  const fields = Object.entries(p.fields ?? {}).filter(([k, v]) => k && v);
  if (fields.length) return fields.slice(0, 2).map(([k, v]) => `${k}: ${v}`).join(" · ");
  const first = p.text.replace(/\s+/g, " ").trim().slice(0, 60);
  return first || kindLabel(p.kind, lang);
}

// El texto de un papel para el visor: campos primero, el texto entero después.
export function paperText(p: Paper, lang: Lang): string {
  const t = T[lang];
  const head: string[] = [];
  if (p.date) head.push(`${t.date}: ${p.date}`);
  for (const [k, v] of Object.entries(p.fields ?? {})) if (k && v) head.push(`${k}: ${v}`);
  return [head.join("\n"), p.text.trim()].filter(Boolean).join("\n\n");
}

// ---------------------------------------------------------------- proyección al calendario

function addHour(hhmm: string): string {
  const [h, m] = hhmm.split(":").map(Number);
  const t = Math.min(23 * 60 + 59, h * 60 + m + 60);
  return `${String(Math.floor(t / 60)).padStart(2, "0")}:${String(t % 60).padStart(2, "0")}`;
}

type CalRaw = { version?: number; events?: unknown[] };

// `calendar.ts` muestra como "trip" todo evento con `tripId`. Para que el viaje
// se vea ahí sin que existan dos verdades, los ítems CON HORA se ESPEJAN: la
// fuente sigue siendo el documento `trips` y este espejo se rehace entero
// (borrar los del viaje y volver a escribirlos) en cada cambio. Con el viaje
// borrado, deja el calendario sin sus eventos.
export async function syncCalendar(accountId: number, tripId: string): Promise<number> {
  const trip = (await loadTrips(accountId)).find((t) => t.id === tripId) ?? null;
  const shapeCal = (raw: unknown): CalRaw => {
    const cal = (raw && typeof raw === "object" ? raw : {}) as CalRaw;
    cal.version ??= 1;
    if (!Array.isArray(cal.events)) cal.events = [];
    return cal;
  };
  return mutateDoc(accountId, "calendar", shapeCal, (cal) => {
    const all = Array.isArray(cal.events) ? cal.events : [];
    const mine = new Map<string, Record<string, unknown>>();
    const rest: unknown[] = [];
    for (const raw of all) {
      const e = raw as Record<string, unknown> | null;
      if (e && typeof e === "object" && String(e.tripId ?? "") === tripId) mine.set(String(e.tripItem ?? ""), e);
      else rest.push(raw);
    }
    // Los id son números y tienen que ser únicos en todo el archivo; el del
    // ítem que ya estaba se conserva para no romper nada que lo apunte.
    let free = 0;
    for (const raw of all) {
      const n = Math.floor(Number((raw as Record<string, unknown> | null)?.id));
      if (Number.isFinite(n)) free = Math.max(free, n);
    }
    const out = rest;
    let n = 0;
    if (trip) {
      trip.days.forEach((day, i) => {
        for (const item of day.items) {
          if (!item.at) continue;
          const old = mine.get(item.id);
          const keep = Math.floor(Number(old?.id));
          const place = item.place || day.place || trip.place || "";
          const note = [item.code ? `${T.es.code}: ${item.code}` : "", item.note ?? ""].filter(Boolean).join(" · ");
          out.push({
            id: Number.isFinite(keep) && keep > 0 ? keep : ++free,
            title: item.title,
            start: `${day.date}T${item.at}`,
            end: `${day.date}T${addHour(item.at)}`,
            allDay: false,
            ...(place ? { place } : {}),
            ...(note ? { note } : {}),
            tripId: trip.id,
            tripDay: i + 1,
            tripItem: item.id,
            tripKind: item.kind,
          });
          n++;
        }
      });
    }
    out.sort((a, b) =>
      String((a as Record<string, unknown> | null)?.start ?? "").localeCompare(String((b as Record<string, unknown> | null)?.start ?? "")));
    cal.version = 1;
    cal.events = out;
    return n;
  });
}

// ---------------------------------------------------------------- recordatorios

// El recordatorio de "2 h antes" vive en store.ts como cualquier otro (suena en
// el aparato con RTC y todo). La hora del ítem está en la zona del VIAJE; el
// store guarda hora local de la CUENTA, así que se pasa por epoch.
async function reminderDueAt(accountId: number, trip: Trip, date: string, at: string): Promise<string> {
  const tz = trip.timezone && tzOk(trip.timezone) ? trip.timezone : await timeZoneOf(accountId);
  const epoch = zonedToEpoch(date, at, tz) - REMIND_BEFORE_S;
  return epochToLocal(epoch);
}

async function setReminder(accountId: number, tripId: string, itemId: string, on: boolean, lang: Lang): Promise<number | undefined> {
  const trip = (await loadTrips(accountId)).find((t) => t.id === tripId);
  if (!trip) throw new Error("not found");
  const found = findItem(trip, itemId);
  if (!found) throw new Error("not found");
  const { day, item } = found;
  const oldId = item.reminderId;
  let newId: number | undefined;
  if (on) {
    if (!item.at) throw new Error("el ítem no tiene hora");
    const dueAt = await reminderDueAt(accountId, trip, day.date, item.at);
    newId = await mutateStore(accountId, (store) => {
      // Si ya había uno, se reescribe en su lugar (la hora pudo cambiar).
      const existing = oldId ? store.reminders.find((r) => r.id === oldId) : undefined;
      if (existing) {
        existing.title = T[lang].reminder(item.title, item.at as string);
        existing.dueAt = dueAt;
        existing.done = false;
        return existing.id;
      }
      const id = nextId(store);
      store.reminders.push({ id, title: T[lang].reminder(item.title, item.at as string), dueAt, repeat: NO_REPEAT, done: false, createdAt: new Date().toISOString() });
      return id;
    });
  } else if (oldId) {
    await mutateStore(accountId, (store) => {
      store.reminders = store.reminders.filter((r) => r.id !== oldId);
    });
  }
  await update(accountId, (store) => {
    const t = store.trips.find((x) => x.id === tripId);
    const it = t && findItem(t, itemId)?.item;
    if (!it) return;
    if (newId) it.reminderId = newId;
    else delete it.reminderId;
  });
  return newId;
}

async function dropReminders(accountId: number, ids: number[]): Promise<void> {
  const set = new Set(ids.filter((n) => Number.isFinite(n) && n > 0));
  if (!set.size) return;
  await mutateStore(accountId, (store) => {
    store.reminders = store.reminders.filter((r) => !set.has(r.id));
  });
}

function reminderIdsOf(trip: Trip, items?: TripItem[]): number[] {
  const list = items ?? trip.days.flatMap((d) => d.items);
  return list.map((i) => i.reminderId).filter((n): n is number => typeof n === "number");
}

// ---------------------------------------------------------------- vistas

function tripStats(t: Trip) {
  const guideDays = t.days.filter((d) => !!d.guide).length;
  return {
    itemCount: t.days.reduce((n, d) => n + d.items.length, 0),
    paperCount: t.papers.length,
    packDone: t.packing.filter((p) => p.done).length,
    packTotal: t.packing.length,
    // `guideReady` del contrato v1 se lee ahora como "algún día tiene guía";
    // `guideDays`/`dayCount` dicen cuántos ("Guía · 3 de 13 días").
    guideReady: guideDays > 0,
    guideDays,
    dayCount: t.days.length,
  };
}

function listRow(t: Trip, lang: Lang, active: boolean) {
  return {
    id: t.id,
    name: t.name,
    place: placeSummary(t),
    start: t.start,
    end: t.end,
    when: rangeText(t.start, t.end, lang),
    active,
    ...tripStats(t),
  };
}

// La vista compacta del contrato: lo que la app guarda en viaje.json. Tope de
// 40 KB (el aparato la parsea entera en el heap interno): si se pasa, se
// recortan las notas, primero a 80 caracteres y después a cero.
export async function compactView(accountId: number, trip: Trip, lang: Lang) {
  const tz = trip.timezone && tzOk(trip.timezone) ? trip.timezone : await timeZoneOf(accountId);
  const build = (noteMax: number) => ({
    id: trip.id,
    name: trip.name,
    place: placeSummary(trip),
    start: trip.start,
    end: trip.end,
    when: rangeText(trip.start, trip.end, lang),
    today: todayIn(tz),
    days: trip.days.map((d, i) => ({
      date: d.date,
      n: i + 1,
      label: dayLabel(d.date, lang),
      short: dayShort(d.date, lang),
      place: d.place ?? "",
      hotel: d.hotel ?? "",
      note: (d.note ?? "").slice(0, noteMax),
      guide: { ready: !!d.guide, at: d.guide?.at ?? 0 },
      items: d.items.map((it) => ({
        id: it.id,
        at: it.at ?? "",
        title: it.title,
        kind: it.kind,
        kindLabel: kindLabel(it.kind, lang),
        place: it.place ?? "",
        code: it.code ?? "",
        note: (it.note ?? "").slice(0, noteMax),
        paperId: it.paperId ?? "",
        remind: !!it.reminderId,
      })),
    })),
    packing: trip.packing.map((p) => ({ id: p.id, text: p.text, done: p.done })),
    papers: trip.papers.map((p) => ({ id: p.id, date: p.date ?? "", kind: p.kind, title: p.title, line: paperLine(p, lang) })),
  });
  for (const noteMax of [400, 80, 0]) {
    const view = build(noteMax);
    if (Buffer.byteLength(JSON.stringify(view)) <= MAX_VIEW_BYTES) return view;
  }
  return build(0);
}

// La vista de la web: todo, papeles con su texto incluido, y de cada guía sólo
// la ficha (el texto se pide aparte, por día).
function webView(trip: Trip, lang: Lang, active: boolean) {
  return {
    id: trip.id,
    name: trip.name,
    place: trip.place,                 // lo que escribió el usuario (vacío = se arma solo)
    placeSummary: placeSummary(trip),
    timezone: trip.timezone ?? "",
    start: trip.start,
    end: trip.end,
    when: rangeText(trip.start, trip.end, lang),
    notes: trip.notes ?? "",
    active,
    days: trip.days.map((d, i) => ({
      date: d.date,
      n: i + 1,
      label: `${dayLabel(d.date, lang)} · día ${i + 1}`,
      place: d.place ?? "",
      hotel: d.hotel ?? "",
      note: d.note ?? "",
      guide: {
        ready: !!d.guide,
        at: d.guide?.at ?? 0,
        answers: d.guide?.answers ?? {},
        chars: d.guide?.text.length ?? 0,
        job: d.guideJob ?? null,
      },
      items: d.items.map((it) => ({
        id: it.id, at: it.at ?? "", title: it.title, kind: it.kind, kindLabel: kindLabel(it.kind, lang),
        place: it.place ?? "", code: it.code ?? "", note: it.note ?? "", paperId: it.paperId ?? "", reminderId: it.reminderId ?? 0,
      })),
    })),
    packing: trip.packing,
    papers: trip.papers.map((p) => ({ id: p.id, title: p.title, date: p.date ?? "", kind: p.kind, kindLabel: kindLabel(p.kind, lang), text: p.text, fields: p.fields ?? {}, itemId: p.itemId ?? "", line: paperLine(p, lang) })),
    ...tripStats(trip),
  };
}

// ---------------------------------------------------------------- guía: preguntas y contexto

export type GuideQuestion = { key: string; text: string };

// Las preguntas de UN día, decididas mirando el itinerario y sin modelo:
// `hotel` si el día no tiene hotel; `llegada` si el día cambia de lugar
// respecto del anterior (o es el primero) y no hay vuelo ni tren cargado ese
// día; `intereses` siempre (es lo que la agenda no dice). Si el itinerario ya
// lo dice, no se pregunta.
export function dayGuideQuestions(trip: Trip, index: number, lang: Lang): GuideQuestion[] {
  const t = T[lang];
  const day = trip.days[index];
  const out: GuideQuestion[] = [];
  if (!day.hotel) out.push({ key: "hotel", text: t.qHotel(longDate(day.date, lang)) });
  const prev = index > 0 ? trip.days[index - 1] : undefined;
  const changes = !prev || norm(day.place ?? "") !== norm(prev.place ?? "");
  const hasTransport = day.items.some((i) => i.kind === "flight" || i.kind === "train");
  if (changes && !hasTransport) out.push({ key: "llegada", text: t.qArrival(day.place ?? "") });
  out.push({ key: "intereses", text: t.qInterests });
  return out;
}

function itemLine(it: TripItem, lang: Lang): string {
  return [it.at || "sin hora", `[${kindLabel(it.kind, lang)}]`, it.title, it.place ? `en ${it.place}` : "", it.code ? `código ${it.code}` : "", it.note ?? ""].filter(Boolean).join(" · ");
}

// "Roma (14-16) · crucero (17-20) · Madrid (21-26)": el recorrido del viaje
// en una línea, para que el modelo sepa de dónde se viene y a dónde se va.
function routeLine(trip: Trip): string {
  const legs: { place: string; from: string; to: string }[] = [];
  for (const d of trip.days) {
    const p = (d.place ?? "").trim();
    if (!p) continue;
    const last = legs[legs.length - 1];
    if (last && norm(last.place) === norm(p)) last.to = d.date;
    else legs.push({ place: p, from: d.date, to: d.date });
  }
  return legs.map((l) => `${l.place} (${l.from.slice(5)}${l.to !== l.from ? ` a ${l.to.slice(5)}` : ""})`).join(" · ");
}

// El viaje entero como texto para el modelo (agenda con lugar y hotel de cada
// día, papeles, lo que se sabe). `guideOf` es la fecha cuya guía se adjunta.
function tripContext(trip: Trip, lang: Lang, guideOf = ""): string {
  const lines: string[] = [];
  const summary = placeSummary(trip);
  lines.push(`Viaje: ${trip.name}${summary && summary !== trip.name ? ` (${summary})` : ""}`);
  lines.push(`Fechas: del ${trip.start} al ${trip.end} (${Math.max(1, trip.days.length)} días)`);
  if (trip.timezone) lines.push(`Zona horaria: ${trip.timezone}`);
  const route = routeLine(trip);
  if (route) lines.push(`Recorrido: ${route}`);
  if (trip.notes) lines.push(`Notas del viaje: ${trip.notes}`);
  lines.push("");
  lines.push("Agenda (cada día con su lugar y el hotel de esa noche):");
  trip.days.forEach((d, i) => {
    const bits = [`Día ${i + 1} · ${dayLabel(d.date, lang)} (${d.date})`, d.place ? `lugar: ${d.place}` : "", d.hotel ? `hotel: ${d.hotel}` : "", d.guide ? "(tiene guía)" : ""].filter(Boolean);
    lines.push(`${bits.join(" · ")}${d.note ? ` — ${d.note}` : ""}`);
    if (!d.items.length) lines.push("  (libre)");
    for (const it of d.items) lines.push(`  ${itemLine(it, lang)}`);
  });
  if (trip.packing.length) {
    lines.push("");
    lines.push(`Lista para llevar: ${trip.packing.map((p) => `${p.done ? "[x]" : "[ ]"} ${p.text}`).join(", ")}`);
  }
  if (trip.papers.length) {
    lines.push("");
    lines.push("Papeles (texto de cada reserva, recortado):");
    for (const p of trip.papers) {
      lines.push(`--- ${p.title}${p.date ? ` (${p.date})` : ""} [${kindLabel(p.kind, lang)}]`);
      for (const [k, v] of Object.entries(p.fields ?? {})) if (k && v) lines.push(`${k}: ${v}`);
      lines.push(p.text.replace(/\s+/g, " ").trim().slice(0, 1500));
    }
  }
  const g = guideOf ? trip.days.find((d) => d.date === guideOf)?.guide : undefined;
  if (g) {
    lines.push("");
    lines.push(`Guía del día ${guideOf} (ya escrita para el viajero):`);
    lines.push(g.text.trim());
  }
  return lines.join("\n");
}

// Lo que el modelo recibe para escribir la guía de UN día: el viaje en dos
// líneas, y ese día entero (lugar, de dónde se viene y a dónde se va, hotel,
// agenda, respuestas del viajero).
function dayGuideContext(trip: Trip, index: number, answers: Record<string, string>, lang: Lang): string {
  const day = trip.days[index];
  const prev = index > 0 ? trip.days[index - 1] : undefined;
  const next = index + 1 < trip.days.length ? trip.days[index + 1] : undefined;
  const lines: string[] = [];
  const summary = placeSummary(trip);
  lines.push(`Viaje: ${trip.name}${summary && summary !== trip.name ? ` (${summary})` : ""}`);
  lines.push(`Fechas del viaje: del ${trip.start} al ${trip.end} (${trip.days.length} días)`);
  if (trip.timezone) lines.push(`Zona horaria: ${trip.timezone}`);
  const route = routeLine(trip);
  if (route) lines.push(`Recorrido: ${route}`);
  if (trip.notes) lines.push(`Notas del viaje: ${trip.notes}`);
  lines.push("");
  lines.push(`EL DÍA DE ESTA GUÍA: día ${index + 1} de ${trip.days.length} · ${dayLabel(day.date, lang)} (${day.date})`);
  lines.push(`Lugar del día: ${day.place || "(no cargado: dedúcelo de la agenda y del recorrido)"}`);
  if (!prev) lines.push("Es el primer día del viaje (día de llegada).");
  else if (norm(prev.place ?? "") !== norm(day.place ?? "")) lines.push(`Viene de: ${prev.place || "(sin lugar cargado)"} (el día anterior) — es un día de LLEGADA a este lugar.`);
  if (!next) lines.push("Es el último día del viaje (día de salida).");
  else if (norm(next.place ?? "") !== norm(day.place ?? "")) lines.push(`Al día siguiente sigue a: ${next.place || "(sin lugar cargado)"} — es el último día en este lugar.`);
  lines.push(`Hotel de esa noche: ${day.hotel || answers.hotel || "(no cargado)"}`);
  if (day.note) lines.push(`Nota del día: ${day.note}`);
  lines.push("");
  lines.push("Agenda del día:");
  if (!day.items.length) lines.push("  (nada cargado todavía)");
  for (const it of day.items) lines.push(`  ${itemLine(it, lang)}`);
  const said = Object.entries(answers).filter(([, v]) => v);
  if (said.length) {
    lines.push("");
    lines.push("Lo que dijo el viajero para este día:");
    for (const [k, v] of said) lines.push(`  ${k}: ${v}`);
  }
  return lines.join("\n");
}

function guideSystem(lang: Lang): string {
  return [
    "Eres el autor de una guía de viaje a medida, hecha para leerse en un lector de tinta electrónica de pantalla chica, sin imágenes ni enlaces.",
    `Escribes en ${LANGUAGE_NAME[lang]}.`,
    "Reglas de forma: texto plano en párrafos cortos separados por una línea en blanco; sin markdown, sin viñetas con asteriscos, sin encabezados, sin tablas, sin direcciones de internet. Para enumerar usa una línea por cosa, con el nombre primero y un guion. No anuncies lo que vas a contar. Entra en materia desde la primera frase.",
    "Reglas de fondo: concreto y verificable (nombres, direcciones o zonas, horarios, precios aproximados con moneda y de cuándo es el dato). Busca en internet lo que pueda haber cambiado (restaurantes, precios, horarios, huelgas, feriados, clima) y di de cuándo son las reseñas o los datos que uses. Si algo no lo puedes confirmar, dilo en vez de inventarlo.",
    "Escribes la guía de UN SOLO DÍA de un viaje de varios lugares: tienes el lugar de ese día, de dónde viene y a dónde sigue el viajero, su hotel, lo que ya tiene agendado y lo que te dijo. Adapta todo a ESE día en ESE lugar, no a un turista genérico ni al viaje entero.",
  ].join("\n");
}

const GUIDE_BRIEF = [
  "Escribe la guía de ESE día. Entre 600 y 1000 palabras, sólo el texto. Tiene que cubrir, en este orden y sin encabezados:",
  "- qué hay CERCA de cada cosa ya agendada (si hay Casa Batlló, qué hay alrededor a pie y qué conviene encadenar con ella);",
  "- qué se estaría perdiendo uno en ese lugar ese día, dicho para que decida (dos o tres cosas, con el porqué);",
  "- cómo moverse entre las cosas del día (a pie, metro, bus, taxi o app; tiempos y precios aproximados) y desde el hotel;",
  "- dónde comer cerca de cada cosa (con reseñas recientes, di de cuándo son; horarios de comida del lugar);",
  "- lo práctico de ESA fecha: horarios de apertura y cierres de ese día de la semana, entradas y si conviene reservar, huelgas, feriados o eventos que caigan ese día, y consejos según el clima previsto para esa fecha;",
  "- si es un día de crucero, de isla o de tránsito (llegada, salida, tren, vuelo), qué entra de verdad en el tiempo disponible, contando traslados y esperas.",
  "Si el día no tiene nada cargado, arma la propuesta a partir del lugar, del hotel y de los intereses del viajero.",
].join("\n");

// ---------------------------------------------------------------- helpers de texto

const norm = (s: string) => s.toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/[^\p{L}\p{N}]+/gu, " ").trim();

function addPacking(trip: Trip, text: string): PackItem | null {
  const clean = text.replace(/\s+/g, " ").trim().slice(0, 120);
  if (!clean) return null;
  const key = norm(clean);
  const same = trip.packing.find((p) => norm(p.text) === key);
  if (same) {
    same.done = false;
    return same;
  }
  if (trip.packing.length >= MAX_PACKING) return null;
  const item: PackItem = { id: newId(), text: clean, done: false };
  trip.packing.push(item);
  return item;
}

function removePacking(trip: Trip, text: string): boolean {
  const key = norm(text);
  if (!key) return false;
  const before = trip.packing.length;
  trip.packing = trip.packing.filter((p) => {
    const k = norm(p.text);
    return !(k === key || k.includes(key) || key.includes(k));
  });
  return trip.packing.length !== before;
}

// Sin clave del modelo: un reparto a mano ("cargador, adaptador y quita el
// paraguas"). Alcanza para no dejar la fila muda.
function naiveSplit(said: string): { add: string[]; remove: string[] } {
  const add: string[] = [];
  const remove: string[] = [];
  for (const raw of said.split(/[,;]| y | and | et | und | e | и /i)) {
    const part = raw.trim();
    if (!part) continue;
    const m = /^(quita|saca|elimina|borra|sin|remove|delete|enlève|retire|streich|entferne|tira|remova|убери|удали)\s+(el |la |los |las |un |una |the |le |la |les |der |die |das |o |a |os |as )?(.+)$/i.exec(part);
    if (m) remove.push(m[3].trim());
    else add.push(part);
  }
  return { add, remove };
}

function parseFields(v: unknown): Record<string, string> | undefined {
  const out: Record<string, string> = {};
  if (v && typeof v === "object" && !Array.isArray(v)) {
    for (const [k, val] of Object.entries(v as Record<string, unknown>)) {
      const key = str(k, 40), value = str(val, 200);
      if (key && value) out[key] = value;
      if (Object.keys(out).length >= 20) break;
    }
  } else if (typeof v === "string") {
    for (const line of v.split("\n")) {
      const at = line.indexOf(":");
      if (at <= 0) continue;
      const key = line.slice(0, at).trim().slice(0, 40), value = line.slice(at + 1).trim().slice(0, 200);
      if (key && value) out[key] = value;
      if (Object.keys(out).length >= 20) break;
    }
  }
  return Object.keys(out).length ? out : undefined;
}

function parseAnswers(raw: unknown): Record<string, string> {
  const answers: Record<string, string> = {};
  if (raw && typeof raw === "object" && !Array.isArray(raw)) {
    for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
      const key = str(k, 20), val = str(v, 400);
      if (key && val) answers[key] = val;
    }
  }
  return answers;
}

// ---------------------------------------------------------------- rutas web

export const tripsApi = new Hono<AppEnv>();   // GET /api/trips
export const tripApi = new Hono<AppEnv>();    // /api/trip*

tripsApi.get("/", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const trips = await loadTrips(acc);
  const today = todayIn(await timeZoneOf(acc));
  const active = pickActive(trips, today);
  const rows = trips
    .map((t) => ({ ...listRow(t, lang, active?.id === t.id), state: today < t.start ? "next" : today > t.end ? "past" : "now" }))
    .sort((a, b) => a.start.localeCompare(b.start));
  return c.json({ ok: true, today, trips: rows });
});

tripApi.get("/", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const id = (c.req.query("id") ?? "").toString();
  const trips = await loadTrips(acc);
  const today = todayIn(await timeZoneOf(acc));
  const active = pickActive(trips, today);
  const trip = id ? trips.find((t) => t.id === id) : active;
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, today, trip: webView(trip, lang, active?.id === trip.id) });
});

// Crear o editar. Sin `id` crea; con `id` cambia nombre, resumen de lugares,
// fechas, zona y notas, y rearma los días sin perder lo cargado.
tripApi.post("/", async (c) => {
  const b = await readBody(c);
  const name = str(b.name, 80);
  const start = (b.start ?? "").toString();
  const end = ((b.end ?? "").toString() || start);
  if (!name) return c.json({ ok: false, error: "falta el nombre" }, 400);
  if (!isDate(start) || !isDate(end)) return c.json({ ok: false, error: "las fechas van como YYYY-MM-DD" }, 400);
  if (daysBetween(start, end) < 0) return c.json({ ok: false, error: "la vuelta es antes de la ida" }, 400);
  if (daysBetween(start, end) > MAX_DAYS) return c.json({ ok: false, error: `el viaje no puede pasar de ${MAX_DAYS} días` }, 400);
  const timezone = str(b.timezone, 64);
  if (timezone && !tzOk(timezone)) return c.json({ ok: false, error: "zona horaria desconocida (va como Europe/Madrid)" }, 400);

  const res = await update(accountOf(c), (store) => {
    const id = (b.id ?? "").toString();
    let trip = id ? store.trips.find((t) => t.id === id) : undefined;
    if (id && !trip) return { error: "not found" as const };
    if (!trip) {
      if (store.trips.length >= MAX_TRIPS) return { error: "demasiados viajes" as const };
      trip = { id: newId(), name, place: "", start, end, days: [], packing: [], papers: [] };
      store.trips.push(trip);
      // El primero que se carga queda activo solo.
      if (store.trips.length === 1) trip.active = true;
    }
    const tzBefore = trip.timezone ?? "";
    trip.name = name;
    trip.start = start;
    trip.end = end;
    // Vacío vale: el resumen se arma con los lugares de los días.
    if (b.place !== undefined) trip.place = str(b.place, 120);
    if (b.timezone !== undefined) {
      if (timezone) trip.timezone = timezone;
      else delete trip.timezone;
    }
    const notes = str(b.notes, 1000);
    if (notes) trip.notes = notes; else delete trip.notes;
    if (b.active === true) {
      for (const t of store.trips) delete t.active;
      trip.active = true;
    }
    rebuildDays(trip);
    return { trip, tzChanged: (trip.timezone ?? "") !== tzBefore };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, res.error === "not found" ? 404 : 400);
  await syncCalendar(accountOf(c), res.trip.id);
  // Con otra zona horaria, los recordatorios de "2 h antes" caen en otro
  // instante: se recalculan (los ítems con hora y recordatorio puesto).
  if (res.tzChanged) {
    for (const d of res.trip.days) for (const it of d.items) {
      if (!it.reminderId || !it.at) continue;
      try {
        await setReminder(accountOf(c), res.trip.id, it.id, true, normalizeLang(c.req.query("lang")));
      } catch (err) {
        console.error("trip tz reminder:", err);
      }
    }
  }
  return c.json({ ok: true, id: res.trip.id });
});

tripApi.post("/delete", async (c) => {
  const acc = accountOf(c);
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const gone = await update(acc, (store) => {
    const trip = store.trips.find((t) => t.id === id);
    if (!trip) return null;
    store.trips = store.trips.filter((t) => t.id !== id);
    return reminderIdsOf(trip);
  });
  if (!gone) return c.json({ ok: false, error: "not found" }, 404);
  await syncCalendar(acc, id);  // el viaje ya no está: esto le saca los eventos al calendario
  await dropReminders(acc, gone);
  return c.json({ ok: true });
});

tripApi.post("/active", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const ok = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === id);
    if (!trip) return false;
    for (const t of store.trips) delete t.active;
    trip.active = true;
    return true;
  });
  return ok ? c.json({ ok: true, id }) : c.json({ ok: false, error: "not found" }, 404);
});

// El día: su lugar, el hotel de esa noche y la nota ("día libre", "hay que
// estar 2 h antes"). Sólo se toca lo que viene en el cuerpo, así la hoja de la
// nota no borra el lugar ni el editor del día borra la nota.
tripApi.post("/day", async (c) => {
  const acc = accountOf(c);
  const b = await readBody(c);
  const date = (b.date ?? "").toString();
  if (!isDate(date)) return c.json({ ok: false, error: "fecha inválida" }, 400);
  const tripId = (b.tripId ?? "").toString();
  const res = await update(acc, (store) => {
    const trip = store.trips.find((t) => t.id === tripId);
    if (!trip) return false;
    const day = trip.days.find((d) => d.date === date);
    if (!day) return false;
    const set = (key: "place" | "hotel" | "note", max: number) => {
      if (b[key] === undefined) return;
      const v = str(b[key], max);
      if (v) day[key] = v;
      else delete day[key];
    };
    set("place", 80);
    set("hotel", 120);
    set("note", 300);
    return true;
  });
  if (!res) return c.json({ ok: false, error: "not found" }, 404);
  if (b.place !== undefined) await syncCalendar(acc, tripId);  // el lugar del día va en los eventos sin lugar propio
  return c.json({ ok: true });
});

// Un ítem del día: hora, título, tipo, lugar, código, nota y papel. Sin `id`
// es nuevo; con `id` se edita el que está (y si cambió de día, se muda).
tripApi.post("/day/item", async (c) => {
  const acc = accountOf(c);
  const b = await readBody(c);
  const date = (b.date ?? "").toString();
  const title = str(b.title, 120);
  if (!isDate(date)) return c.json({ ok: false, error: "fecha inválida" }, 400);
  if (!title) return c.json({ ok: false, error: "falta el título" }, 400);
  const at = str(b.at, 5);
  if (at && !TIME_RE.test(at)) return c.json({ ok: false, error: "la hora va como HH:MM" }, 400);
  const kind = kindOf(b.kind);
  const tripId = (b.tripId ?? "").toString();

  const res = await update(acc, (store) => {
    const trip = store.trips.find((t) => t.id === tripId);
    if (!trip) return { error: "not found" as const };
    const id = (b.id ?? "").toString();
    let item: TripItem | undefined;
    if (id) {
      const found = findItem(trip, id);
      if (!found) return { error: "not found" as const };
      item = found.item;
      if (found.day.date !== date) found.day.items = found.day.items.filter((i) => i.id !== id);
    }
    let day = trip.days.find((d) => d.date === date);
    if (!day) {
      // Una fecha fuera del rango (el vuelo de vuelta que cae un día después):
      // se agrega el día en vez de rechazarlo.
      day = { date, items: [] };
      trip.days.push(day);
      trip.days.sort((x, y) => x.date.localeCompare(y.date));
    }
    if (!item) {
      item = { id: newId(), title, kind };
      day.items.push(item);
    } else if (!day.items.includes(item)) day.items.push(item);
    item.title = title;
    item.kind = kind;
    if (at) item.at = at; else delete item.at;
    const place = str(b.place, 100);
    if (place) item.place = place; else delete item.place;
    const code = str(b.code, 40);
    if (code) item.code = code; else delete item.code;
    const note = str(b.note, 400);
    if (note) item.note = note; else delete item.note;
    const paperId = str(b.paperId, 20);
    if (paperId && trip.papers.some((p) => p.id === paperId)) item.paperId = paperId; else delete item.paperId;
    sortItems(day);
    return { id: item.id, reminderId: item.reminderId, at: item.at, date: day.date };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, 404);
  await syncCalendar(acc, tripId);
  // Si tenía recordatorio, se recalcula con la hora nueva (o se va sin hora).
  if (res.reminderId) {
    try {
      await setReminder(acc, tripId, res.id, !!res.at, normalizeLang(c.req.query("lang")));
    } catch (err) {
      console.error("trip item reminder:", err);
    }
  }
  return c.json({ ok: true, id: res.id });
});

tripApi.post("/day/item/delete", async (c) => {
  const acc = accountOf(c);
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const tripId = (b.tripId ?? "").toString();
  const gone = await update(acc, (store) => {
    const trip = store.trips.find((t) => t.id === tripId);
    if (!trip) return null;
    const found = findItem(trip, id);
    if (!found) return null;
    found.day.items = found.day.items.filter((i) => i.id !== id);
    for (const p of trip.papers) if (p.itemId === id) delete p.itemId;
    return found.item.reminderId ? [found.item.reminderId] : [];
  });
  if (!gone) return c.json({ ok: false, error: "not found" }, 404);
  await syncCalendar(acc, tripId);
  await dropReminders(acc, gone);
  return c.json({ ok: true });
});

// Lista de cosas para llevar: alta, tilde y baja en el mismo endpoint.
tripApi.post("/packing", async (c) => {
  const b = await readBody(c);
  const action = (b.action ?? "").toString();
  const res = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return { error: "not found" as const };
    const id = (b.id ?? "").toString();
    if (action === "delete") {
      const before = trip.packing.length;
      trip.packing = trip.packing.filter((p) => p.id !== id);
      return before === trip.packing.length ? { error: "not found" as const } : { id };
    }
    let item = id ? trip.packing.find((p) => p.id === id) : undefined;
    if (id && !item) return { error: "not found" as const };
    if (!item) {
      const text = str(b.text, 120);
      if (!text) return { error: "falta el texto" as const };
      const added = addPacking(trip, text);
      if (!added) return { error: "la lista está llena" as const };
      item = added;
    } else {
      const text = str(b.text, 120);
      if (text) item.text = text;
      if (typeof b.done === "boolean") item.done = b.done;
      else if (b.done === undefined && text === "") item.done = !item.done;
    }
    return { id: item.id, done: item.done };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, res.error === "not found" ? 404 : 400);
  return c.json({ ok: true, ...res });
});

// Un papel: título, fecha, tipo, el texto pegado y campos "Etiqueta: valor"
// (como objeto o como líneas). Sin `id` es nuevo.
tripApi.post("/paper", async (c) => {
  const b = await readBody(c);
  const title = str(b.title, 120);
  if (!title) return c.json({ ok: false, error: "falta el título" }, 400);
  const date = (b.date ?? "").toString();
  if (date && !isDate(date)) return c.json({ ok: false, error: "fecha inválida" }, 400);
  const res = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return { error: "not found" as const };
    const id = (b.id ?? "").toString();
    let paper = id ? trip.papers.find((p) => p.id === id) : undefined;
    if (id && !paper) return { error: "not found" as const };
    if (!paper) {
      if (trip.papers.length >= MAX_PAPERS) return { error: "demasiados papeles" as const };
      paper = { id: newId(), title, kind: "other", text: "" };
      trip.papers.push(paper);
    }
    paper.title = title;
    paper.kind = kindOf(b.kind);
    if (date) paper.date = date; else delete paper.date;
    if (b.text !== undefined) paper.text = (b.text ?? "").toString().replace(/\r/g, "").trim().slice(0, MAX_PAPER_TEXT);
    const fields = parseFields(b.fields);
    if (b.fields !== undefined) { if (fields) paper.fields = fields; else delete paper.fields; }
    const itemId = str(b.itemId, 20);
    if (b.itemId !== undefined) {
      const found = itemId ? findItem(trip, itemId) : null;
      // El vínculo va en los dos sentidos: el ítem sabe su papel y el papel su ítem.
      for (const d of trip.days) for (const it of d.items) if (it.paperId === paper.id && it.id !== itemId) delete it.paperId;
      if (found) { paper.itemId = itemId; found.item.paperId = paper.id; }
      else delete paper.itemId;
    }
    trip.papers.sort((x, y) => (x.date ?? "9999").localeCompare(y.date ?? "9999") || x.title.localeCompare(y.title));
    return { id: paper.id };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, res.error === "not found" ? 404 : 400);
  return c.json({ ok: true, id: res.id });
});

tripApi.post("/paper/delete", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const gone = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return false;
    const before = trip.papers.length;
    trip.papers = trip.papers.filter((p) => p.id !== id);
    for (const d of trip.days) for (const it of d.items) if (it.paperId === id) delete it.paperId;
    return trip.papers.length !== before;
  });
  return gone ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
});

// La guía de UN día, por la web: las preguntas, generar, leer.
tripApi.get("/:id/day/:date/guide/questions", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const trip = await getTrip(accountOf(c), c.req.param("id"));
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  const found = findDay(trip, c.req.param("date"));
  if (!found) return c.json({ ok: false, error: T[lang].noDay }, 404);
  return c.json({ ok: true, questions: dayGuideQuestions(trip, found.index, lang) });
});

tripApi.post("/:id/day/:date/guide", async (c) => {
  const b = await readBody(c);
  const lang = normalizeLang(c.req.query("lang"));
  try {
    const jobId = await generateDayGuide(accountOf(c), c.req.param("id"), c.req.param("date"), b.answers, lang);
    return c.json({ ok: true, jobId });
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return c.json({ ok: false, error: msg, code: err instanceof AppsLlmError ? err.code : "error" }, msg === "not found" ? 404 : 400);
  }
});

tripApi.get("/:id/day/:date/guide", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const trip = await getTrip(accountOf(c), c.req.param("id"));
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  const g = findDay(trip, c.req.param("date"))?.day.guide;
  if (!g) return c.json({ ok: false, error: T[lang].noGuide }, 404);
  return c.json({ ok: true, text: g.text.slice(0, MAX_GUIDE_TEXT), at: g.at, answers: g.answers });
});

// Reparación a mano: vuelve a escribir el espejo del viaje en el calendario.
tripApi.post("/sync", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const ids = id ? [id] : (await loadTrips(accountOf(c))).map((t) => t.id);
  let n = 0;
  for (const t of ids) n += await syncCalendar(accountOf(c), t);
  return c.json({ ok: true, events: n });
});

// ---------------------------------------------------------------- la guía de un día (trabajo)

// Lo que se nombra en el rótulo "Buscando cerca de …": la primera cosa
// agendada con lugar o título, si no el lugar del día, si no "tu día".
function nearLabel(day: TripDay, lang: Lang): string {
  const first = day.items.find((it) => it.kind !== "flight" && it.kind !== "train" && it.kind !== "hotel") ?? day.items[0];
  return (first?.title || day.place || T[lang].yourDay).slice(0, 60);
}

async function generateDayGuide(accountId: number, id: string, date: string, rawAnswers: unknown, lang: Lang): Promise<string> {
  const trip = await getTrip(accountId, id);
  if (!trip) throw new Error("not found");
  const found = findDay(trip, date);
  if (!found) throw new Error(T[lang].noDay);
  const { day, index } = found;
  const answers = parseAnswers(rawAnswers);
  // Un trabajo en curso de hace menos de 20 minutos no se duplica.
  if (day.guideJob && Date.now() - day.guideJob.at < 20 * 60 * 1000) return day.guideJob.id;
  // Sin clave se contesta acá, en el acto: arrancar un trabajo que muere en la
  // primera llamada deja al aparato sondeando `job.status` para leer lo mismo.
  if (!(await config()).apps.key) throw new AppsLlmError(NO_KEY_MSG, "no_key");
  const t = T[lang];
  const tripId = trip.id;
  const when = longDate(day.date, lang);

  const jobId = await startJob(accountId, t.guideStart(when), async (job): Promise<JobFile[]> => {
    const system: Anthropic.TextBlockParam[] = [
      { type: "text", text: guideSystem(lang) },
      { type: "text", text: dayGuideContext(trip, index, answers, lang), cache_control: { type: "ephemeral" } },
    ];
    const t0 = Date.now();
    let lastProgress = 0;
    try {
      await job.setProgress(1, 3, t.searching(nearLabel(day, lang)));
      const r = await appsProseSearch({
        accountId,
        system,
        user: GUIDE_BRIEF,
        maxTokens: 6000,
        maxUses: 6,
        // El progreso mientras escribe, sin escribir el documento a cada
        // delta: una vez cada 3 s alcanza para que la pantalla se mueva.
        onProgress: (chars) => {
          const now = Date.now();
          if (now - lastProgress < 3000) return;
          lastProgress = now;
          void job.setProgress(2, 3, t.writing(Math.round(chars / 6))).catch(() => {});
        },
      });
      const text = r.text.replace(/\r/g, "").replace(/\*\*/g, "").replace(/^#+\s*/gm, "").trim().slice(0, MAX_GUIDE_TEXT);
      if (!text) throw new Error("la guía salió vacía");
      await job.setProgress(3, 3, t.saving);
      await update(accountId, (store) => {
        const tr = store.trips.find((x) => x.id === tripId);
        const d = tr && tr.days.find((x) => x.date === date);
        if (!d) return;
        d.guide = { at: Math.floor(Date.now() / 1000), answers, text };
        // Si el viajero acaba de decir dónde duerme, eso es el hotel del día.
        if (!d.hotel && answers.hotel) d.hotel = answers.hotel.slice(0, 120);
        delete d.guideJob;
      });
      console.log(`viajes ${job.id}: guía del ${date} · ${text.length} caracteres · ${r.sources.length} fuentes · ${Math.round((Date.now() - t0) / 1000)} s`);
    } catch (err) {
      await update(accountId, (store) => {
        const tr = store.trips.find((x) => x.id === tripId);
        const d = tr && tr.days.find((x) => x.date === date);
        if (d) delete d.guideJob;
      });
      throw err;
    }
    return [];
  });
  await update(accountId, (store) => {
    const tr = store.trips.find((x) => x.id === tripId);
    const d = tr && tr.days.find((x) => x.date === date);
    if (d) d.guideJob = { id: jobId, at: Date.now() };
  });
  return jobId;
}

// ---------------------------------------------------------------- servicios de la app

function argStr(v: unknown, max = 40): string {
  return typeof v === "string" ? v.trim().slice(0, max) : "";
}

async function needTrip(accountId: number, id: unknown): Promise<Trip> {
  const trip = await getTrip(accountId, argStr(id));
  if (!trip) throw new Error("no hay ningún viaje cargado");
  return trip;
}

function needDay(trip: Trip, date: unknown, lang: Lang): { day: TripDay; index: number } {
  const found = findDay(trip, argStr(date, 10));
  if (!found) throw new Error(T[lang].noDay);
  return found;
}

const lista: Service = async (ctx) => {
  const trips = await loadTrips(ctx.accountId);
  const active = pickActive(trips, todayIn(await timeZoneOf(ctx.accountId)));
  return { trips: trips.slice().sort((a, b) => a.start.localeCompare(b.start)).map((t) => listRow(t, ctx.lang, active?.id === t.id)) };
};

const activar: Service = async (ctx, args) => {
  const id = argStr(args.id);
  const ok = await update(ctx.accountId, (store) => {
    const trip = store.trips.find((t) => t.id === id);
    if (!trip) return false;
    for (const t of store.trips) delete t.active;
    trip.active = true;
    return true;
  });
  if (!ok) throw new Error("viaje desconocido");
  return { id };
};

const viaje: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  return { trip: await compactView(ctx.accountId, trip, ctx.lang) };
};

const papel: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const p = trip.papers.find((x) => x.id === argStr(args.paperId));
  if (!p) throw new Error("papel desconocido");
  return { title: p.title, text: paperText(p, ctx.lang) };
};

const LLEVAR_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["add", "remove"],
  properties: {
    add: { type: "array", items: { type: "string" }, description: "Cada cosa que el viajero quiere AGREGAR a la lista, una por elemento, en singular y con mayúscula inicial ('Cargador', 'Adaptador de enchufe'). Vacío si no agrega nada." },
    remove: { type: "array", items: { type: "string" }, description: "Cada cosa que quiere QUITAR de la lista ('quita el paraguas' → 'paraguas'), tal como la nombró. Vacío si no quita nada." },
  },
};

const llevar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const action = argStr(args.action);
  let note = "";
  if (action === "add") {
    const said = argStr(args.text, 500);
    if (!said) throw new Error("no se dijo nada");
    let split: { add: string[]; remove: string[] };
    try {
      const raw = await appsJson<{ add?: unknown; remove?: unknown }>({
        accountId: ctx.accountId,
        system: `Eres el asistente de una lista de cosas para llevar a un viaje. El viajero dicta por voz, así que el texto puede venir coloquial o con errores de transcripción. Separa lo dicho en altas y bajas. Los nombres van en ${LANGUAGE_NAME[ctx.lang]}.`,
        user: `Lista actual: ${trip.packing.map((p) => p.text).join(", ") || "(vacía)"}\n\nDijo: "${said}"`,
        schema: LLEVAR_SCHEMA,
        maxTokens: 800,
      });
      const clean = (v: unknown) => (Array.isArray(v) ? v.map((x) => argStr(x, 120)).filter(Boolean).slice(0, 30) : []);
      split = { add: clean(raw.add), remove: clean(raw.remove) };
    } catch (err) {
      if (!(err instanceof AppsLlmError && err.code === "no_key")) throw err;
      split = naiveSplit(said);
      note = T[ctx.lang].noKeyList;
    }
    await update(ctx.accountId, (store) => {
      const t = store.trips.find((x) => x.id === trip.id);
      if (!t) return;
      for (const r of split.remove) removePacking(t, r);
      for (const a of split.add) addPacking(t, a);
    });
  } else if (action === "toggle" || action === "remove") {
    const itemId = argStr(args.itemId);
    const ok = await update(ctx.accountId, (store) => {
      const t = store.trips.find((x) => x.id === trip.id);
      const p = t?.packing.find((x) => x.id === itemId);
      if (!t || !p) return false;
      if (action === "remove") t.packing = t.packing.filter((x) => x.id !== itemId);
      else p.done = !p.done;
      return true;
    });
    if (!ok) throw new Error("ítem desconocido");
  } else throw new Error("acción desconocida");
  const fresh = await needTrip(ctx.accountId, trip.id);
  return { packing: fresh.packing, ...(note ? { note } : {}) };
};

const SUGERIR_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["suggestions"],
  properties: {
    suggestions: { type: "array", items: { type: "string" }, description: "Hasta 12 cosas concretas para llevar que NO están en la lista, en singular y con mayúscula inicial, de la más a la menos importante." },
  },
};

const sugerir: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const raw = await appsJson<{ suggestions?: unknown }>({
    accountId: ctx.accountId,
    system: `Eres quien ayuda a armar la valija para un viaje. Sugiere solo cosas concretas y útiles para ESTE viaje (los lugares de cada día, las fechas y la estación, lo que hay en la agenda: un vuelo pide auriculares y almohada, una playa pide protector, un crucero pide lo suyo). Nada que ya esté en la lista. Los nombres van en ${LANGUAGE_NAME[ctx.lang]}.`,
    user: `${tripContext(trip, ctx.lang)}\n\nSugiere hasta 12 cosas que faltan en la lista.`,
    schema: SUGERIR_SCHEMA,
    maxTokens: 800,
  });
  const have = new Set(trip.packing.map((p) => norm(p.text)));
  const out: string[] = [];
  for (const s of Array.isArray(raw.suggestions) ? raw.suggestions : []) {
    const text = argStr(s, 120);
    if (!text || have.has(norm(text)) || out.some((o) => norm(o) === norm(text))) continue;
    out.push(text);
    if (out.length >= 12) break;
  }
  return { suggestions: out };
};

const sugerirAgregar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const texts = Array.isArray(args.texts) ? args.texts.map((x) => argStr(x, 120)).filter(Boolean).slice(0, 30) : [];
  if (!texts.length) throw new Error("no hay nada que agregar");
  await update(ctx.accountId, (store) => {
    const t = store.trips.find((x) => x.id === trip.id);
    if (!t) return;
    for (const text of texts) addPacking(t, text);
  });
  const fresh = await needTrip(ctx.accountId, trip.id);
  return { packing: fresh.packing };
};

const guiaPreguntas: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const { index } = needDay(trip, args.date, ctx.lang);
  return { questions: dayGuideQuestions(trip, index, ctx.lang) };
};

const guiaGenerar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const { day } = needDay(trip, args.date, ctx.lang);
  return { jobId: await generateDayGuide(ctx.accountId, trip.id, day.date, args.answers, ctx.lang) };
};

const guiaDia: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const { day } = needDay(trip, args.date, ctx.lang);
  if (!day.guide) throw new Error(T[ctx.lang].noGuide);
  return { text: day.guide.text.slice(0, MAX_GUIDE_TEXT), at: day.guide.at };
};

// Preguntar: el viaje entero en el system (cacheado), con el lugar y el hotel
// de cada día y la guía del día del que se habla (o la de hoy), y la pregunta
// con el día o el ítem. Busca en internet SOLO si el usuario lo dijo
// ("busca…"): regla de la casa (1.5.103), la misma que en Hablar.
const preguntar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const question = argStr(args.question, 600);
  if (!question) throw new Error("no se dijo la pregunta");
  const date = argStr(args.date, 10);
  const itemId = argStr(args.itemId);
  const tz = trip.timezone && tzOk(trip.timezone) ? trip.timezone : await timeZoneOf(ctx.accountId);
  const today = todayIn(tz);
  const focus: string[] = [];
  let guideOf = "";
  if (isDate(date)) {
    const f = findDay(trip, date);
    focus.push(`La pregunta es sobre el día ${f ? f.index + 1 : "?"} (${dayLabel(date, ctx.lang)}, ${date})${f?.day.place ? `, en ${f.day.place}` : ""}.`);
    guideOf = date;
  }
  if (itemId) {
    const f = findItem(trip, itemId);
    if (f) {
      focus.push(`La pregunta es sobre este ítem: ${f.item.at ?? ""} ${f.item.title}${f.item.place ? ` (${f.item.place})` : ""}${f.item.code ? `, código ${f.item.code}` : ""}, del ${f.day.date}.`);
      if (!guideOf) guideOf = f.day.date;
    }
  }
  if (!guideOf && today && trip.days.some((d) => d.date === today)) guideOf = today;
  const system: Anthropic.TextBlockParam[] = [
    {
      type: "text",
      text: [
        "Eres el asistente de viaje del lector: tienes su viaje entero (agenda con el lugar y el hotel de cada día, papeles, y la guía del día si la hay) y respondes con eso primero. Si lo que pregunta no está en el viaje, responde con lo que sabes y dilo.",
        `Respondes en ${LANGUAGE_NAME[ctx.lang]}, en texto plano, sin markdown ni listas con asteriscos, en pocas frases claras (la pantalla es chica).`,
        "Al final, en una línea aparte que empiece exactamente con 'HABLADO:', escribe una versión de la respuesta de hasta 200 caracteres para leerla en voz alta (solo lo esencial, sin códigos largos ni direcciones de internet).",
      ].join("\n"),
    },
    { type: "text", text: tripContext(trip, ctx.lang, guideOf), cache_control: { type: "ephemeral" } },
  ];
  const user = [today ? `Hoy es ${today}.` : "", ...focus, `Pregunta: ${question}`].filter(Boolean).join("\n");
  const search = asksForSearch(question, ctx.lang);
  const text = search
    ? (await appsProseSearch({ accountId: ctx.accountId, system, user, maxTokens: 2000, maxUses: 3 })).text
    : await appsProse({ accountId: ctx.accountId, system, user, maxTokens: 2000 });
  const lines = text.replace(/\r/g, "").split("\n");
  let spoken = "";
  for (let i = lines.length - 1; i >= 0; i--) {
    const m = /^\s*\**\s*(HABLADO|SPOKEN|ORAL|GESPROCHEN|FALADO|ВСЛУХ)\s*:\s*\**\s*(.*)$/i.exec(lines[i]);
    if (m) {
      spoken = [m[2], ...lines.slice(i + 1)].join(" ").replace(/\s+/g, " ").trim();
      lines.length = i;
      break;
    }
  }
  const answer = lines.join("\n").replace(/\*\*/g, "").trim();
  if (!answer) throw new Error("el modelo no respondió");
  if (!spoken) spoken = answer.replace(/\s+/g, " ");
  if (spoken.length > 220) spoken = spoken.slice(0, 217).replace(/\s+\S*$/, "") + "…";
  console.log(`viajes preguntar: ${question.length} caracteres, web=${search ? "si" : "no"}, guía=${guideOf || "no"}`);
  return { answer, spoken };
};

const recordar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const itemId = argStr(args.itemId);
  const on = args.on === true || args.on === "true" || args.on === 1;
  const reminderId = await setReminder(ctx.accountId, trip.id, itemId, on, ctx.lang);
  return reminderId ? { reminderId } : {};
};

export const VIAJES_SERVICES: Record<string, Service> = {
  "viajes.lista": lista,
  "viajes.activar": activar,
  "viajes.viaje": viaje,
  "viajes.papel": papel,
  "viajes.llevar": llevar,
  "viajes.sugerir": sugerir,
  "viajes.sugerir.agregar": sugerirAgregar,
  "viajes.guia.preguntas": guiaPreguntas,
  "viajes.guia.generar": guiaGenerar,
  "viajes.guia.dia": guiaDia,
  "viajes.preguntar": preguntar,
  "viajes.recordar": recordar,
};
