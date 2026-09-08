// Calendario local del aparato. SIN Google, sin ICS y sin cuentas de nadie:
// todo vive en el volumen de Railway (/data/calendar.json) y se edita desde el
// aparato o desde /board.
//
//   GET  /api/calendar?from=&to=&lang=      eventos del rango con las repeticiones
//                                           YA EXPANDIDAS + resumen por día
//   GET  /api/calendar/day?date=&lang=      el día completo
//   POST /api/calendar/event                alta, o edición si trae id
//   POST /api/calendar/event/delete         { id }
//   GET  /api/calendar/repeat?...           la repetición en una línea (lo usa /board
//                                           para mostrarla mientras se edita)
//
// Qué sale en el calendario:
//   1. los eventos de /data/calendar.json (los que se cargan acá),
//   2. los RECORDATORIOS de store.json (se leen, NO se copian: el dueño sigue
//      siendo store.ts, y tildarlos sigue yendo por POST /api/hub/done),
//   3. los ítems de viaje que otro módulo escriba en calendar.json con tripId.
//
// ── Zona horaria ────────────────────────────────────────────────────────────
// La mitad de los bugs de calendario salen de mezclar fechas con instantes.
// Acá:
//   - un evento se guarda con la fecha y la hora LOCALES ("2026-09-15T10:30"),
//     nunca en epoch, y uno de todo el día se guarda como fecha sola
//     ("2026-09-15"): un evento de todo el día no es un instante y pasarlo a
//     epoch "a lo bruto" lo corre un día para atrás o para adelante;
//   - toda la cuenta de repeticiones se hace sobre fechas civiles (store.ts:
//     expandRepeat / nextOccurrence), así el horario de verano no corre nada;
//   - el epoch se calcula recién al final (`localToEpoch`), con la zona del
//     LUGAR guardado en hub-settings.json (el mismo del clima) y HUB_TZ de
//     respaldo — `refreshTimeZone()` la relee antes de cada pedido.
//   - `end` de un evento de todo el día es el ÚLTIMO día INCLUIDO (no el
//     siguiente, como en ICS): es lo que espera cualquiera que lo lea.
import { Hono } from "hono";
import { readJsonSafe, serialize, writeAtomicNow } from "./fsjson";
import { readBody } from "./net";
import { normalizeLang, type Lang } from "./lang";
import {
  addDays, alignToRepeat, diffDays, endOfLocalDay, epochToLocal, expandRepeat, isDateStr, load as loadStore,
  localToEpoch, normalizeRepeat, refreshTimeZone, repeatText, startOfLocalDay, timeZone, todayLocal, type Repeat,
} from "./store";

const FILE = process.env.CALENDAR_FILE ?? "/data/calendar.json";
const MAX_OCCURRENCES = 500;

export type CalEvent = {
  id: number;
  start: string;        // "YYYY-MM-DD" (todo el día) o "YYYY-MM-DDTHH:MM"
  end: string;          // idem; en todo el día es el último día incluido
  allDay: boolean;
  title: string;
  place?: string;
  note?: string;
  repeat?: Repeat;
  tripId?: string;      // lo escribe el módulo de viajes
  tripDay?: number;
  [extra: string]: unknown;  // lo que agregue otro módulo se conserva tal cual
};

export type CalendarFile = { version: 1; events: CalEvent[] };

// ── Archivo ─────────────────────────────────────────────────────────────────

function localStamp(v: unknown, fallback: string): string {
  // Se acepta lo que ya esté guardado: fecha local, fecha con hora, o un epoch
  // (en segundos o en milisegundos) por si otro módulo lo escribió así.
  if (typeof v === "string") {
    const m = /^(\d{4}-\d{2}-\d{2})(?:[T ](\d{2}):(\d{2}))?/.exec(v.trim());
    if (m) return m[2] ? `${m[1]}T${m[2]}:${m[3]}` : m[1];
  }
  if (typeof v === "number" && Number.isFinite(v) && v > 0) {
    // Epoch en segundos o en milisegundos -> fecha y hora locales.
    return epochToLocal(v > 1e11 ? Math.floor(v / 1000) : Math.floor(v));
  }
  return fallback;
}

