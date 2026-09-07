// Datos del hub del aparato en una sola llamada. El lector la hace al arrancar
// cuando la caché tiene más de unas horas, al mantener Atrás en el hub y desde
// Settings -> Sincronizar hub; guarda la respuesta en la SD y la muestra sin
// WiFi hasta la próxima.
//
//   GET /api/hub   (Bearer del aparato, lo chequea api.ts)
//   200: {
//     ok: true,
//     now: <epoch UTC en segundos>,          // el aparato pone en hora el RTC con esto
//     weather: { line, detail },             // "Nublado · 18°" / "Máx 22° · Mín 11° · Humedad 60 %"
//     reminders: [{ title, when }],          // el primero es el próximo
//     events: [{ when, title }],             // agenda de hoy (máx. 4)
//     messages: [{ from, text }],            // pizarra (máx. 5)
//     quote: string
//   }
//
//   GET /api/hub/location/search?q=<texto dicho>   -> { ok, results: [{ name, label, lat, lon, timezone }] }
//   POST /api/hub/location { name, label, lat, lon, timezone } -> { ok }
//     El aparato elige el lugar por voz (Settings -> Lugar del clima): transcribe,
//     busca acá (geocoding de Open-Meteo), el usuario elige de la lista y se guarda
//     en /data/hub-settings.json. Mientras no haya lugar guardado se usa el env.
//
// Clima: Open-Meteo, gratis y sin key. Env de respaldo en Railway:
//   HUB_LAT, HUB_LON   (si no hay lugar guardado; sin nada, weather queda vacío)
//   HUB_TZ             default "America/Argentina/Buenos_Aires"
// Recordatorios, agenda y mensajes: por ahora se leen de /data/hub-data.json
// (volumen de Railway) con la misma forma que la respuesta; la Fase 2 los
// reemplaza por las tablas de verdad. Si el archivo no existe, van vacíos.
import { Hono } from "hono";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import { hubSlice, markDone, editEntry } from "./voice";
import { QUOTES, LABELS, describeWeather, normalizeLang, type Lang } from "./lang";
import { load as loadStore, DEFAULT_SETTINGS } from "./store";
import { agendaConfigured, todayForHub } from "./agenda";
import { verseOfTheDay } from "./bible";

const LAT = process.env.HUB_LAT ?? "";
const LON = process.env.HUB_LON ?? "";
const TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";
const DATA_FILE = process.env.HUB_DATA_FILE ?? "/data/hub-data.json";
const SETTINGS_FILE = process.env.HUB_SETTINGS_FILE ?? "/data/hub-settings.json";
const WEATHER_TTL_MS = 15 * 60 * 1000;

type Place = { name: string; label: string; lat: number; lon: number; timezone: string };

// Desfase (ms) de la zona del lugar respecto de UTC, para ubicar "ahora" en
// las horas del pronóstico.
function tzOffsetMs(at: number): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TZ, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  return Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second")) - Math.floor(at / 1000) * 1000;
}

let placeCache: Place | null | undefined; // undefined = not loaded yet

async function place(): Promise<Place | null> {
  if (placeCache !== undefined) return placeCache;
  try {
    placeCache = JSON.parse(await readFile(SETTINGS_FILE, "utf8")) as Place;
  } catch {
    placeCache = LAT && LON ? { name: "", label: "", lat: Number(LAT), lon: Number(LON), timezone: TZ } : null;
  }
  return placeCache;
}

async function savePlace(p: Place): Promise<void> {
  await mkdir(dirname(SETTINGS_FILE), { recursive: true });
  await writeFile(SETTINGS_FILE, JSON.stringify(p, null, 2));
  placeCache = p;
  weatherCache = null;
}

let weatherCache: { at: number; lang: Lang; value: { line: string; detail: string } } | null = null;

async function weather(lang: Lang): Promise<{ line: string; detail: string }> {
  const p = await place();
  if (!p) return { line: "", detail: "" };
  if (weatherCache && weatherCache.lang === lang && Date.now() - weatherCache.at < WEATHER_TTL_MS) return weatherCache.value;
  const url =
    `https://api.open-meteo.com/v1/forecast?latitude=${p.lat}&longitude=${p.lon}` +
    `&current=temperature_2m,relative_humidity_2m,weather_code` +
    `&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=${encodeURIComponent(p.timezone || TZ)}`;
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`open-meteo ${res.status}`);
    const data = (await res.json()) as {
      current: { temperature_2m: number; relative_humidity_2m: number; weather_code: number };
      daily: { temperature_2m_max: number[]; temperature_2m_min: number[] };
    };
    const l = LABELS[lang];
    const value = {
      line: `${describeWeather(data.current.weather_code, lang)} · ${Math.round(data.current.temperature_2m)}°`,
      detail:
        (p.name ? `${p.name} · ` : "") +
        `${l.max} ${Math.round(data.daily.temperature_2m_max[0])}° · ${l.min} ${Math.round(data.daily.temperature_2m_min[0])}°` +
        ` · ${l.hum} ${Math.round(data.current.relative_humidity_2m)} %`,
    };
    weatherCache = { at: Date.now(), lang, value };
    return value;
  } catch (err) {
    console.error("hub weather:", err);
    return weatherCache?.value ?? { line: "", detail: "" };
  }
}

