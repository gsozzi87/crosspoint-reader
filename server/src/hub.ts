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
//     lists: [{ key, name, items }],         // solo dos: compras y tareas
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
// Recordatorios y agenda: por ahora se leen de /data/hub-data.json
// (volumen de Railway) con la misma forma que la respuesta; la Fase 2 los
// reemplaza por las tablas de verdad. Si el archivo no existe, van vacíos.
import { Hono } from "hono";
import { readDoc, writeDoc } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";
import { DEFAULT_ACCOUNT, multiUser } from "./db";
import { readBody } from "./net";
import { hubSlice, markDone, editEntry } from "./voice";
import { QUOTES, LABELS, describeWeather, normalizeLang, type Lang } from "./lang";
import { VOICES } from "./tts";
import { metNoForecast, type MetNoData } from "./metno";
import { load as loadStore, mutate as mutateStore, DEFAULT_SETTINGS, refreshTimeZone, forgetTimeZone, repeatText, whenLabel, upsertReminder, normalizeRepeat, repeatToWire, localToEpoch } from "./store";
import { agendaConfigured, todayForHub } from "./agenda";
import { verseOfTheDay } from "./bible";

const LAT = process.env.HUB_LAT ?? "";
const LON = process.env.HUB_LON ?? "";
const TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";
const WEATHER_TTL_MS = 15 * 60 * 1000;

type Place = { name: string; label: string; lat: number; lon: number; timezone: string };

// Desfase (ms) de la zona del lugar respecto de UTC, para ubicar "ahora" en
// las horas del pronóstico.
function tzOffsetMs(at: number, tz: string = TZ): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: tz, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  return Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second")) - Math.floor(at / 1000) * 1000;
}

// Las cachés son POR CUENTA: con un solo lugar global, el aparato de una casa
// veía el clima de otra. Con tope, que 1000 cuentas en un Map sin límite es una
// fuga de memoria.
const MAX_CACHED = 256;

function cacheSet<T>(m: Map<number, T>, k: number, v: T): T {
  m.delete(k);
  m.set(k, v);
  while (m.size > MAX_CACHED) {
    const oldest = m.keys().next().value;
    if (oldest === undefined) break;
    m.delete(oldest);
  }
  return v;
}

const placeCache = new Map<number, Place>();
const forecastCache = new Map<number, { at: number; lang: Lang; value: object }>();

