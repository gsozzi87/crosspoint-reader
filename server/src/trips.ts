// Viajes: la lista de lo que se hace cada día y los papeles que hacen falta.
// La idea del usuario, textual: "para un viaje quiero poder ver en el calendario
// qué voy a ir haciendo por día y poder acceder por ejemplo a los QR de los
// vuelos o los datos de una reserva o los pdf del hotel", y "ir viendo en mi
// viaje a qué hora tomar el tren, a qué hora entrar al hotel, a qué hora son las
// entradas al Vaticano".
//
// Un viaje tiene días (uno por fecha entre start y end), cada día tiene ítems
// con hora y tipo (vuelo, tren, hotel, entrada, comida, visita, otro), y cada
// ítem puede tener adjuntos (`attachments.ts`: el PDF ya convertido a bitmaps
// que el aparato pinta, con el código de barras vuelto a generar limpio).
//
// La fuente de verdad de un viaje es SIEMPRE /data/trips.json. El calendario
// (`calendar.ts`, /data/calendar.json) muestra como "trip" cualquier evento con
// `tripId`, así que los ítems se espejan ahí con `syncCalendar()`: el espejo se
// rehace entero en cada cambio y nunca se edita a mano. Quien prefiera leer los
// ítems sin pasar por el archivo tiene `tripCalendarEvents()`.
//
//   GET  /api/trips?lang=              -> lista de viajes
//   GET  /api/trip?id=&lang=           -> un viaje entero, con adjuntos resueltos
//   POST /api/trip                     -> crear o editar {id?, name, place?, start, end}
//   POST /api/trip/delete              -> {id}
//   POST /api/trip/day                 -> {tripId, date, note}
//   POST /api/trip/day/item            -> crear o editar un ítem del día
//   POST /api/trip/day/item/delete     -> {tripId, date, id}
//   POST /api/trip/packing             -> {tripId, id?, text?, done?, action?}
//   POST /api/trip/attach              -> colgar/descolgar un adjunto de un ítem o del viaje
import { Hono } from "hono";
import { mutateDoc, readDoc } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";
import { readBody } from "./net";
import { normalizeLang, type Lang } from "./lang";
import { deleteAttachmentsOfTrip, listAttachments, type Attachment } from "./attachments";
import { forgetTrip } from "./suggest";

// El calendario es de `calendar.ts`: acá solo se espeja lo del viaje adentro de
// su misma cola de escritura (ver `syncCalendar`).

export type ItemKind = "flight" | "train" | "hotel" | "ticket" | "meal" | "visit" | "other";
export const KINDS: ItemKind[] = ["flight", "train", "hotel", "ticket", "meal", "visit", "other"];

export type TripItem = {
  id: string;
  at?: string;          // "HH:MM"; sin hora, es algo del día sin horario
  title: string;
  kind: ItemKind;
  place?: string;
  note?: string;
  attachmentIds: string[];
};

export type TripDay = { date: string; note?: string; items: TripItem[] };
export type PackItem = { id: string; text: string; done: boolean };

export type Trip = {
  id: string;
  name: string;
  place?: string;
  start: string;        // "YYYY-MM-DD"
  end: string;
  days: TripDay[];
  packing: PackItem[];
  docs: string[];       // adjuntos del viaje que no cuelgan de ningún ítem
};

type Store = { version: number; trips: Trip[] };

const EMPTY: Store = { version: 1, trips: [] };
// Un viaje de más de dos meses día por día no lo carga nadie a mano, y son
// 60 objetos por viaje en el JSON del volumen.
const MAX_DAYS = 90;
const MAX_TRIPS = 40;

// ---------------------------------------------------------------- storage

// Todo lo que escribe pasa por acá: leer y escribir adentro del mismo
// `serialize()` es lo que evita que dos pedidos a la vez se pisen (uno lee,
// el otro lee lo mismo, los dos escriben y el último borra lo del primero).
// `writeJsonAtomic` sola no alcanza: encola la escritura, no la lectura.
function shapeTrips(raw: unknown): Store {
  const store = (raw && typeof raw === "object" ? raw : structuredClone(EMPTY)) as Store;
  store.version ??= 1;
  store.trips ??= [];
  return store;
}