// Un evento suelto no puede tirar abajo el calendario entero: lo que no tenga
// forma se descarta y lo demás se conserva (incluidos los campos que agregue
// otro módulo, como los del viaje).
function normalizeEvent(raw: unknown, fallbackId: () => number): CalEvent | null {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
  const r = { ...(raw as Record<string, unknown>) };
  const title = String(r.title ?? "").trim().slice(0, 200);
  const start = localStamp(r.start, "");
  if (!title || !isDateStr(start.slice(0, 10))) return null;
  const allDay = r.allDay === true || start.length === 10;
  const end = localStamp(r.end, "") || start;
  const idNum = Math.floor(Number(r.id));
  const id = Number.isFinite(idNum) && idNum > 0 ? idNum : fallbackId();
  const repeat = normalizeRepeat(r.repeat);
  delete r.id; delete r.title; delete r.start; delete r.end; delete r.allDay;
  delete r.place; delete r.note; delete r.repeat;
  const ev: CalEvent = {
    ...r,
    id,
    title,
    start: allDay ? start.slice(0, 10) : start,
    end: allDay ? end.slice(0, 10) : end,
    allDay,
    ...(typeof (raw as any).place === "string" && (raw as any).place ? { place: String((raw as any).place).slice(0, 120) } : {}),
    ...(typeof (raw as any).note === "string" && (raw as any).note ? { note: String((raw as any).note).slice(0, 1000) } : {}),
    ...(repeat.kind === "none" ? {} : { repeat }),
  };
  if (ev.end < ev.start) ev.end = ev.start;
  return ev;
}

function normalizeCalendar(raw: unknown): CalendarFile {
  const r = (raw && typeof raw === "object" && !Array.isArray(raw) ? raw : {}) as Record<string, unknown>;
  const list = Array.isArray(r.events) ? r.events : [];
  const used = new Set<number>();
  for (const e of list) {
    const n = Math.floor(Number((e as Record<string, unknown> | null)?.id));
    if (Number.isFinite(n) && n > 0) used.add(n);
  }
  let free = (used.size ? Math.max(...used) : 0) + 1;
  const fallbackId = () => {
    while (used.has(free)) free++;
    used.add(free);
    return free++;
  };
  const events: CalEvent[] = [];
  for (const e of list) {
    const ev = normalizeEvent(e, fallbackId);
    if (ev) events.push(ev);
  }
  events.sort((a, b) => a.start.localeCompare(b.start));
  return { version: 1, events };
}

export async function loadCalendar(): Promise<CalendarFile> {
  return normalizeCalendar(await readJsonSafe<unknown>(FILE, null));
}

// Leer-modificar-escribir dentro de la MISMA cola de fsjson que usa
// writeJsonAtomic: si otro módulo (viajes) escribe el archivo al mismo tiempo,
// las dos escrituras se ordenan en vez de pisarse. Nunca se cachea el archivo
// en memoria, justamente porque no somos los únicos que lo escriben.
async function mutate<T>(fn: (cal: CalendarFile) => T): Promise<T> {
  return serialize(FILE, async () => {
    const cal = normalizeCalendar(await readJsonSafe<unknown>(FILE, null));
    const out = fn(cal);
    cal.events.sort((a, b) => a.start.localeCompare(b.start));
    await writeAtomicNow(FILE, JSON.stringify(cal, null, 2));
    return out;
  });
}

function nextEventId(cal: CalendarFile): number {
  return cal.events.reduce((max, e) => Math.max(max, e.id), 0) + 1;
}

// ── Expansión ───────────────────────────────────────────────────────────────

