// Viajes: agenda día por día, papeles (texto), lista para llevar y guía, para
// la app de Lua `viajes` y para la pestaña Viajes de /board.
//
// Resucitado de `d13923b^:server/src/trips.ts` con lo que dice
// docs/ws397/VIAJES_CONTRATO.md: SIN adjuntos (los papeles son texto pegado con
// título, fecha, tipo y campos), SIN diario, con `hotel`, `code` por ítem,
// `papers[]` por viaje, la guía guardada en el viaje y un viaje `active` por
// cuenta.
//
// La fuente de verdad es el documento `trips` (/data/trips.json sin base de
// datos). El calendario muestra como "trip" cualquier evento con `tripId`, así
// que los ítems CON HORA se espejan ahí con `syncCalendar()`: el espejo se
// rehace entero en cada cambio y nunca se edita a mano.
//
// Rutas de la web (montadas en api.ts, heredan el Bearer / la sesión):
//   GET  /api/trips?lang=                 -> lista de viajes
//   GET  /api/trip?id=&lang=              -> un viaje entero
//   POST /api/trip                        -> crear o editar {id?, name, place, lat, lon, timezone, start, end, hotel, notes, active}
//   POST /api/trip/delete                 -> {id}
//   POST /api/trip/active                 -> {id}
//   POST /api/trip/day                    -> {tripId, date, note}
//   POST /api/trip/day/item               -> crear o editar un ítem del día
//   POST /api/trip/day/item/delete        -> {tripId, date, id}
//   POST /api/trip/packing                -> {tripId, id?, text?, done?, action?}
//   POST /api/trip/paper                  -> crear o editar un papel {tripId, id?, title, date, kind, text, fields, itemId}
//   POST /api/trip/paper/delete           -> {tripId, id}
//   GET  /api/trip/guide/questions?id=    -> las preguntas que faltan antes de generar
//   POST /api/trip/guide/generate         -> {id, answers} -> {jobId}
//   GET  /api/trip/guide/section?id=&n=   -> una sección de la guía
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
import { weatherLineAt } from "./hub";
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

export type TripDay = { date: string; note?: string; items: TripItem[] };
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
export type GuideSection = { n: number; title: string; text: string };
export type Guide = { at: number; answers: Record<string, string>; sections: GuideSection[] };

export type Trip = {
  id: string;
  name: string;
  place: string;
  lat?: number;
  lon?: number;
  timezone?: string;
  start: string;        // "YYYY-MM-DD"
  end: string;
  hotel?: string;
  notes?: string;
  active?: boolean;
  days: TripDay[];
  packing: PackItem[];
  papers: Paper[];
  guide?: Guide;
  // Mientras se genera la guía: el trabajo en curso, para que la web y la app
  // puedan retomar el `job.status` si se fueron en el medio.
  guideJob?: { id: string; at: number };
};

type Store = { version: number; trips: Trip[] };

const EMPTY: Store = { version: 1, trips: [] };
const MAX_DAYS = 90;
const MAX_TRIPS = 40;
const MAX_PACKING = 200;
const MAX_PAPERS = 60;
const MAX_PAPER_TEXT = 24 * 1024;
const MAX_SECTION_TEXT = 24 * 1024;
const MAX_VIEW_BYTES = 40 * 1024;
const GUIDE_SECTIONS = 10;
const REMIND_BEFORE_S = 2 * 3600;

// ---------------------------------------------------------------- storage

function str(v: unknown, max: number): string {
  return typeof v === "string" ? v.trim().slice(0, max) : "";
}

function kindOf(v: unknown): ItemKind {
  return KINDS.includes(v as ItemKind) ? (v as ItemKind) : "other";
}

