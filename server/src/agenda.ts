// Agenda de hoy para el hub, desde un calendario en formato ICS (Google
// Calendar "dirección secreta en formato iCal", Apple, Outlook, Nextcloud...).
//
//   HUB_ICS_URL   una o varias URLs separadas por coma. Sin esto, la agenda
//                 sale de /data/hub-data.json (hub.ts) como hasta ahora.
//
// Parser mínimo y suficiente: desdobla líneas, toma VEVENT con DTSTART/DTEND/
// SUMMARY, entiende fechas UTC (Z), con TZID (se asumen en HUB_TZ, que es donde
// está el aparato) y de día entero. Repeticiones (RRULE) diarias y semanales se
// expanden dentro de la ventana; el resto se ignora. Caché de 30 minutos.
import { LABELS, type Lang } from "./lang";

const URLS = (process.env.HUB_ICS_URL ?? "").split(",").map((s) => s.trim()).filter(Boolean);
const TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";
const TTL_MS = 30 * 60 * 1000;

export type Event = { start: number; end: number; allDay: boolean; title: string };

let cache: { at: number; events: Event[] } | null = null;

export function agendaConfigured(): boolean {
  return URLS.length > 0;
}

function tzOffsetMs(at: number): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TZ, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  return Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second")) - Math.floor(at / 1000) * 1000;
}

// "20260907T103000Z" | "20260907T103000" (local HUB_TZ) | "20260907" (día entero)
function parseStamp(value: string): { at: number; allDay: boolean } | null {
  const m = /^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})?(Z)?)?$/.exec(value.trim());
  if (!m) return null;
  const [y, mo, d] = [Number(m[1]), Number(m[2]), Number(m[3])];
  if (!m[4]) {
    const guess = Date.UTC(y, mo - 1, d);
    return { at: guess - tzOffsetMs(guess), allDay: true };
  }
  const guess = Date.UTC(y, mo - 1, d, Number(m[4]), Number(m[5]), Number(m[6] ?? 0));
  if (m[7]) return { at: guess, allDay: false };
  const first = guess - tzOffsetMs(guess);
  return { at: guess - tzOffsetMs(first), allDay: false };
}

function unfold(text: string): string[] {
  return text.replace(/\r\n?/g, "\n").replace(/\n[ \t]/g, "").split("\n");
}

function parseIcs(text: string, windowStart: number, windowEnd: number): Event[] {
  const out: Event[] = [];
  let cur: Record<string, string> | null = null;
  for (const line of unfold(text)) {
    if (line === "BEGIN:VEVENT") { cur = {}; continue; }
    if (line === "END:VEVENT") {
      if (cur) pushEvent(cur, out, windowStart, windowEnd);
      cur = null;
      continue;
    }
    if (!cur) continue;
    const idx = line.indexOf(":");
    if (idx < 0) continue;
    const key = line.slice(0, idx).split(";")[0].toUpperCase();
    cur[key] = line.slice(idx + 1);
  }
  return out;
}

function pushEvent(v: Record<string, string>, out: Event[], windowStart: number, windowEnd: number) {
  const start = parseStamp(v.DTSTART ?? "");
  if (!start) return;
  const end = parseStamp(v.DTEND ?? "") ?? { at: start.at + (start.allDay ? 86_400_000 : 3_600_000), allDay: start.allDay };
  const title = (v.SUMMARY ?? "").replace(/\\,/g, ",").replace(/\\n/g, " ").trim() || "(sin título)";
  const duration = Math.max(end.at - start.at, 0);
  const rule = (v.RRULE ?? "").toUpperCase();
  const freq = /FREQ=DAILY/.test(rule) ? 86_400_000 : /FREQ=WEEKLY/.test(rule) ? 7 * 86_400_000 : 0;
  const until = /UNTIL=(\d{8}(?:T\d{6}Z?)?)/.exec(rule)?.[1];
  const untilAt = until ? parseStamp(until)?.at ?? Infinity : Infinity;
  if (!freq) {
    if (start.at < windowEnd && end.at > windowStart) out.push({ start: start.at, end: end.at, allDay: start.allDay, title });
    return;
  }
  // Expand the repeat only inside the window (a few days): cheap and enough.
  let at = start.at;
  if (at < windowStart) at += Math.floor((windowStart - at) / freq) * freq;
  for (let i = 0; i < 20 && at < windowEnd && at <= untilAt; i++, at += freq) {
    if (at + duration > windowStart) out.push({ start: at, end: at + duration, allDay: start.allDay, title });
  }
}

export async function loadEvents(): Promise<Event[]> {
  if (!URLS.length) return [];
  if (cache && Date.now() - cache.at < TTL_MS) return cache.events;
  const now = Date.now();
  const windowStart = now - 86_400_000;
  const windowEnd = now + 8 * 86_400_000;
  const events: Event[] = [];
  for (const url of URLS) {
    try {
      const res = await fetch(url, { headers: { "User-Agent": "ws397-hub" } });
      if (!res.ok) throw new Error(`ics ${res.status}`);
      events.push(...parseIcs(await res.text(), windowStart, windowEnd));
    } catch (err) {
      console.error("agenda:", url.slice(0, 40), err);
    }
  }
  events.sort((a, b) => a.start - b.start);
  cache = { at: now, events };
  return events;
}

// Eventos de hoy (y, si hoy no queda nada, los de mañana) como {when, title}.
export async function todayForHub(lang: Lang, max = 4): Promise<{ when: string; title: string }[]> {
  const events = await loadEvents();
  const now = Date.now();
  const localNow = new Date(now + tzOffsetMs(now));
  const dayStart = Date.UTC(localNow.getUTCFullYear(), localNow.getUTCMonth(), localNow.getUTCDate()) - tzOffsetMs(now);
  const dayEnd = dayStart + 86_400_000;
  const fmt = (e: Event, prefix: string) => {
    if (e.allDay) return { when: prefix, title: e.title };
    const local = new Date(e.start + tzOffsetMs(e.start));
    const hh = String(local.getUTCHours()).padStart(2, "0");
    const mm = String(local.getUTCMinutes()).padStart(2, "0");
    return { when: prefix ? `${prefix} ${hh}:${mm}` : `${hh}:${mm}`, title: e.title };
  };
  const today = events.filter((e) => e.start < dayEnd && e.end > dayStart && e.end > now);
  if (today.length) return today.slice(0, max).map((e) => fmt(e, ""));
  const tomorrow = events.filter((e) => e.start >= dayEnd && e.start < dayEnd + 86_400_000);
  return tomorrow.slice(0, max).map((e) => fmt(e, LABELS[lang].tomorrow));
}