export type Occurrence = {
  key: string;           // único por ocurrencia: "ev-12@2026-09-15"
  id: number;
  kind: "event" | "reminder" | "trip";
  date: string;          // día local "YYYY-MM-DD"
  startAt: string;       // arranque de la SERIE tal como está guardado ("2026-09-15T10:30")
  endAt: string;         // fin de la serie, igual formato
  time: string;          // "HH:MM" o "" si es de todo el día
  endTime: string;
  allDay: boolean;
  days: number;          // cuántos días dura en total (1 = un solo día)
  dayIndex: number;      // qué día de esos es este (1 = el primero)
  title: string;
  place: string;
  note: string;
  repeat: Repeat;        // la repetición cruda: con esto se edita la serie
  repeatText: string;
  start: number;         // epoch UTC en segundos (00:00 local si es de todo el día)
  end: number;
  tripId?: string;
  tripDay?: number;
};

function timeOf(stamp: string): string {
  return stamp.length > 10 ? stamp.slice(11, 16) : "";
}

function push(out: Occurrence[], o: Occurrence): boolean {
  if (out.length >= MAX_OCCURRENCES) return false;
  out.push(o);
  return true;
}

// Un evento (con su repetición y su duración) -> una entrada por día visible.
function expandEvent(ev: CalEvent, from: string, to: string, lang: Lang, out: Occurrence[]): boolean {
  const startDate = ev.start.slice(0, 10);
  const endDate = ev.end.slice(0, 10);
  const span = Math.max(0, diffDays(endDate, startDate));  // días extra que dura
  const time = timeOf(ev.start);
  const endTime = timeOf(ev.end);
  const rep = normalizeRepeat(ev.repeat);
  const text = repeatText(rep, ev.start, lang);
  const kind: Occurrence["kind"] = ev.tripId ? "trip" : "event";
  // Se busca desde antes del rango: un evento de cinco días que arrancó el mes
  // pasado tiene que seguir apareciendo en los días que caen adentro.
  const dates = expandRepeat(startDate, rep, addDays(from, -span), to, MAX_OCCURRENCES);
  for (const base of dates) {
    for (let i = 0; i <= span; i++) {
      const date = addDays(base, i);
      if (date < from || date > to) continue;
      const ok = push(out, {
        key: `ev-${ev.id}@${date}`,
        id: ev.id,
        kind,
        date,
        startAt: ev.start,
        endAt: ev.end,
        time: ev.allDay ? "" : i === 0 ? time : "",
        endTime: ev.allDay ? "" : i === span ? endTime : "",
        allDay: ev.allDay,
        days: span + 1,
        dayIndex: i + 1,
        title: ev.title,
        place: typeof ev.place === "string" ? ev.place : "",
        note: typeof ev.note === "string" ? ev.note : "",
        repeat: rep,
        repeatText: text,
        start: ev.allDay ? startOfLocalDay(date) : localToEpoch(date + (time ? "T" + time : "")),
        end: ev.allDay ? endOfLocalDay(date) : localToEpoch(addDays(base, span) + (endTime ? "T" + endTime : "T23:59")),
        ...(ev.tripId ? { tripId: String(ev.tripId) } : {}),
        ...(Number.isFinite(Number(ev.tripDay)) ? { tripDay: Number(ev.tripDay) } : {}),
      });
      if (!ok) return false;
    }
  }
  return true;
}

// Los recordatorios de store.ts se PROYECTAN acá: se leen, no se copian.
async function expandReminders(from: string, to: string, lang: Lang, out: Occurrence[]): Promise<void> {
  const store = await loadStore();
  for (const r of store.reminders) {
    if (r.done || !r.dueAt) continue;
    const date = r.dueAt.slice(0, 10);
    if (!isDateStr(date)) continue;
    const time = timeOf(r.dueAt);
    const rep = normalizeRepeat(r.repeat);
    const text = repeatText(rep, r.dueAt, lang);
    for (const d of expandRepeat(date, rep, from, to, 200)) {
      const ok = push(out, {
        key: `rem-${r.id}@${d}`,
        id: r.id,
        kind: "reminder",
        date: d,
        startAt: r.dueAt,
        endAt: r.dueAt,
        time,
        endTime: "",
        allDay: !time,
        days: 1,
        dayIndex: 1,
        title: r.title,
        place: "",
        note: "",
        repeat: rep,
        repeatText: text,
        start: localToEpoch(d + (time ? "T" + time : "")),
        end: localToEpoch(d + (time ? "T" + time : "")),
      });
      if (!ok) return;
    }
  }
}