type HubData = {
  reminders?: { title: string; when: string }[];
  events?: { when: string; title: string }[];
  messages?: { from: string; text: string }[];
  quote?: string;
};

async function data(): Promise<HubData> {
  try {
    return JSON.parse(await readFile(DATA_FILE, "utf8")) as HubData;
  } catch {
    return {};
  }
}

function quoteOfTheDay(lang: Lang): string {
  const day = Math.floor(Date.now() / 86_400_000);
  const list = QUOTES[lang];
  return list[day % list.length];
}

export const hub = new Hono();

// Geocoding de Open-Meteo (gratis). La consulta llega transcripta de voz
// ("Rosario", "Rosario Argentina", "Ciudad de México"): probamos el texto entero
// y, si no hay nada, solo la primera parte antes de una coma o de "en".
hub.get("/location/search", async (c) => {
  const q = (c.req.query("q") ?? "").trim().slice(0, 80);
  if (!q) return c.json({ ok: false, error: "q is required" }, 400);
  const tries = [q, q.split(/,| en /i)[0].trim()].filter((t, i, a) => t && a.indexOf(t) === i);
  for (const name of tries) {
    try {
      const res = await fetch(
        `https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(name)}&count=5&language=es&format=json`,
      );
      if (!res.ok) continue;
      const data = (await res.json()) as {
        results?: { name: string; admin1?: string; country?: string; latitude: number; longitude: number; timezone?: string }[];
      };
      const results = (data.results ?? []).map((r) => ({
        name: r.name,
        label: [r.name, r.admin1, r.country].filter(Boolean).join(", "),
        lat: r.latitude,
        lon: r.longitude,
        timezone: r.timezone ?? TZ,
      }));
      if (results.length) return c.json({ ok: true, results });
    } catch (err) {
      console.error("hub geocode:", err);
    }
  }
  return c.json({ ok: true, results: [] });
});

hub.post("/location", async (c) => {
  let body: Partial<Place>;
  try {
    body = await c.req.json();
  } catch {
    return c.json({ ok: false, error: "invalid json" }, 400);
  }
  const lat = Number(body.lat), lon = Number(body.lon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return c.json({ ok: false, error: "lat/lon required" }, 400);
  const p: Place = {
    name: (body.name ?? "").toString().slice(0, 80),
    label: (body.label ?? "").toString().slice(0, 160),
    lat,
    lon,
    timezone: (body.timezone ?? TZ).toString().slice(0, 64),
  };
  await savePlace(p);
  console.log("hub place:", p.label || `${lat},${lon}`);
  return c.json({ ok: true, place: p });
});

hub.get("/location", async (c) => c.json({ ok: true, place: await place() }));

// Pronóstico para la pantalla de Clima del aparato: hoy por horas y los
// próximos días. Una sola llamada, cacheada 15 minutos como el resumen.
let forecastCache: { at: number; lang: Lang; value: object } | null = null;

hub.get("/forecast", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const p = await place();
  if (!p) return c.json({ ok: false, error: "no place set" }, 503);
  if (forecastCache && forecastCache.lang === lang && Date.now() - forecastCache.at < WEATHER_TTL_MS) {
    return c.json(forecastCache.value);
  }
  const tz = p.timezone || TZ;
  const url =
    `https://api.open-meteo.com/v1/forecast?latitude=${p.lat}&longitude=${p.lon}` +
    `&current=temperature_2m,relative_humidity_2m,weather_code,apparent_temperature,wind_speed_10m` +
    `&hourly=temperature_2m,weather_code,precipitation_probability` +
    `&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max,sunrise,sunset` +
    `&forecast_days=6&timezone=${encodeURIComponent(tz)}`;
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`open-meteo ${res.status}`);
    const d = (await res.json()) as {
      current: { temperature_2m: number; relative_humidity_2m: number; weather_code: number; apparent_temperature: number; wind_speed_10m: number };
      hourly: { time: string[]; temperature_2m: number[]; weather_code: number[]; precipitation_probability: number[] };
      daily: { time: string[]; temperature_2m_max: number[]; temperature_2m_min: number[]; weather_code: number[]; precipitation_probability_max: number[]; sunrise: string[]; sunset: string[] };
    };
    // Horas: de la próxima en adelante, de a dos, ocho tramos.
    const nowIso = new Date(Date.now() + tzOffsetMs(Date.now())).toISOString().slice(0, 13);
    let start = d.hourly.time.findIndex((t) => t.slice(0, 13) >= nowIso);
    if (start < 0) start = 0;
    const hours = [];
    for (let i = start; i < d.hourly.time.length && hours.length < 8; i += 2) {
      hours.push({
        h: d.hourly.time[i].slice(11, 16),
        t: Math.round(d.hourly.temperature_2m[i]),
        c: describeWeather(d.hourly.weather_code[i], lang),
        p: Math.round(d.hourly.precipitation_probability?.[i] ?? 0),
      });
    }
    const wd = new Intl.DateTimeFormat(lang === "en" ? "en-US" : lang, { weekday: "short", timeZone: tz });
    const days = d.daily.time.slice(0, 6).map((t, i) => ({
      d: wd.format(new Date(t + "T12:00:00Z")),
      date: t.slice(8, 10) + "/" + t.slice(5, 7),
      max: Math.round(d.daily.temperature_2m_max[i]),
      min: Math.round(d.daily.temperature_2m_min[i]),
      c: describeWeather(d.daily.weather_code[i], lang),
      p: Math.round(d.daily.precipitation_probability_max?.[i] ?? 0),
    }));
    const value = {
      ok: true,
      place: p.name || p.label,
      now: {
        t: Math.round(d.current.temperature_2m),
        feels: Math.round(d.current.apparent_temperature),
        hum: Math.round(d.current.relative_humidity_2m),
        wind: Math.round(d.current.wind_speed_10m),
        c: describeWeather(d.current.weather_code, lang),
      },
      sunrise: d.daily.sunrise?.[0]?.slice(11, 16) ?? "",
      sunset: d.daily.sunset?.[0]?.slice(11, 16) ?? "",
      hours,
      days,
    };
    forecastCache = { at: Date.now(), lang, value };
    return c.json(value);
  } catch (err) {
    console.error("forecast:", err);
    if (forecastCache) return c.json(forecastCache.value);
    return c.json({ ok: false, error: "weather unavailable" }, 502);
  }
});