function update<T>(accountId: number, fn: (store: Store) => T | Promise<T>): Promise<T> {
  return mutateDoc(accountId, "trips", shapeTrips, fn);
}

export async function loadTrips(accountId: number): Promise<Trip[]> {
  return shapeTrips(await readDoc<unknown>(accountId, "trips", null)).trips;
}

// Un viaje por id. Sin id devuelve el primero (que es como pide el aparato
// cuando todavía no eligió ninguno). Lo usan las rutas de acá y `suggest.ts`.
export async function getTrip(accountId: number, id?: string): Promise<Trip | null> {
  const trips = await loadTrips(accountId);
  if (!id) return trips[0] ?? null;
  return trips.find((t) => t.id === id) ?? null;
}

// El viaje que contiene esa fecha (para las sugerencias del día: si hoy estoy
// de viaje, lo del día sale del viaje y no de la agenda de casa).
export async function tripOnDate(accountId: number, date: string): Promise<Trip | null> {
  const trips = await loadTrips(accountId);
  return trips.find((t) => t.start <= date && date <= t.end) ?? null;
}

function newId(): string {
  return Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36).padStart(2, "0");
}

// ---------------------------------------------------------------- fechas

const DATE_RE = /^\d{4}-\d{2}-\d{2}$/;
const TIME_RE = /^([01]\d|2[0-3]):[0-5]\d$/;

function isDate(s: unknown): s is string {
  if (typeof s !== "string" || !DATE_RE.test(s)) return false;
  const d = new Date(`${s}T00:00:00Z`);
  return !Number.isNaN(d.getTime()) && d.toISOString().slice(0, 10) === s;
}