function shapeTrips(raw: unknown): Store {
  const store = (raw && typeof raw === "object" ? raw : structuredClone(EMPTY)) as Store;
  store.version ??= 1;
  if (!Array.isArray(store.trips)) store.trips = [];
  for (const t of store.trips) {
    if (!Array.isArray(t.days)) t.days = [];
    for (const d of t.days) if (!Array.isArray(d.items)) d.items = [];
    if (!Array.isArray(t.packing)) t.packing = [];
    if (!Array.isArray(t.papers)) t.papers = [];
    if (typeof t.place !== "string") t.place = "";
    if (t.guide && !Array.isArray(t.guide.sections)) delete t.guide;
  }
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
  for (const leftover of old.values()) if (leftover.items.length || leftover.note) out.push(leftover);
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

// Las preguntas que la app hace por voz antes de generar la guía, y las
// etiquetas que van en el texto de un papel. Español neutro.
const T: Record<Lang, {
  qHotel: string; qArrival: string; qDeparture: string; qInterests: string;
  section: (n: number) => string; code: string; date: string; noKeyList: string;
  reminder: (title: string, at: string) => string;
}> = {
  es: {
    qHotel: "¿Dónde te alojas? Di el nombre del hotel o la zona.",
    qArrival: "¿Cómo y a qué hora llegas el primer día?",
    qDeparture: "¿Cómo y a qué hora te vas el último día?",
    qInterests: "¿Qué te interesa más de este viaje? Por ejemplo comida, historia, arte, naturaleza, salir de noche.",
    section: (n) => `Sección ${n} de ${GUIDE_SECTIONS}`, code: "Código", date: "Fecha", noKeyList: "Carga la clave de las apps en la web para separar lo dicho; mientras tanto se agregó tal cual.",
    reminder: (t, at) => `En 2 h: ${t} (${at})`,
  },
  en: {
    qHotel: "Where are you staying? Say the hotel name or the area.",
    qArrival: "How and at what time do you arrive on the first day?",
    qDeparture: "How and at what time do you leave on the last day?",
    qInterests: "What interests you most on this trip? For example food, history, art, nature, nightlife.",
    section: (n) => `Section ${n} of ${GUIDE_SECTIONS}`, code: "Code", date: "Date", noKeyList: "Add the apps key on the web to split what you said; it was added as is for now.",
    reminder: (t, at) => `In 2 h: ${t} (${at})`,
  },
  fr: {
    qHotel: "Où loges-tu ? Dis le nom de l'hôtel ou le quartier.",
    qArrival: "Comment et à quelle heure arrives-tu le premier jour ?",
    qDeparture: "Comment et à quelle heure repars-tu le dernier jour ?",
    qInterests: "Qu'est-ce qui t'intéresse le plus pour ce voyage ? Par exemple la cuisine, l'histoire, l'art, la nature, les sorties.",
    section: (n) => `Section ${n} sur ${GUIDE_SECTIONS}`, code: "Code", date: "Date", noKeyList: "Ajoute la clé des apps sur le web pour séparer ce qui a été dit ; ajouté tel quel pour l'instant.",
    reminder: (t, at) => `Dans 2 h : ${t} (${at})`,
  },
  de: {
    qHotel: "Wo übernachtest du? Sag den Namen des Hotels oder das Viertel.",
    qArrival: "Wie und um wie viel Uhr kommst du am ersten Tag an?",
    qDeparture: "Wie und um wie viel Uhr reist du am letzten Tag ab?",
    qInterests: "Was interessiert dich auf dieser Reise am meisten? Zum Beispiel Essen, Geschichte, Kunst, Natur, Nachtleben.",
    section: (n) => `Abschnitt ${n} von ${GUIDE_SECTIONS}`, code: "Code", date: "Datum", noKeyList: "Trag den Apps-Schlüssel im Web ein, damit das Gesagte aufgeteilt wird; vorerst wurde es so übernommen.",
    reminder: (t, at) => `In 2 h: ${t} (${at})`,
  },
  pt: {
    qHotel: "Onde você se hospeda? Diga o nome do hotel ou a região.",
    qArrival: "Como e a que horas você chega no primeiro dia?",
    qDeparture: "Como e a que horas você vai embora no último dia?",
    qInterests: "O que mais te interessa nesta viagem? Por exemplo comida, história, arte, natureza, vida noturna.",
    section: (n) => `Seção ${n} de ${GUIDE_SECTIONS}`, code: "Código", date: "Data", noKeyList: "Carregue a chave dos apps na web para separar o que foi dito; por enquanto foi adicionado como está.",
    reminder: (t, at) => `Em 2 h: ${t} (${at})`,
  },
  ru: {
    qHotel: "Где вы остановитесь? Назовите отель или район.",
    qArrival: "Как и во сколько вы прибываете в первый день?",
    qDeparture: "Как и во сколько вы уезжаете в последний день?",
    qInterests: "Что вам интереснее всего в этой поездке? Например еда, история, искусство, природа, ночная жизнь.",
    section: (n) => `Раздел ${n} из ${GUIDE_SECTIONS}`, code: "Код", date: "Дата", noKeyList: "Добавьте ключ приложений на сайте, чтобы разделить сказанное; пока добавлено как есть.",
    reminder: (t, at) => `Через 2 ч: ${t} (${at})`,
  },
};

// Los diez títulos de la guía (VIAJES_APP.md §7), en el idioma del aparato.
const GUIDE_TITLES: Record<Lang, string[]> = {
  es: ["Para entender el lugar", "Barrios", "Imperdibles", "Para una mente curiosa", "Comer", "Moverse", "Ojo con", "Un día perfecto", "Frases útiles", "Por si acaso"],
  en: ["Understanding the place", "Neighbourhoods", "Must-sees", "For a curious mind", "Eating", "Getting around", "Watch out for", "A perfect day", "Useful phrases", "Just in case"],
  fr: ["Comprendre le lieu", "Quartiers", "Incontournables", "Pour un esprit curieux", "Manger", "Se déplacer", "Attention à", "Une journée parfaite", "Phrases utiles", "Au cas où"],
  de: ["Den Ort verstehen", "Viertel", "Unbedingt sehen", "Für neugierige Köpfe", "Essen", "Unterwegs", "Vorsicht bei", "Ein perfekter Tag", "Nützliche Sätze", "Für alle Fälle"],
  pt: ["Para entender o lugar", "Bairros", "Imperdíveis", "Para uma mente curiosa", "Comer", "Deslocar-se", "Cuidado com", "Um dia perfeito", "Frases úteis", "Por via das dúvidas"],
  ru: ["Понять место", "Районы", "Обязательно посмотреть", "Для любознательных", "Еда", "Передвижение", "Осторожно", "Идеальный день", "Полезные фразы", "На всякий случай"],
};

// Qué va en cada sección (se lo pide al modelo en español: el resultado sale
// en el idioma del aparato por el system).
const GUIDE_BRIEF: string[] = [
  "historia del lugar en épocas (cuatro o cinco, con lo que dejó cada una en lo que hoy se ve), idioma o idiomas que se hablan, moneda y cambio aproximado a la fecha, cómo se saluda y qué se considera de buena educación.",
  "los barrios: cuáles son, qué carácter tiene cada uno, dónde conviene dormir según lo que se busca, y dónde no conviene andar de noche, dicho sin alarmismo.",
  "los imperdibles: qué ver y POR QUÉ vale la pena cada uno, con el dato concreto que se puede contar después; horarios habituales y si conviene reservar.",
  "para una mente curiosa: lo que no está en las guías comunes — rarezas, historias poco conocidas, detalles de la vida cotidiana, un lugar que sólo conocen los de ahí.",
  "comer: los platos típicos y dónde probarlos, con lugares que tengan reseñas recientes (di de cuándo son), horarios de comidas, qué se pide y qué no, cuánto cuesta comer bien sin gastar de más.",
  "moverse: del aeropuerto o la estación al centro (opciones, precio y tiempo), transporte público y sus tarjetas, taxis y apps, propinas, y cómo se pagan las cosas.",
  "ojo con: estafas típicas para turistas, zonas a evitar, el clima que espera en ESAS fechas (busca el pronóstico o el clima habitual de esos días), feriados o fiestas que caen en el viaje y cómo afectan (cierres, precios).",
  "un día perfecto: un itinerario a pie del primer día libre del viaje, desde la mañana hasta la noche, con horas aproximadas, dónde comer en el camino y qué dejar para otro día.",
  "frases útiles: veinte frases del idioma del lugar para leer en el mostrador (saludos, pedir la cuenta, preguntar precios, direcciones, emergencias), cada una con su traducción y la pronunciación figurada. Si el idioma del lugar es el del lector, dedica la sección a las expresiones locales y a lo que se dice distinto.",
  "por si acaso: número de emergencias, embajada o consulado del país del lector si se puede saber (y si no, cómo encontrarlo), hospital cercano al hotel o al centro, cómo bloquear una tarjeta perdida, dónde está la comisaría de turistas, y qué hacer si se pierde el pasaporte.",
];

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
          const place = item.place || trip.place || "";
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
  return {
    itemCount: t.days.reduce((n, d) => n + d.items.length, 0),
    paperCount: t.papers.length,
    packDone: t.packing.filter((p) => p.done).length,
    packTotal: t.packing.length,
    guideReady: !!(t.guide && t.guide.sections.length),
  };
}