// Todo lo que cae en [from, to], ordenado por día y hora.
export async function occurrencesBetween(from: string, to: string, lang: Lang): Promise<{ items: Occurrence[]; truncated: boolean }> {
  const cal = await loadCalendar();
  const out: Occurrence[] = [];
  let full = false;
  for (const ev of cal.events) {
    if (!expandEvent(ev, from, to, lang, out)) { full = true; break; }
  }
  if (!full) await expandReminders(from, to, lang, out);
  out.sort((a, b) =>
    a.date.localeCompare(b.date) ||
    (a.allDay === b.allDay ? (a.time || "").localeCompare(b.time || "") : a.allDay ? -1 : 1) ||
    a.title.localeCompare(b.title));
  return { items: out, truncated: full || out.length >= MAX_OCCURRENCES };
}

// Resumen por día para pintar el mes sin bajar todo.
export function daySummary(items: Occurrence[]): { date: string; count: number; firstTitle: string }[] {
  const byDay = new Map<string, { date: string; count: number; firstTitle: string }>();
  for (const o of items) {
    const d = byDay.get(o.date);
    if (d) d.count++;
    else byDay.set(o.date, { date: o.date, count: 1, firstTitle: o.title });
  }
  return [...byDay.values()].sort((a, b) => a.date.localeCompare(b.date));
}

// ── Rutas ───────────────────────────────────────────────────────────────────

export const calendar = new Hono();

function rangeOf(fromRaw: string, toRaw: string): { from: string; to: string } {
  // Sin rango: el mes de hoy. Tope de 400 días para que nadie pida diez años.
  const today = todayLocal();
  const from = isDateStr(fromRaw) ? fromRaw : today.slice(0, 8) + "01";
  // Sin `to`: hasta el último día del mes de `from` (el 1 del mes que viene, menos un día).
  let to = isDateStr(toRaw) ? toRaw : addDays(addDays(from.slice(0, 8) + "01", 32).slice(0, 8) + "01", -1);
  if (to < from) to = from;
  if (diffDays(to, from) > 400) to = addDays(from, 400);
  return { from, to };
}

calendar.get("/", async (c) => {
  await refreshTimeZone();
  const lang = normalizeLang(c.req.query("lang"));
  const { from, to } = rangeOf(c.req.query("from") ?? "", c.req.query("to") ?? "");
  const { items, truncated } = await occurrencesBetween(from, to, lang);
  const summary = daySummary(items);
  // Un mes sin nada igual lleva un día con count 0: el aparato saca de ahí de
  // qué mes es la respuesta cuando pide sin rango (sin reloj no sabe la fecha),
  // y un count 0 no pinta nada en la grilla.
  if (!summary.length) summary.push({ date: from, count: 0, firstTitle: "" });
  // ?summary=1: solo el resumen por día (lo que necesita el aparato para
  // pintar la grilla del mes).
  const onlySummary = c.req.query("summary") === "1";
  return c.json({
    ok: true, from, to, tz: timeZone(), today: todayLocal(),
    count: items.length, truncated,
    days: summary,
    // El firmware (CalendarActivity) lee `days` para pintar el mes y `events`
    // para poder mirarlo sin WiFi; el nombre del arreglo es el que espera él.
    events: onlySummary ? [] : items,
  });
});

calendar.get("/day", async (c) => {
  await refreshTimeZone();
  const lang = normalizeLang(c.req.query("lang"));
  const date = isDateStr(c.req.query("date") ?? "") ? (c.req.query("date") as string) : todayLocal();
  const { items } = await occurrencesBetween(date, date, lang);
  return c.json({ ok: true, date, tz: timeZone(), count: items.length, items });
});