// Ojo: el "no hay lugar" NO se cachea. Si se cacheara, un proceso que arrancó
// antes de que se guardara el lugar no volvería a leer el archivo nunca más y el
// clima quedaría vacío para siempre aunque el lugar ya esté cargado.
// Un archivo a medias (o de otra versión) no puede dar un lugar con lat/lon
// NaN: con eso Open-Meteo contesta 400 y el clima queda roto sin explicación.
function validPlace(raw: unknown): Place | null {
  const r = (raw && typeof raw === "object" ? raw : {}) as Record<string, any>;
  const lat = Number(r.lat), lon = Number(r.lon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
  if (Math.abs(lat) > 90 || Math.abs(lon) > 180) return null;
  return {
    name: String(r.name ?? "").slice(0, 80),
    label: String(r.label ?? "").slice(0, 160),
    lat,
    lon,
    timezone: String(r.timezone || TZ).slice(0, 64),
  };
}

async function place(accountId: number): Promise<Place | null> {
  const hit = placeCache.get(accountId);
  if (hit) return hit;
  const fromEnv = LAT && LON ? validPlace({ lat: LAT, lon: LON, timezone: TZ }) : null;
  const saved = validPlace(await readDoc<unknown>(accountId, "hub-settings", null));
  if (!saved) return fromEnv;
  return cacheSet(placeCache, accountId, saved);
}

async function savePlace(accountId: number, p: Place): Promise<void> {
  await writeDoc(accountId, "hub-settings", p);
  cacheSet(placeCache, accountId, p);
  weatherCache.delete(accountId);
  forecastCache.delete(accountId);  // el pronóstico cacheado era del lugar viejo
  forgetTimeZone(accountId);        // y la zona horaria también cambió
}

type Weather = { line: string; detail: string; noPlace?: boolean; error?: string };

// Open-Meteo primero; si el servidor no puede con él (desde Railway venía dando
// 502 sin parar), se cae a met.no, que devuelve lo mismo traducido en metno.ts.
async function openMeteoOrMetNo(url: string, p: Place): Promise<MetNoData> {
  try {
    const res = await fetchRetry(url);
    if (!res.ok) throw new Error(`open-meteo ${res.status}: ${(await res.text()).slice(0, 120)}`);
    return (await res.json()) as MetNoData;
  } catch (err) {
    console.error("open-meteo falló, probando met.no:", err);
    return metNoForecast(p.lat, p.lon, p.timezone || TZ);
  }
}

const weatherCache = new Map<number, { at: number; lang: Lang; value: Weather }>();

// Open-Meteo desde Railway falla de a ratos (corte de red, 429 por IP compartida).
// Con un solo intento el clima quedaba vacío hasta el próximo ciclo de 15 minutos.
async function fetchRetry(url: string, tries = 3): Promise<Response> {
  let last: unknown;
  for (let i = 0; i < tries; i++) {
    try {
      const res = await fetch(url, { signal: AbortSignal.timeout(8000) });
      if (res.ok || (res.status >= 400 && res.status < 500 && res.status !== 429)) return res;
      last = new Error(`open-meteo ${res.status}`);
    } catch (err) {
      last = err;
    }
    if (i < tries - 1) await new Promise((r) => setTimeout(r, 400 * (i + 1)));
  }
  throw last instanceof Error ? last : new Error(String(last));
}

async function weather(accountId: number, lang: Lang): Promise<Weather> {
  const p = await place(accountId);
  // Sin lugar guardado no hay clima posible: el aparato lo dice tal cual
  // ("cargá el lugar en la web") en vez de un "sin datos" que no explica nada.
  if (!p) return { line: "", detail: "", noPlace: true };
  const cached = weatherCache.get(accountId);
  if (cached && cached.lang === lang && Date.now() - cached.at < WEATHER_TTL_MS) return cached.value;
  const url =
    `https://api.open-meteo.com/v1/forecast?latitude=${p.lat}&longitude=${p.lon}` +
    `&current=temperature_2m,relative_humidity_2m,weather_code` +
    `&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=${encodeURIComponent(p.timezone || TZ)}`;
  try {
    const data = await openMeteoOrMetNo(url, p);
    const l = LABELS[lang];
    const value = {
      line: `${describeWeather(data.current.weather_code, lang)} · ${Math.round(data.current.temperature_2m)}°`,
      detail:
        (p.name ? `${p.name} · ` : "") +
        `${l.max} ${Math.round(data.daily.temperature_2m_max[0])}° · ${l.min} ${Math.round(data.daily.temperature_2m_min[0])}°` +
        ` · ${l.hum} ${Math.round(data.current.relative_humidity_2m)} %`,
    };
    cacheSet(weatherCache, accountId, { at: Date.now(), lang, value });
    return value;
  } catch (err) {
    console.error("hub weather:", p.label || `${p.lat},${p.lon}`, err);
    return weatherCache.get(accountId)?.value ?? { line: "", detail: "", error: String(err).slice(0, 200) };
  }
}

type HubData = {
  reminders?: { title: string; when: string }[];
  events?: { when: string; title: string }[];
  quote?: string;
};

async function data(accountId: number): Promise<HubData> {
  const raw = await readDoc<unknown>(accountId, "hub-data", {});
  const r = (raw && typeof raw === "object" && !Array.isArray(raw) ? raw : {}) as Record<string, any>;
  // Lo que no sea array se ignora: este archivo se edita a mano y un campo
  // suelto no tiene que tirar abajo la sincronización entera del aparato.
  const arr = <T>(v: unknown): T[] | undefined => (Array.isArray(v) ? (v as T[]) : undefined);
  return {
    reminders: arr(r.reminders),
    events: arr(r.events),
    quote: typeof r.quote === "string" ? r.quote : undefined,
  };
}

function quoteOfTheDay(lang: Lang): string {
  const day = Math.floor(Date.now() / 86_400_000);
  const list = QUOTES[lang];
  return list[day % list.length];
}

// `HUB_ICS_URL` es una variable del entorno, o sea del OPERADOR, no de una
// cuenta: si se usara para todas, el calendario privado del que puso la variable
// se lo verían los 1000 aparatos. En multiusuario vale solo para la cuenta 1
// (la que ya venía andando con esa variable puesta).
function icsFor(accountId: number): boolean {
  if (!agendaConfigured()) return false;
  return !multiUser || accountId === DEFAULT_ACCOUNT;
}

export const hub = new Hono<AppEnv>();

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
        { signal: AbortSignal.timeout(8000) },
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
  // readBody y no c.req.json(): un cuerpo literal "null" pasaba el catch y
  // reventaba en la primera propiedad que se leía.
  const body: Partial<Place> = await readBody(c);
  const lat = Number(body.lat), lon = Number(body.lon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return c.json({ ok: false, error: "lat/lon required" }, 400);
  const p: Place = {
    name: (body.name ?? "").toString().slice(0, 80),
    label: (body.label ?? "").toString().slice(0, 160),
    lat,
    lon,
    timezone: (body.timezone ?? TZ).toString().slice(0, 64),
  };
  const acc = accountOf(c);
  try {
    await savePlace(acc, p);
  } catch (err) {
    // Sin volumen montado esto tiraba un 500 pelado y el aparato decía "error".
    console.error("hub place:", err);
    return c.json({ ok: false, error: "no se pudo guardar el lugar", code: "storage" }, 500);
  }
  console.log("hub place:", p.label || `${lat},${lon}`);
  // Se consulta el clima ahí mismo: así /board muestra enseguida si el lugar
  // nuevo anda, sin esperar a que el aparato sincronice.
  const w = await weather(acc, normalizeLang(c.req.query("lang")));
  return c.json({ ok: true, place: p, weather: w });
});