function addDays(date: string, n: number): string {
  const d = new Date(`${date}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() + n);
  return d.toISOString().slice(0, 10);
}

function daysBetween(a: string, b: string): number {
  return Math.round((Date.parse(`${b}T00:00:00Z`) - Date.parse(`${a}T00:00:00Z`)) / 86_400_000);
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
  // arrastra al final para que el usuario lo vea y decida.
  for (const leftover of old.values()) if (leftover.items.length || leftover.note) out.push(leftover);
  out.sort((a, b) => a.date.localeCompare(b.date));
  trip.days = out;
}

function sortItems(day: TripDay): void {
  day.items.sort((a, b) => (a.at ?? "99:99").localeCompare(b.at ?? "99:99") || a.title.localeCompare(b.title));
}

// ---------------------------------------------------------------- idiomas

// Los tipos de ítem, en los seis idiomas del aparato.
const KIND_LABEL: Record<Lang, Record<ItemKind, string>> = {
  es: { flight: "Vuelo", train: "Tren", hotel: "Hotel", ticket: "Entrada", meal: "Comida", visit: "Visita", other: "Otro" },
  en: { flight: "Flight", train: "Train", hotel: "Hotel", ticket: "Ticket", meal: "Meal", visit: "Visit", other: "Other" },
  fr: { flight: "Vol", train: "Train", hotel: "Hôtel", ticket: "Billet", meal: "Repas", visit: "Visite", other: "Autre" },
  de: { flight: "Flug", train: "Zug", hotel: "Hotel", ticket: "Ticket", meal: "Essen", visit: "Besuch", other: "Sonstiges" },
  pt: { flight: "Voo", train: "Trem", hotel: "Hotel", ticket: "Ingresso", meal: "Refeição", visit: "Visita", other: "Outro" },
  ru: { flight: "Рейс", train: "Поезд", hotel: "Отель", ticket: "Билет", meal: "Еда", visit: "Экскурсия", other: "Другое" },
};

const WORDS: Record<Lang, { packing: string; docs: string; day: string; noTrips: string; copyWarn: string }> = {
  es: { packing: "Para llevar", docs: "Papeles", day: "Día", noTrips: "Sin viajes cargados", copyWarn: "Código copiado: puede no escanear" },
  en: { packing: "Packing", docs: "Documents", day: "Day", noTrips: "No trips yet", copyWarn: "Copied code: it may not scan" },
  fr: { packing: "À emporter", docs: "Documents", day: "Jour", noTrips: "Aucun voyage", copyWarn: "Code copié : peut ne pas scanner" },
  de: { packing: "Packliste", docs: "Unterlagen", day: "Tag", noTrips: "Keine Reisen", copyWarn: "Kopierter Code: scannt evtl. nicht" },
  pt: { packing: "Para levar", docs: "Documentos", day: "Dia", noTrips: "Sem viagens", copyWarn: "Código copiado: pode não escanear" },
  ru: { packing: "Собрать", docs: "Документы", day: "День", noTrips: "Поездок нет", copyWarn: "Копия кода: может не сканироваться" },
};

export function kindLabel(kind: ItemKind, lang: Lang): string {
  return KIND_LABEL[lang][kind] ?? KIND_LABEL.es[kind];
}

// ---------------------------------------------------------------- proyección al calendario

export type TripEvent = {
  id: string;
  start: string;      // "YYYY-MM-DDTHH:MM" con hora, "YYYY-MM-DD" si es de todo el día
  end: string;
  allDay: boolean;
  title: string;
  place?: string;
  note?: string;
  tripId: string;
  tripDay: string;
};

// Los ítems de los días vistos como eventos del calendario. `calendar.ts` los
// suma a los suyos: no se guarda nada duplicado, la fuente sigue siendo el viaje.
// Un ítem con hora dura una hora por default (lo que ocupa en la grilla); uno
// sin hora es de todo el día.
export async function tripCalendarEvents(accountId: number, from?: string, to?: string): Promise<TripEvent[]> {
  const trips = await loadTrips(accountId);
  const out: TripEvent[] = [];
  for (const trip of trips) {
    for (const day of trip.days) {
      if (from && day.date < from) continue;
      if (to && day.date > to) continue;
      for (const item of day.items) {
        const allDay = !item.at;
        const start = allDay ? day.date : `${day.date}T${item.at}`;
        out.push({
          id: `trip:${trip.id}:${item.id}`,
          start,
          end: allDay ? day.date : `${day.date}T${addHour(item.at as string)}`,
          allDay,
          title: item.title,
          place: item.place || trip.place || undefined,
          note: item.note || undefined,
          tripId: trip.id,
          tripDay: day.date,
        });
      }
    }
  }
  out.sort((a, b) => a.start.localeCompare(b.start));
  return out;
}

function addHour(hhmm: string): string {
  const [h, m] = hhmm.split(":").map(Number);
  const t = Math.min(23 * 60 + 59, h * 60 + m + 60);
  return `${String(Math.floor(t / 60)).padStart(2, "0")}:${String(t % 60).padStart(2, "0")}`;
}

// `calendar.ts` lee /data/calendar.json y muestra como "trip" todo evento que
// traiga `tripId`. Para que el viaje se vea ahí sin que existan dos verdades,
// los ítems se ESPEJAN: la fuente sigue siendo trips.json y este espejo se
// rehace entero (borrar los del viaje y volver a escribirlos) en cada cambio.
// Nada se edita a mano en el calendario: si alguien lo toca, el próximo cambio
// del viaje lo vuelve a dejar como está en trips.json.
//
// Se escribe adentro de `serialize(CAL_FILE, ...)`, la misma cola que usa
// calendar.ts: sin eso, dos escrituras al mismo tiempo se pisan y el usuario
// pierde eventos.
type CalRaw = { version?: number; events?: unknown[] };

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
          const old = mine.get(item.id);
          const keep = Math.floor(Number(old?.id));
          const allDay = !item.at;
          const place = item.place || trip.place || "";
          out.push({
            id: Number.isFinite(keep) && keep > 0 ? keep : ++free,
            title: item.title,
            start: allDay ? day.date : `${day.date}T${item.at}`,
            end: allDay ? day.date : `${day.date}T${addHour(item.at as string)}`,
            allDay,
            ...(place ? { place } : {}),
            ...(item.note ? { note: item.note } : {}),
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

// ---------------------------------------------------------------- vistas

// Lo que se le manda al aparato de cada adjunto: nada de texto largo (el heap
// del aparato es chico). Si quiere todo, pide /api/attachment/info.
function slimAttachment(a: Attachment) {
  return {
    id: a.id,
    name: a.name,
    kind: a.kind,
    pages: a.pages,
    fields: a.fields,
    codes: a.codes.map((c) => ({ format: c.format, page: c.page, verified: c.verified, copy: c.copy })),
    warn: a.warn,
  };
}

async function tripView(accountId: number, trip: Trip, lang: Lang) {
  const all = await listAttachments(accountId);
  const byId = new Map(all.map((a) => [a.id, a]));
  const inItems = new Set<string>();
  for (const d of trip.days) for (const i of d.items) for (const id of i.attachmentIds) inItems.add(id);
  // Los papeles del viaje son los que se colgaron a mano y, además, todo lo que
  // se subió con `?trip=` y no quedó colgado de ningún ítem: si falla la llamada
  // que lo cuelga, el archivo igual se ve en vez de quedar invisible.
  const docIds = trip.docs.filter((id) => byId.has(id));
  for (const a of all) if (a.tripId === trip.id && !inItems.has(a.id) && !docIds.includes(a.id)) docIds.push(a.id);
  const w = WORDS[lang];
  return {
    id: trip.id,
    name: trip.name,
    title: trip.name,   // el firmware lee `title`; `name` queda por compatibilidad
    place: trip.place ?? "",
    when: rangeText(trip.start, trip.end, lang),
    start: trip.start,
    end: trip.end,
    labels: { packing: w.packing, docs: w.docs, day: w.day, copyWarn: w.copyWarn },
    days: trip.days.map((d, i) => ({
      date: d.date,
      label: `${shortDate(d.date, lang)} · ${w.day} ${i + 1}`,
      note: d.note ?? "",
      items: d.items.map((i) => ({
        id: i.id,
        at: i.at ?? "",
        title: i.title,
        kind: i.kind,
        kindLabel: kindLabel(i.kind, lang),
        place: i.place ?? "",
        note: i.note ?? "",
        attachmentIds: i.attachmentIds,
        attachments: i.attachmentIds.map((id) => byId.get(id)).filter(Boolean).map((a) => slimAttachment(a as Attachment)),
      })),
    })),
    packing: trip.packing,
    docs: docIds.map((id) => byId.get(id)).filter(Boolean).map((a) => slimAttachment(a as Attachment)),
  };
}

// Fechas cortas para las pantallas ("lun 13 oct"). Se arman en UTC a propósito:
// la fecha del viaje es una fecha civil, no un instante, y pasarla por la zona
// local la corre un día.
function shortDate(date: string, lang: Lang): string {
  try {
    return new Intl.DateTimeFormat(lang, { weekday: "short", day: "numeric", month: "short", timeZone: "UTC" })
      .format(new Date(`${date}T12:00:00Z`));
  } catch {
    return date;
  }
}

function rangeText(start: string, end: string, lang: Lang): string {
  return start === end ? shortDate(start, lang) : `${shortDate(start, lang)} - ${shortDate(end, lang)}`;
}

function today(): string {
  return new Date().toISOString().slice(0, 10);
}

// ---------------------------------------------------------------- rutas

export const tripsApi = new Hono<AppEnv>();   // GET /api/trips
export const tripApi = new Hono<AppEnv>();    // /api/trip*

tripsApi.get("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const trips = await loadTrips(accountOf(c));
  const now = today();
  const rows = trips
    .map((t) => ({
      id: t.id,
      name: t.name,
      title: t.name,                                   // el firmware lee `title`
      when: rangeText(t.start, t.end, lang),           // ...y `when`
      place: t.place ?? "",
      start: t.start,
      end: t.end,
      days: t.days.length,
      items: t.days.reduce((n, d) => n + d.items.length, 0),
      packingLeft: t.packing.filter((p) => !p.done).length,
      state: now < t.start ? "next" : now > t.end ? "past" : "now",
    }))
    .sort((a, b) => a.start.localeCompare(b.start));
  return c.json({ ok: true, today: now, trips: rows, empty: rows.length ? "" : WORDS[lang].noTrips });
});

tripApi.get("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const id = (c.req.query("id") ?? "").toString();
  const trips = await loadTrips(accountOf(c));
  const trip = trips.find((t) => t.id === id) ?? (id ? null : trips[0]);
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, today: today(), trip: await tripView(accountOf(c), trip, lang) });
});

// Crear o editar. Sin `id` crea; con `id` cambia nombre, lugar y fechas y
// rearma los días sin perder lo cargado.
tripApi.post("/", async (c) => {
  const b = await readBody(c);
  const name = (b.name ?? "").toString().trim().slice(0, 80);
  const start = (b.start ?? "").toString();
  const end = ((b.end ?? "").toString() || start);
  if (!name) return c.json({ ok: false, error: "falta el nombre" }, 400);
  if (!isDate(start) || !isDate(end)) return c.json({ ok: false, error: "las fechas van como YYYY-MM-DD" }, 400);
  if (daysBetween(start, end) < 0) return c.json({ ok: false, error: "la vuelta es antes de la ida" }, 400);
  if (daysBetween(start, end) > MAX_DAYS) return c.json({ ok: false, error: `el viaje no puede pasar de ${MAX_DAYS} días` }, 400);

  const res = await update(accountOf(c), (store) => {
    const id = (b.id ?? "").toString();
    let trip = id ? store.trips.find((t) => t.id === id) : undefined;
    if (id && !trip) return { error: "not found" as const };
    if (!trip) {
      if (store.trips.length >= MAX_TRIPS) return { error: "demasiados viajes" as const };
      trip = { id: newId(), name, start, end, days: [], packing: [], docs: [] };
      store.trips.push(trip);
    }
    trip.name = name;
    trip.start = start;
    trip.end = end;
    const place = (b.place ?? "").toString().trim().slice(0, 80);
    if (place) trip.place = place;
    else delete trip.place;
    rebuildDays(trip);
    return { trip };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, res.error === "not found" ? 404 : 400);
  await syncCalendar(accountOf(c), res.trip.id);
  return c.json({ ok: true, id: res.trip.id });
});

tripApi.post("/delete", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const gone = await update(accountOf(c), (store) => {
    const before = store.trips.length;
    store.trips = store.trips.filter((t) => t.id !== id);
    return store.trips.length !== before;
  });
  if (!gone) return c.json({ ok: false, error: "not found" }, 404);
  await syncCalendar(accountOf(c), id);  // el viaje ya no está: esto le saca los eventos al calendario
  // Y las sugerencias que se generaron para él, que viven aparte en suggest.json
  // con la clave `trip:<id>:…`: si no se borran acá, el viaje sigue existiendo
  // para el aparato aunque no esté en ninguna lista.
  const olvidadas = await forgetTrip(accountOf(c), id);
  // Los bitmaps de los adjuntos son 96 KB por página: si no se borran acá,
  // quedan ocupando el volumen sin que nadie los pueda ver nunca más.
  const dropped = await deleteAttachmentsOfTrip(accountOf(c), id);
  return c.json({ ok: true, attachments: dropped, suggestions: olvidadas });
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
    const note = (b.note ?? "").toString().trim().slice(0, 300);
    if (note) day.note = note;
    else delete day.note;
    return true;
  });
  return res ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
});

// Un ítem del día: hora, título, tipo, lugar, nota y adjuntos. Sin `id` es
// nuevo; con `id` se edita el que está.
tripApi.post("/day/item", async (c) => {
  const b = await readBody(c);
  const date = (b.date ?? "").toString();
  const title = (b.title ?? "").toString().trim().slice(0, 120);
  if (!isDate(date)) return c.json({ ok: false, error: "fecha inválida" }, 400);
  if (!title) return c.json({ ok: false, error: "falta el título" }, 400);
  const at = (b.at ?? "").toString().trim();
  if (at && !TIME_RE.test(at)) return c.json({ ok: false, error: "la hora va como HH:MM" }, 400);
  const kind: ItemKind = KINDS.includes(b.kind) ? b.kind : "other";

  const res = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return { error: "not found" as const };
    let day = trip.days.find((d) => d.date === date);
    if (!day) {
      // Una fecha fuera del rango (el vuelo de vuelta que cae un día después):
      // se agrega el día en vez de rechazarlo.
      day = { date, items: [] };
      trip.days.push(day);
      trip.days.sort((x, y) => x.date.localeCompare(y.date));
    }
    const id = (b.id ?? "").toString();
    let item = id ? day.items.find((i) => i.id === id) : undefined;
    if (id && !item) return { error: "not found" as const };
    if (!item) {
      item = { id: newId(), title, kind, attachmentIds: [] };
      day.items.push(item);
    }
    item.title = title;
    item.kind = kind;
    if (at) item.at = at;
    else delete item.at;
    const place = (b.place ?? "").toString().trim().slice(0, 100);
    if (place) item.place = place;
    else delete item.place;
    const note = (b.note ?? "").toString().trim().slice(0, 400);
    if (note) item.note = note;
    else delete item.note;
    if (Array.isArray(b.attachmentIds)) {
      item.attachmentIds = b.attachmentIds.map((x: unknown) => String(x).replace(/[^a-z0-9]/gi, "")).filter(Boolean).slice(0, 10);
    }
    sortItems(day);
    return { id: item.id };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, 404);
  await syncCalendar(accountOf(c), (b.tripId ?? "").toString());
  return c.json({ ok: true, id: res.id });
});

tripApi.post("/day/item/delete", async (c) => {
  const b = await readBody(c);
  const date = (b.date ?? "").toString();
  const id = (b.id ?? "").toString();
  const gone = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    const day = trip?.days.find((d) => d.date === date);
    if (!day) return false;
    const before = day.items.length;
    day.items = day.items.filter((i) => i.id !== id);
    return day.items.length !== before;
  });
  if (gone) await syncCalendar(accountOf(c), (b.tripId ?? "").toString());
  return gone ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
});

// Lista de cosas para llevar: alta, tilde y baja en el mismo endpoint (es lo
// que el aparato hace con OK sobre la fila).
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
      const text = (b.text ?? "").toString().trim().slice(0, 120);
      if (!text) return { error: "falta el texto" as const };
      if (trip.packing.length >= 200) return { error: "la lista está llena" as const };
      item = { id: newId(), text, done: false };
      trip.packing.push(item);
    } else {
      const text = (b.text ?? "").toString().trim().slice(0, 120);
      if (text) item.text = text;
      if (typeof b.done === "boolean") item.done = b.done;
      else if (b.done === undefined && text === "") item.done = !item.done;  // OK en el aparato = tildar
    }
    return { id: item.id, done: item.done };
  });
  if ("error" in res) return c.json({ ok: false, error: res.error }, res.error === "not found" ? 404 : 400);
  return c.json({ ok: true, ...res });
});

// Colgar un adjunto ya subido de un ítem (o de los papeles del viaje) y
// descolgarlo. El archivo en sí se sube por POST /api/board/attachment.
tripApi.post("/attach", async (c) => {
  const b = await readBody(c);
  const attachmentId = (b.attachmentId ?? "").toString().replace(/[^a-z0-9]/gi, "");
  const remove = b.action === "remove";
  if (!attachmentId) return c.json({ ok: false, error: "falta el adjunto" }, 400);
  const res = await update(accountOf(c), (store) => {
    const trip = store.trips.find((t) => t.id === (b.tripId ?? "").toString());
    if (!trip) return false;
    const date = (b.date ?? "").toString();
    const itemId = (b.itemId ?? "").toString();
    const target = itemId ? trip.days.find((d) => d.date === date)?.items.find((i) => i.id === itemId) : null;
    if (itemId && !target) return false;
    const list = target ? target.attachmentIds : trip.docs;
    const at = list.indexOf(attachmentId);
    if (remove) {
      if (at >= 0) list.splice(at, 1);
    } else if (at < 0) {
      if (list.length >= 10) return false;
      list.push(attachmentId);
    }
    return true;
  });
  return res ? c.json({ ok: true }) : c.json({ ok: false, error: "not found" }, 404);
});

// Reparación a mano: vuelve a escribir el espejo del viaje en el calendario
// (por si alguien editó calendar.json por afuera o falló un guardado).
tripApi.post("/sync", async (c) => {
  const b = await readBody(c);
  const id = (b.id ?? "").toString();
  const ids = id ? [id] : (await loadTrips(accountOf(c))).map((t) => t.id);
  let n = 0;
  for (const t of ids) n += await syncCalendar(accountOf(c), t);
  return c.json({ ok: true, events: n });
});