// Alta y edición. Acepta {date, time, endTime} (lo cómodo para un formulario) o
// {start, end} ya armados.
calendar.post("/event", async (c) => {
  await refreshTimeZone();
  const lang = normalizeLang(c.req.query("lang"));
  const b = await readBody(c);
  const title = (b.title ?? "").toString().trim().slice(0, 200);
  if (!title) return c.json({ ok: false, error: "title required" }, 400);
  const allDay = b.allDay === true || b.allDay === "true";
  const date = isDateStr(b.date) ? (b.date as string) : (b.start ?? "").toString().slice(0, 10);
  if (!isDateStr(date)) return c.json({ ok: false, error: "date required (YYYY-MM-DD)" }, 400);
  const time = /^\d{2}:\d{2}$/.test(String(b.time ?? "")) ? String(b.time) : (b.start ?? "").toString().slice(11, 16);
  const endDate = isDateStr(b.endDate) ? (b.endDate as string) : (b.end ?? "").toString().slice(0, 10) || date;
  const endTime = /^\d{2}:\d{2}$/.test(String(b.endTime ?? "")) ? String(b.endTime) : (b.end ?? "").toString().slice(11, 16);
  const repeat = normalizeRepeat(b.repeat);
  const timed = !allDay && /^\d{2}:\d{2}$/.test(time);
  // Con repetición semanal el evento arranca el primer día que corresponde.
  const first = alignToRepeat(date, repeat);
  const span = Math.max(0, diffDays(endDate, date));
  const ev = await mutate((cal) => {
    const id = Math.floor(Number(b.id));
    const existing = Number.isFinite(id) && id > 0 ? cal.events.find((e) => e.id === id) : undefined;
    if (Number.isFinite(id) && id > 0 && !existing) return null;
    const base: CalEvent = existing ?? { id: nextEventId(cal), title, start: first, end: first, allDay: !timed };
    base.title = title;
    base.allDay = !timed;
    base.start = timed ? `${first}T${time}` : first;
    base.end = timed
      ? `${addDays(first, span)}T${/^\d{2}:\d{2}$/.test(endTime) ? endTime : time}`
      : addDays(first, span);
    if (base.end < base.start) base.end = base.start;
    if (repeat.kind === "none") delete base.repeat;
    else base.repeat = repeat;
    const place = (b.place ?? "").toString().trim().slice(0, 120);
    const note = (b.note ?? "").toString().trim().slice(0, 1000);
    if (place) base.place = place; else delete base.place;
    if (note) base.note = note; else delete base.note;
    if (!existing) cal.events.push(base);
    return base;
  });
  if (!ev) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, event: ev, repeatText: repeatText(ev.repeat, ev.start, lang) });
});

calendar.post("/event/delete", async (c) => {
  const b = await readBody(c);
  const id = Math.floor(Number(b.id));
  if (!Number.isFinite(id) || id <= 0) return c.json({ ok: false, error: "id required" }, 400);
  const found = await mutate((cal) => {
    const before = cal.events.length;
    cal.events = cal.events.filter((e) => e.id !== id);
    return cal.events.length !== before;
  });
  return c.json({ ok: true, found });
});

// La repetición en una línea, para mostrarla mientras se edita.
//   GET /api/calendar/repeat?kind=weekly&days=2,4&interval=1&until=2026-12-31&date=2026-09-08&lang=es
calendar.get("/repeat", async (c) => {
  await refreshTimeZone();
  const lang = normalizeLang(c.req.query("lang"));
  const daysRaw = (c.req.query("days") ?? "").split(",").map((d) => Number(d)).filter((d) => Number.isInteger(d));
  const untilRaw = c.req.query("until") ?? "";
  const repeat = normalizeRepeat({
    kind: c.req.query("kind") ?? "none",
    days: daysRaw,
    interval: Number(c.req.query("interval") ?? 1),
    until: isDateStr(untilRaw) ? localToEpoch(untilRaw + "T23:59") : 0,
  });
  const date = isDateStr(c.req.query("date") ?? "") ? (c.req.query("date") as string) : todayLocal();
  return c.json({ ok: true, repeat, text: repeatText(repeat, date, lang), first: alignToRepeat(date, repeat) });
});