hub.get("/location", async (c) => c.json({ ok: true, place: await place(accountOf(c)) }));

// Pronóstico para la pantalla de Clima del aparato: hoy por horas y los
// próximos días. Una sola llamada, cacheada 15 minutos como el resumen.
hub.get("/forecast", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const acc = accountOf(c);
  const p = await place(acc);
  if (!p) return c.json({ ok: false, noPlace: true, error: "no place set" }, 503);
  const cached = forecastCache.get(acc);
  if (cached && cached.lang === lang && Date.now() - cached.at < WEATHER_TTL_MS) {
    return c.json(cached.value);
  }
  const tz = p.timezone || TZ;
  const url =
    `https://api.open-meteo.com/v1/forecast?latitude=${p.lat}&longitude=${p.lon}` +
    `&current=temperature_2m,relative_humidity_2m,weather_code,apparent_temperature,wind_speed_10m` +
    `&hourly=temperature_2m,weather_code,precipitation_probability` +
    `&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max,sunrise,sunset` +
    `&forecast_days=6&timezone=${encodeURIComponent(tz)}`;
  try {
    const d = await openMeteoOrMetNo(url, p);
    // Horas: de la próxima en adelante, de a dos, ocho tramos.
    // La zona del lugar elegido, no la del env: si no, las horas del pronóstico
  // arrancan corridas para cualquier ciudad de otro huso.
  const nowIso = new Date(Date.now() + tzOffsetMs(Date.now(), tz)).toISOString().slice(0, 13);
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
    cacheSet(forecastCache, acc, { at: Date.now(), lang, value });
    return c.json(value);
  } catch (err) {
    console.error("forecast:", err);
    // Un pronóstico viejo sirve, pero no uno de ayer.
    const stale = forecastCache.get(acc);
    if (stale && Date.now() - stale.at < 6 * 3600 * 1000) return c.json(stale.value);
    return c.json({ ok: false, error: String(err).slice(0, 200) }, 502);
  }
});

// Cuándo fue la última vez que el aparato pidió sus datos. Se ve en /board para
// saber si ya se llevó lo que se cargó desde el teléfono. Por cuenta: con una
// sola variable, la web de una cuenta mostraba la sincronización de otra.
const lastFetchByAccount = new Map<number, number>();

// Lo que /board muestra para saber por qué el clima está vacío y si el aparato
// ya vino a buscar los datos.
export async function hubDiagnostics(accountId: number): Promise<{ place: Place | null; weather: Weather; lastDeviceFetch: number }> {
  return {
    place: await place(accountId),
    weather: await weather(accountId, "es"),
    lastDeviceFetch: lastFetchByAccount.get(accountId) ?? 0,
  };
}

hub.get("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const acc = accountOf(c);
  cacheSet(lastFetchByAccount, acc, Date.now());
  // La zona del lugar guardado, antes de armar las horas de los recordatorios.
  await refreshTimeZone(acc);
  const [w, d, s, ics, verse, store] = await Promise.all([weather(acc, lang), data(acc), hubSlice(acc, lang), icsFor(acc) ? todayForHub(lang) : Promise.resolve([]), verseOfTheDay(lang), loadStore(acc)]);
  // Recordatorios y listas salen del store del asistente (voice.ts); el
  // hub-data.json a mano sigue sirviendo para la agenda y como respaldo.
  return c.json({
    ok: true,
    now: Math.floor(Date.now() / 1000),
    weather: w,
    reminders: s.reminders.length ? s.reminders : (d.reminders ?? []).slice(0, 5),
    lists: s.lists,
    events: icsFor(acc) ? ics : (d.events ?? []).slice(0, 4),
    notes: s.notes,
    quote: d.quote || quoteOfTheDay(lang),
    verse,  // { ref, text } del día, o null si la Biblia no está
    // Ajustes cargados en /board; el aparato los aplica si `rev` subió.
    settings: store.settings ?? DEFAULT_SETTINGS,
    // Voz de Piper en uso: si cambió, el aparato tira los clips que tenía
    // cacheados en la SD (si no, sigue avisando con la voz vieja para siempre).
    ttsVoice: VOICES[lang],
  });
});