hub.get("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const [w, d, s, ics, verse, store] = await Promise.all([weather(lang), data(), hubSlice(lang), agendaConfigured() ? todayForHub(lang) : Promise.resolve([]), verseOfTheDay(lang), loadStore()]);
  // Recordatorios, listas y mensajes salen del store del asistente (voice.ts);
  // el hub-data.json a mano sigue sirviendo para la agenda y como respaldo.
  return c.json({
    ok: true,
    now: Math.floor(Date.now() / 1000),
    weather: w,
    reminders: s.reminders.length ? s.reminders : (d.reminders ?? []).slice(0, 5),
    lists: s.lists,
    events: agendaConfigured() ? ics : (d.events ?? []).slice(0, 4),
    messages: s.messages.length ? s.messages : (d.messages ?? []).slice(0, 5),
    notes: s.notes,
    quote: d.quote || quoteOfTheDay(lang),
    verse,  // { ref, text } del día, o null si la Biblia no está
    // Ajustes cargados en /board; el aparato los aplica si `rev` subió.
    settings: store.settings ?? DEFAULT_SETTINGS,
  });
});

// Menú de un ítem de lista en el aparato (mover, fecha, borrar) o borrar una nota.
//   { kind: "item", id, action: "move", list } | { kind: "item", id, action: "date", dueDate: "YYYY-MM-DD" | null }
//   { kind: "item", id, action: "delete" } | { kind: "note", id, action: "delete" }
hub.post("/edit", async (c) => {
  let body: { kind?: string; id?: number; action?: string; list?: string; dueDate?: string | null };
  try {
    body = await c.req.json();
  } catch {
    return c.json({ ok: false, error: "invalid json" }, 400);
  }
  if (!Number.isFinite(Number(body.id))) return c.json({ ok: false, error: "id required" }, 400);
  return c.json({ ok: true, found: await editEntry(body) });
});

// El aparato tilda un recordatorio o un ítem de lista (OK en la pantalla de
// Recordatorios). Llega también desde la cola offline, por eso es idempotente.
hub.post("/done", async (c) => {
  let body: { kind?: string; id?: number; snooze?: number };
  try {
    body = await c.req.json();
  } catch {
    return c.json({ ok: false, error: "invalid json" }, 400);
  }
  const id = Number(body.id);
  if (!Number.isFinite(id) || (body.kind !== "reminder" && body.kind !== "item" && body.kind !== "message")) {
    return c.json({ ok: false, error: "kind (reminder|item|message) and id required" }, 400);
  }
  const found = await markDone(body.kind, id, Number(body.snooze) > 0 ? Number(body.snooze) : 0);
  return c.json({ ok: true, found });
});