function listRow(t: Trip, lang: Lang, active: boolean) {
  return {
    id: t.id,
    name: t.name,
    place: t.place,
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
  const weather = trip.lat !== undefined && trip.lon !== undefined ? await weatherLineAt(trip.lat, trip.lon, tz, lang) : "";
  const build = (noteMax: number) => ({
    id: trip.id,
    name: trip.name,
    place: trip.place,
    start: trip.start,
    end: trip.end,
    when: rangeText(trip.start, trip.end, lang),
    hotel: trip.hotel ?? "",
    weather,
    today: todayIn(tz),
    days: trip.days.map((d, i) => ({
      date: d.date,
      n: i + 1,
      label: dayLabel(d.date, lang),
      short: dayShort(d.date, lang),
      note: (d.note ?? "").slice(0, noteMax),
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
    guide: {
      ready: !!(trip.guide && trip.guide.sections.length),
      at: trip.guide?.at ?? 0,
      sections: (trip.guide?.sections ?? []).map((s) => ({ n: s.n, title: s.title })),
    },
  });
  for (const noteMax of [400, 80, 0]) {
    const view = build(noteMax);
    if (Buffer.byteLength(JSON.stringify(view)) <= MAX_VIEW_BYTES) return view;
  }
  return build(0);
}

// La vista de la web: todo, papeles con su texto incluido, y de la guía solo
// los títulos (las secciones se piden de a una).
function webView(trip: Trip, lang: Lang, active: boolean) {
  return {
    id: trip.id,
    name: trip.name,
    place: trip.place,
    lat: trip.lat,
    lon: trip.lon,
    timezone: trip.timezone ?? "",
    start: trip.start,
    end: trip.end,
    when: rangeText(trip.start, trip.end, lang),
    hotel: trip.hotel ?? "",
    notes: trip.notes ?? "",
    active,
    days: trip.days.map((d, i) => ({
      date: d.date,
      n: i + 1,
      label: `${dayLabel(d.date, lang)} · día ${i + 1}`,
      note: d.note ?? "",
      items: d.items.map((it) => ({
        id: it.id, at: it.at ?? "", title: it.title, kind: it.kind, kindLabel: kindLabel(it.kind, lang),
        place: it.place ?? "", code: it.code ?? "", note: it.note ?? "", paperId: it.paperId ?? "", reminderId: it.reminderId ?? 0,
      })),
    })),
    packing: trip.packing,
    papers: trip.papers.map((p) => ({ id: p.id, title: p.title, date: p.date ?? "", kind: p.kind, kindLabel: kindLabel(p.kind, lang), text: p.text, fields: p.fields ?? {}, itemId: p.itemId ?? "", line: paperLine(p, lang) })),
    guide: {
      ready: !!(trip.guide && trip.guide.sections.length),
      at: trip.guide?.at ?? 0,
      answers: trip.guide?.answers ?? {},
      sections: (trip.guide?.sections ?? []).map((s) => ({ n: s.n, title: s.title, chars: s.text.length })),
      job: trip.guideJob ?? null,
    },
    ...tripStats(trip),
  };
}

// ---------------------------------------------------------------- guía: preguntas y contexto

export type GuideQuestion = { key: string; text: string };

// Se decide mirando el itinerario, sin modelo: hotel si no hay ni `hotel` ni
// un ítem de tipo hotel; llegada si el primer día no tiene vuelo/tren con
// hora; salida ídem el último; intereses siempre (es lo que la agenda no dice).
export function guideQuestions(trip: Trip, lang: Lang): GuideQuestion[] {
  const t = T[lang];
  const out: GuideQuestion[] = [];
  const hasHotel = !!trip.hotel || trip.days.some((d) => d.items.some((i) => i.kind === "hotel"));
  const travel = (d: TripDay | undefined) => !!d && d.items.some((i) => (i.kind === "flight" || i.kind === "train") && !!i.at);
  if (!hasHotel) out.push({ key: "hotel", text: t.qHotel });
  if (!travel(trip.days[0])) out.push({ key: "arrival", text: t.qArrival });
  if (trip.days.length > 1 && !travel(trip.days[trip.days.length - 1])) out.push({ key: "departure", text: t.qDeparture });
  out.push({ key: "interests", text: t.qInterests });
  return out.slice(0, 4);
}

// El viaje entero como texto para el modelo (agenda, papeles, lo que se sabe).
function tripContext(trip: Trip, lang: Lang, answers: Record<string, string> = {}, withGuide = false): string {
  const lines: string[] = [];
  lines.push(`Viaje: ${trip.name}${trip.place && trip.place !== trip.name ? ` (${trip.place})` : ""}`);
  lines.push(`Fechas: del ${trip.start} al ${trip.end} (${Math.max(1, trip.days.length)} días)`);
  if (trip.timezone) lines.push(`Zona horaria: ${trip.timezone}`);
  if (trip.hotel) lines.push(`Alojamiento: ${trip.hotel}`);
  if (trip.notes) lines.push(`Notas del viaje: ${trip.notes}`);
  for (const [k, v] of Object.entries(answers)) if (v) lines.push(`Respuesta del viajero (${k}): ${v}`);
  lines.push("");
  lines.push("Agenda:");
  trip.days.forEach((d, i) => {
    const head = `Día ${i + 1} · ${dayLabel(d.date, lang)} (${d.date})${d.note ? ` — ${d.note}` : ""}`;
    lines.push(head);
    if (!d.items.length) lines.push("  (libre)");
    for (const it of d.items) {
      const bits = [it.at || "sin hora", `[${kindLabel(it.kind, lang)}]`, it.title, it.place ? `en ${it.place}` : "", it.code ? `código ${it.code}` : "", it.note ?? ""].filter(Boolean);
      lines.push(`  ${bits.join(" · ")}`);
    }
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
  if (withGuide && trip.guide?.sections.length) {
    lines.push("");
    lines.push("Guía del viaje (resumen de cada sección):");
    for (const s of trip.guide.sections) lines.push(`${s.n}. ${s.title}: ${s.text.replace(/\s+/g, " ").trim().slice(0, 700)}`);
  }
  return lines.join("\n");
}

function guideSystem(lang: Lang): string {
  return [
    "Eres el autor de una guía de viaje a medida, hecha para leerse en un lector de tinta electrónica de pantalla chica, sin imágenes ni enlaces.",
    `Escribes en ${LANGUAGE_NAME[lang]}.`,
    "Reglas de forma: texto plano en párrafos cortos separados por una línea en blanco; sin markdown, sin viñetas con asteriscos, sin encabezados, sin tablas, sin direcciones de internet. Para enumerar usa una línea por cosa, con el nombre primero y un guion. No repitas el título de la sección. No anuncies lo que vas a contar. Entra en materia desde la primera frase.",
    "Reglas de fondo: concreto y verificable (nombres, direcciones o zonas, horarios, precios aproximados con moneda y de cuándo es el dato). Busca en internet lo que pueda haber cambiado (restaurantes, precios, horarios, feriados, clima) y di de cuándo son las reseñas o los datos que uses. Si algo no lo puedes confirmar, dilo en vez de inventarlo.",
    "Tienes el viaje entero (fechas, agenda, alojamiento, lo que dijo el viajero): adapta cada sección a ESE viaje, no a un turista genérico.",
  ].join("\n");
}

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

// Crear o editar. Sin `id` crea; con `id` cambia nombre, lugar, fechas, hotel y
// notas, y rearma los días sin perder lo cargado.
tripApi.post("/", async (c) => {
  const b = await readBody(c);
  const name = str(b.name, 80);
  const start = (b.start ?? "").toString();
  const end = ((b.end ?? "").toString() || start);
  if (!name) return c.json({ ok: false, error: "falta el nombre" }, 400);
  if (!isDate(start) || !isDate(end)) return c.json({ ok: false, error: "las fechas van como YYYY-MM-DD" }, 400);
  if (daysBetween(start, end) < 0) return c.json({ ok: false, error: "la vuelta es antes de la ida" }, 400);
  if (daysBetween(start, end) > MAX_DAYS) return c.json({ ok: false, error: `el viaje no puede pasar de ${MAX_DAYS} días` }, 400);
  const lat = Number(b.lat), lon = Number(b.lon);
  // `null` y "" NO son coordenadas (Number(null) es 0, y 0,0 es el golfo de Guinea).
  const given = (v: unknown) => v !== null && v !== undefined && v !== "";
  const hasCoords = given(b.lat) && given(b.lon) && Number.isFinite(lat) && Number.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
  const timezone = str(b.timezone, 64);

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
    trip.place = str(b.place, 120) || name;
    if (b.lat !== undefined || b.lon !== undefined) {
      if (hasCoords) { trip.lat = lat; trip.lon = lon; }
      else { delete trip.lat; delete trip.lon; }
    }
    if (b.timezone !== undefined) {
      if (timezone && tzOk(timezone)) trip.timezone = timezone;
      else delete trip.timezone;
    }
    const hotel = str(b.hotel, 120);
    if (hotel) trip.hotel = hotel; else delete trip.hotel;
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

// La nota del día ("día libre", "hay que estar 2 h antes").
tripApi.post("/day", async (c) => {
  const b = await readBody(c);
  const date = (b.date ?? "").toString();
  if (!isDate(date)) return c.json({ ok: false, error: "fecha inválida" }, 400);
  const res = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return false;
    const day = trip.days.find((d) => d.date === date);
    if (!day) return false;
    const note = str(b.note, 300);
    if (note) day.note = note;
    else delete day.note;
    return true;
  });
  return res ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
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

tripApi.get("/guide/questions", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const trip = await getTrip(accountOf(c), (c.req.query("id") ?? "").toString());
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, questions: guideQuestions(trip, lang) });
});

tripApi.post("/guide/generate", async (c) => {
  const b = await readBody(c);
  const lang = normalizeLang(c.req.query("lang"));
  try {
    const jobId = await generateGuide(accountOf(c), (b.id ?? "").toString(), b.answers, lang);
    return c.json({ ok: true, jobId });
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return c.json({ ok: false, error: msg, code: err instanceof AppsLlmError ? err.code : "error" }, msg === "not found" ? 404 : 400);
  }
});

tripApi.get("/guide/section", async (c) => {
  const trip = await getTrip(accountOf(c), (c.req.query("id") ?? "").toString());
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  const n = Math.floor(Number(c.req.query("n")));
  const s = trip.guide?.sections.find((x) => x.n === n);
  if (!s) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, n: s.n, title: s.title, text: s.text });
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

// ---------------------------------------------------------------- la guía (trabajo)

async function generateGuide(accountId: number, id: string, rawAnswers: unknown, lang: Lang): Promise<string> {
  const trip = await getTrip(accountId, id);
  if (!trip) throw new Error("not found");
  const answers: Record<string, string> = {};
  if (rawAnswers && typeof rawAnswers === "object" && !Array.isArray(rawAnswers)) {
    for (const [k, v] of Object.entries(rawAnswers as Record<string, unknown>)) {
      const key = str(k, 20), val = str(v, 400);
      if (key && val) answers[key] = val;
    }
  }
  // Un trabajo en curso de hace menos de 20 minutos no se duplica.
  if (trip.guideJob && Date.now() - trip.guideJob.at < 20 * 60 * 1000) return trip.guideJob.id;
  // Sin clave se contesta acá, en el acto: arrancar un trabajo que muere en la
  // primera sección deja al aparato sondeando `job.status` para leer lo mismo.
  if (!(await config()).apps.key) throw new AppsLlmError(NO_KEY_MSG, "no_key");
  const titles = GUIDE_TITLES[lang] ?? GUIDE_TITLES.es;
  const t = T[lang];
  const tripId = trip.id;

  const jobId = await startJob(accountId, t.section(1), async (job): Promise<JobFile[]> => {
    const system: Anthropic.TextBlockParam[] = [
      { type: "text", text: guideSystem(lang) },
      { type: "text", text: tripContext(trip, lang, answers), cache_control: { type: "ephemeral" } },
    ];
    const sections: GuideSection[] = [];
    const t0 = Date.now();
    try {
      for (let n = 1; n <= GUIDE_SECTIONS; n++) {
        await job.setProgress(n, GUIDE_SECTIONS, t.section(n));
        const r = await appsProseSearch({
          accountId,
          system,
          user: [
            `Escribe la sección ${n} de ${GUIDE_SECTIONS} de la guía, "${titles[n - 1]}": ${GUIDE_BRIEF[n - 1]}`,
            sections.length ? `Ya están escritas: ${sections.map((s) => s.title).join(", ")}. No repitas lo que va en ellas.` : "",
            "Largo: entre 500 y 900 palabras. Solo el texto de la sección.",
          ].filter(Boolean).join("\n\n"),
          maxTokens: 6000,
          maxUses: n === 5 || n === 7 || n === 10 ? 6 : 3,
        });
        const text = r.text.replace(/\r/g, "").replace(/\*\*/g, "").replace(/^#+\s*/gm, "").trim().slice(0, MAX_SECTION_TEXT);
        if (!text) throw new Error(`la sección ${n} salió vacía`);
        sections.push({ n, title: titles[n - 1], text });
        console.log(`viajes ${job.id}: sección ${n}/${GUIDE_SECTIONS} · ${text.length} caracteres · ${r.sources.length} fuentes · ${Math.round((Date.now() - t0) / 1000)} s`);
      }
      await update(accountId, (store) => {
        const tr = store.trips.find((x) => x.id === tripId);
        if (!tr) return;
        tr.guide = { at: Math.floor(Date.now() / 1000), answers, sections };
        delete tr.guideJob;
      });
    } catch (err) {
      await update(accountId, (store) => {
        const tr = store.trips.find((x) => x.id === tripId);
        if (tr) delete tr.guideJob;
      });
      throw err;
    }
    return [];
  });
  await update(accountId, (store) => {
    const tr = store.trips.find((x) => x.id === tripId);
    if (tr) tr.guideJob = { id: jobId, at: Date.now() };
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
  const tz = trip.timezone && tzOk(trip.timezone) ? trip.timezone : await timeZoneOf(ctx.accountId);
  const weather = trip.lat !== undefined && trip.lon !== undefined ? await weatherLineAt(trip.lat, trip.lon, tz, ctx.lang) : "";
  const raw = await appsJson<{ suggestions?: unknown }>({
    accountId: ctx.accountId,
    system: `Eres quien ayuda a armar la valija para un viaje. Sugiere solo cosas concretas y útiles para ESTE viaje (destino, fechas y estación, clima, lo que hay en la agenda: un vuelo pide auriculares y almohada, una playa pide protector). Nada que ya esté en la lista. Los nombres van en ${LANGUAGE_NAME[ctx.lang]}.`,
    user: `${tripContext(trip, ctx.lang)}${weather ? `\n\nClima ahora en el destino: ${weather}` : ""}\n\nSugiere hasta 12 cosas que faltan en la lista.`,
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
  return { questions: guideQuestions(trip, ctx.lang) };
};

const guiaGenerar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  return { jobId: await generateGuide(ctx.accountId, trip.id, args.answers, ctx.lang) };
};

const guiaSeccion: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const n = Math.floor(Number(args.n));
  const s = trip.guide?.sections.find((x) => x.n === n);
  if (!s) throw new Error("esa sección no está: la guía no se generó");
  return { n: s.n, title: s.title, text: s.text };
};

// Preguntar: el viaje entero en el system (cacheado) y la pregunta con el día o
// el ítem del que se habla. Busca en internet SOLO si el usuario lo dijo
// ("busca…"): regla de la casa (1.5.103), la misma que en Hablar.
const preguntar: Service = async (ctx, args) => {
  const trip = await needTrip(ctx.accountId, args.id);
  const question = argStr(args.question, 600);
  if (!question) throw new Error("no se dijo la pregunta");
  const date = argStr(args.date, 10);
  const itemId = argStr(args.itemId);
  const focus: string[] = [];
  if (isDate(date)) {
    const i = trip.days.findIndex((d) => d.date === date);
    focus.push(`La pregunta es sobre el día ${i >= 0 ? i + 1 : "?"} (${dayLabel(date, ctx.lang)}, ${date}).`);
  }
  if (itemId) {
    const f = findItem(trip, itemId);
    if (f) focus.push(`La pregunta es sobre este ítem: ${f.item.at ?? ""} ${f.item.title}${f.item.place ? ` (${f.item.place})` : ""}${f.item.code ? `, código ${f.item.code}` : ""}, del ${f.day.date}.`);
  }
  const tz = trip.timezone && tzOk(trip.timezone) ? trip.timezone : await timeZoneOf(ctx.accountId);
  const today = todayIn(tz);
  const system: Anthropic.TextBlockParam[] = [
    {
      type: "text",
      text: [
        "Eres el asistente de viaje del lector: tienes su viaje entero (agenda, papeles, guía) y respondes con eso primero. Si lo que pregunta no está en el viaje, responde con lo que sabes y dilo.",
        `Respondes en ${LANGUAGE_NAME[ctx.lang]}, en texto plano, sin markdown ni listas con asteriscos, en pocas frases claras (la pantalla es chica).`,
        "Al final, en una línea aparte que empiece exactamente con 'HABLADO:', escribe una versión de la respuesta de hasta 200 caracteres para leerla en voz alta (solo lo esencial, sin códigos largos ni direcciones de internet).",
      ].join("\n"),
    },
    { type: "text", text: tripContext(trip, ctx.lang, trip.guide?.answers ?? {}, true), cache_control: { type: "ephemeral" } },
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
  console.log(`viajes preguntar: ${question.length} caracteres, web=${search ? "si" : "no"}`);
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
  "viajes.guia.seccion": guiaSeccion,
  "viajes.preguntar": preguntar,
  "viajes.recordar": recordar,
};