// Menú de un ítem de lista en el aparato (mover, fecha, borrar) o borrar una nota.
//   { kind: "item", id, action: "move", list } | { kind: "item", id, action: "date", dueDate: "YYYY-MM-DD" | null }
//   { kind: "item", id, action: "delete" } | { kind: "note", id, action: "delete" }
hub.post("/edit", async (c) => {
  // readBody: un cuerpo literal "null" pasaba el catch y reventaba en la
  // primera propiedad que se leía.
  let body: { kind?: string; id?: number; action?: string; list?: string; dueDate?: string | null };
  body = await readBody(c);
  if (!Number.isFinite(Number(body.id))) return c.json({ ok: false, error: "id required" }, 400);
  return c.json({ ok: true, found: await editEntry(accountOf(c), body) });
});

// Alta y EDICIÓN de un recordatorio desde el aparato o desde /board: título,
// fecha, hora y repetición. Sin `id` es un alta; con `id` se edita el que ya
// está (hasta ahora un recordatorio solo se podía crear por voz y tildar, y no
// había forma de ver ni cambiar cada cuánto iba a sonar).
//   { id?, title, dueAt: "YYYY-MM-DD"|"YYYY-MM-DDTHH:MM"|null,
//     repeat: { kind, days?, interval?, until? } }
//   -> { ok, reminder: { id, title, dueAt, at, when, repeat, repeatText }, created }
hub.post("/reminder", async (c) => {
  const acc = accountOf(c);
  await refreshTimeZone(acc);
  const lang = normalizeLang(c.req.query("lang"));
  const body = await readBody(c);
  const res = await mutateStore(acc, (store) => upsertReminder(store, body));
  if (!res.ok) {
    return res.error === "not_found"
      ? c.json({ ok: false, error: "no existe ese recordatorio", code: "not_found" }, 404)
      : c.json({ ok: false, error: "title required" }, 400);
  }
  const r = res.reminder;
  console.log(`hub reminder ${res.created ? "nuevo" : "editado"}: ${r.id} "${r.title}" ${r.dueAt ?? "sin fecha"} (${repeatText(r.repeat, r.dueAt, "es")})`);
  return c.json({
    ok: true,
    created: res.created,
    reminder: {
      id: r.id, title: r.title, at: r.dueAt, when: whenLabel(r.dueAt, lang),
      dueAt: localToEpoch(r.dueAt),  // epoch UTC: con esto el aparato arma el wake del deep sleep
      ...repeatToWire(r.repeat, r.dueAt),          // lo que lee el firmware
      repeatSpec: normalizeRepeat(r.repeat),       // el objeto entero (para /board)
      repeatText: repeatText(r.repeat, r.dueAt, lang), done: r.done,
    },
  });
});

// El aparato tilda un recordatorio o un ítem de lista (OK en la pantalla de
// Recordatorios). Llega también desde la cola offline, por eso es idempotente.
hub.post("/done", async (c) => {
  // readBody: un cuerpo literal "null" pasaba el catch y reventaba en la
  // primera propiedad que se leía.
  // `at` = el dueAt (epoch UTC) de la ocurrencia que se está tildando. Lo manda
  // el aparato para que un reintento del mismo tilde no le coma otro ciclo a un
  // recordatorio con repetición; sin él todo sigue igual que antes.
  let body: { kind?: string; id?: number; snooze?: number; at?: number };
  body = await readBody(c);
  const id = Number(body.id);
  if (!Number.isFinite(id) || (body.kind !== "reminder" && body.kind !== "item")) {
    return c.json({ ok: false, error: "kind (reminder|item) and id required" }, 400);
  }
  const at = Number(body.at) > 0 ? Math.floor(Number(body.at)) : 0;
  const found = await markDone(accountOf(c), body.kind, id, Number(body.snooze) > 0 ? Number(body.snooze) : 0, at);
  return c.json({ ok: true, found });
});
