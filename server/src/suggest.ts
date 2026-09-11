// Sugerencias del día y del viaje, con IA.
//
// La idea del usuario, textual: "quiero sugerencias con IA, que vea por donde
// ando y me sugiera adónde visitar, en qué horarios, o cosas que me falten
// llevar" y "ir viendo en mi viaje a qué hora tomar el tren, a qué hora entrar
// al hotel, a qué hora son las entradas al Vaticano, cuánto tengo que ir de un
// lugar a otro".
//
//   GET /api/suggest/day?date=YYYY-MM-DD&lang=xx[&refresh=1]
//       -> { ok, date, at, ageS, stale, searched, lines:[...], text, sources:[{title,url}] }
//   GET /api/suggest/trip?id=&lang=xx[&refresh=1]
//       -> { ok, id, at, ageS, stale, searched, lines:[...], packing:[...], text, sources }
//
// PLATA. Cada sugerencia con búsqueda web cuesta (con Anthropic, USD 0,01 por
// búsqueda, más que la respuesta entera), así que:
//   - se cachean en /data/suggest.json, con la clave del día (o del viaje + el
//     día) y el idioma: una sola generación por clave;
//   - NUNCA se regeneran solas por vencimiento corto. Lo cacheado se devuelve
//     tal cual y el aparato muestra a qué hora se calculó; para recalcular hay
//     que pedirlo con `refresh=1` (OK en la pantalla), que es una acción del
//     usuario, no un temporizador;
//   - hay un tope diario de generaciones (SUGGEST_MAX_PER_DAY, 10 por default).
//     Pasado el tope se devuelve lo último que haya, marcado `stale`, y si no
//     hay nada, un error claro;
//   - la búsqueda web solo se enciende cuando hace falta de verdad: en el viaje
//     (horarios de museos, cuánto se tarda de un lugar a otro) y en el día
//     solo si ese día cae adentro de un viaje. Un día común en casa se contesta
//     con la agenda y el clima que ya tenemos, sin gastar una búsqueda.
import { Hono } from "hono";
import { mutateDoc, readDoc } from "./fsjson";
import { accountOf, type AppEnv } from "./tenant";
import { chatSearch, LlmError, type Source } from "./llm";
import { normalizeLang, LANGUAGE_NAME, type Lang } from "./lang";
import { occurrencesBetween } from "./calendar";
import { getTrip, tripOnDate, kindLabel, type Trip } from "./trips";
import { hubDiagnostics } from "./hub";
import { load as loadStore, memoryLines, todayLocal, pendingReminders } from "./store";
import { addUsage } from "./usage";

const MAX_PER_DAY = Math.max(1, Math.min(100, Number(process.env.SUGGEST_MAX_PER_DAY ?? 10) || 10));

// Lo que se le manda a la pantalla: renglones cortos, no un ensayo. La pantalla
// tiene 480 px de ancho y el usuario lee de un vistazo.
const MAX_LINES = 6;
const MAX_LINE_CHARS = 110;
const MAX_PACKING = 8;
const MAX_PACKING_CHARS = 48;

export type Suggestion = {
  key: string;
  at: number;          // epoch en segundos: cuándo se calculó (el aparato lo muestra)
  lang: Lang;
  lines: string[];
  packing: string[];   // solo en las del viaje: lo que el modelo dice que falta llevar
  searched: boolean;
  sources: Source[];
};

type Store = { version: 1; entries: Record<string, Suggestion>; budget: { date: string; count: number } };

const EMPTY: Store = { version: 1, entries: {}, budget: { date: "", count: 0 } };

// ---------------------------------------------------------------- caché

function shape(raw: unknown): Store {
  const s = (raw && typeof raw === "object" ? raw : structuredClone(EMPTY)) as Store;
  s.version ??= 1;
  s.entries ??= {};
  s.budget ??= { date: "", count: 0 };
  return s;
}

async function loadAll(accountId: number): Promise<Store> {
  return shape(await readDoc<unknown>(accountId, "suggest", null));
}

// Leer y escribir sin carreras: dos pedidos a la vez no se pisan (mismo patrón
// que trips.ts).
function update<T>(accountId: number, fn: (store: Store) => T | Promise<T>): Promise<T> {
  return mutateDoc(accountId, "suggest", shape, fn);
}

// Cuántas generaciones quedan hoy. El contador se reinicia solo al cambiar el día.
async function takeBudget(accountId: number): Promise<boolean> {
  const day = todayLocal();
  return update(accountId, (store) => {
    if (store.budget.date !== day) store.budget = { date: day, count: 0 };
    if (store.budget.count >= MAX_PER_DAY) return false;
    store.budget.count++;
    return true;
  });
}

async function remember(accountId: number, entry: Suggestion): Promise<void> {
  await update(accountId, (store) => {
    store.entries[entry.key] = entry;
    // El archivo no puede crecer para siempre: se guardan las 40 más nuevas.
    const keys = Object.keys(store.entries);
    if (keys.length > 40) {
      keys.sort((a, b) => (store.entries[a].at ?? 0) - (store.entries[b].at ?? 0));
      for (const k of keys.slice(0, keys.length - 40)) delete store.entries[k];
    }
  });
}

// ---------------------------------------------------------------- formato

// El modelo contesta en dos secciones para poder separar lo que se muestra de
// lo que se ofrece para agregar a la lista de cosas para llevar:
//
//   SUGERENCIAS
//   - salí 8:15, el tren tarda 35 min
//   FALTA
//   - adaptador de enchufe
//
// Sale más barato que dos llamadas y convive con la búsqueda web (una respuesta
// con esquema JSON no la acepta en todos los proveedores).
const FORMAT = [
  "Contesta EXACTAMENTE con este formato, sin nada más:",
  "SUGERENCIAS",
  "- una sugerencia por renglón",
  "FALTA",
  "- una cosa por renglón",
  `Máximo ${MAX_LINES} sugerencias de hasta ${MAX_LINE_CHARS} caracteres cada una, texto plano, sin markdown, sin negritas, sin emojis.`,
  "Cada sugerencia tiene que ser accionable y concreta: una hora, un tiempo de viaje, un lugar, algo que preparar.",
  "Nada de introducciones, resúmenes ni frases de relleno.",
  "Si no tienes nada útil que decir en una sección, deja la sección vacía.",
];

function cleanLine(raw: string, max: number): string {
  return raw
    .replace(/^\s*[-*•·\u2022]\s*/, "")
    .replace(/^\s*\d+[.)]\s*/, "")
    .replace(/\*\*/g, "")
    .replace(/[`_#]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, max);
}

// Parte la respuesta en las dos secciones. Un modelo que se olvide de los
// encabezados igual sirve: todo lo que venga antes de "FALTA" son sugerencias.
export function parseSections(text: string): { lines: string[]; packing: string[] } {
  const lines: string[] = [];
  const packing: string[] = [];
  let section: "lines" | "packing" = "lines";
  for (const raw of (text ?? "").split(/\r?\n/)) {
    const head = raw.trim().replace(/[:：]\s*$/, "").toUpperCase();
    if (/^(SUGERENCIAS|SUGGESTIONS|SUGESTÕES|SUGGESTIONS?|VORSCHLÄGE|СОВЕТЫ|РЕКОМЕНДАЦИИ)$/.test(head)) {
      section = "lines";
      continue;
    }
    if (/^(FALTA|FALTAN|MISSING|MANQUE|FEHLT|ЧЕГО НЕ ХВАТАЕТ|НЕ ХВАТАЕТ|PACKING)$/.test(head)) {
      section = "packing";
      continue;
    }
    const text2 = cleanLine(raw, section === "lines" ? MAX_LINE_CHARS : MAX_PACKING_CHARS);
    if (!text2) continue;
    if (section === "lines") {
      if (lines.length < MAX_LINES) lines.push(text2);
    } else if (packing.length < MAX_PACKING) {
      packing.push(text2);
    }
  }
  return { lines, packing };
}

// ---------------------------------------------------------------- contexto

function systemPrompt(lang: Lang): string {
  return [
    `Eres el asistente de un lector de tinta electrónica. Contesta SIEMPRE en ${LANGUAGE_NAME[lang]}.`,
    "La pantalla es chica (480x800) y el usuario lee de un vistazo: renglones cortos y concretos.",
    "No repitas lo que ya está en la agenda: agrega lo que el usuario NO sabe (a qué hora conviene salir,",
    "cuánto se tarda de un lugar al otro, qué conviene reservar antes, qué le falta preparar o llevar).",
    "Si no estás seguro de un horario, dilo en la misma línea con 'aprox.' en vez de inventarlo.",
    ...FORMAT,
  ].join("\n");
}

// La agenda del día en texto, tal como la ve el calendario (incluye lo que el
// viaje espeja ahí y los recordatorios con fecha).
async function agendaText(accountId: number, date: string, lang: Lang): Promise<string> {
  const { items } = await occurrencesBetween(accountId, date, date, lang);
  if (!items.length) return "(no hay nada agendado)";
  return items
    .slice(0, 20)
    .map((o) => `- ${o.allDay ? "todo el día" : o.time} ${o.title}${o.place ? ` (${o.place})` : ""}`)
    .join("\n");
}

function tripText(trip: Trip, lang: Lang, fromDate?: string): string {
  const out: string[] = [`Viaje: ${trip.name}${trip.place ? ` — ${trip.place}` : ""} (${trip.start} a ${trip.end})`];
  const days = fromDate ? trip.days.filter((d) => d.date >= fromDate).slice(0, 8) : trip.days.slice(0, 12);
  for (const day of days) {
    out.push(`${day.date}${day.note ? ` — ${day.note}` : ""}`);
    if (!day.items.length) out.push("  (sin nada cargado)");
    for (const item of day.items.slice(0, 12)) {
      out.push(
        `  - ${item.at ?? "s/hora"} ${item.title} [${kindLabel(item.kind, lang)}]` +
          `${item.place ? ` en ${item.place}` : ""}${item.note ? ` — ${item.note}` : ""}`,
      );
    }
  }
  const left = trip.packing.filter((p) => !p.done).map((p) => p.text);
  const done = trip.packing.filter((p) => p.done).map((p) => p.text);
  out.push(`Cosas para llevar ya anotadas (no las repitas): ${[...left, ...done].join(", ") || "(ninguna)"}`);
  return out.join("\n");
}

// Clima y lugar salen del hub: es el mismo que ve el usuario en la pantalla.
async function placeAndWeather(accountId: number): Promise<string> {
  try {
    const d = await hubDiagnostics(accountId);
    const where = d.place ? `${d.place.label || d.place.name} (${d.place.lat}, ${d.place.lon})` : "(sin lugar cargado)";
    const w = d.weather.line ? `${d.weather.line} · ${d.weather.detail}` : "(sin clima)";
    return `Dónde está: ${where}\nClima de hoy ahí: ${w}`;
  } catch {
    return "Dónde está: (sin datos)";
  }
}

async function pendingText(accountId: number): Promise<string> {
  const store = await loadStore(accountId);
  const rem = pendingReminders(store)
    .slice(0, 8)
    .map((r) => `- ${r.title}${r.dueAt ? ` (${r.dueAt.replace("T", " ")})` : ""}`);
  const items: string[] = [];
  for (const [name, list] of Object.entries(store.lists ?? {})) {
    const left = list.filter((i) => !i.done);
    if (left.length) items.push(`- ${name}: ${left.slice(0, 6).map((i) => i.text).join(", ")}`);
  }
  return [
    `Recordatorios pendientes:\n${rem.join("\n") || "(ninguno)"}`,
    `Listas pendientes:\n${items.join("\n") || "(ninguna)"}`,
  ].join("\n");
}

async function memories(accountId: number): Promise<string[]> {
  try {
    return memoryLines(await loadStore(accountId));
  } catch {
    return [];
  }
}

// ---------------------------------------------------------------- generación

type Ask = { accountId: number; key: string; lang: Lang; user: string; search: "off" | "force" };

async function generate(ask: Ask): Promise<Suggestion> {
  const res = await chatSearch({
    system: systemPrompt(ask.lang),
    user: ask.user,
    lang: ask.lang,
    maxTokens: 700,
    search: ask.search,
    memories: await memories(ask.accountId),
  });
  // Se cobra ACÁ y no en la ruta: la ruta sirve de la caché la mayoría de las
  // veces y ahí no se llama al modelo. chatSearch con búsqueda web es de lo
  // más caro que hace el servidor y hasta ahora no sumaba nada.
  void addUsage(ask.accountId, { llm: 1 });
  const { lines, packing } = parseSections(res.text);
  return {
    key: ask.key,
    at: Math.floor(Date.now() / 1000),
    lang: ask.lang,
    lines,
    packing,
    searched: res.searched,
    sources: res.sources.slice(0, 4),
  };
}

function view(entry: Suggestion, stale = false) {
  const now = Math.floor(Date.now() / 1000);
  return {
    ok: true,
    at: entry.at,
    ageS: Math.max(0, now - entry.at),
    stale,
    searched: entry.searched,
    lines: entry.lines,
    packing: entry.packing,
    text: entry.lines.join("\n"),
    sources: entry.sources,
  };
}

// Lo común de las dos rutas: caché, tope diario y errores del proveedor
// traducidos a algo que el aparato pueda mostrar.
async function serve(ask: Ask, refresh: boolean, extra: Record<string, unknown>) {
  const store = await loadAll(ask.accountId);
  const cached = store.entries[ask.key];
  if (cached && !refresh) return { status: 200 as const, body: { ...view(cached), ...extra } };
  if (!(await takeBudget(ask.accountId))) {
    // Se acabó el presupuesto del día: lo viejo sirve más que un error.
    if (cached) return { status: 200 as const, body: { ...view(cached, true), ...extra, budget: "spent" } };
    return { status: 429 as const, body: { ok: false, error: "budget", ...extra } };
  }
  try {
    const entry = await generate(ask);
    if (!entry.lines.length && !entry.packing.length && cached) {
      return { status: 200 as const, body: { ...view(cached, true), ...extra } };
    }
    await remember(ask.accountId, entry);
    return { status: 200 as const, body: { ...view(entry), ...extra } };
  } catch (err) {
    const code = err instanceof LlmError ? err.code : "provider_error";
    console.error("suggest:", ask.key, err);
    if (cached) return { status: 200 as const, body: { ...view(cached, true), ...extra, warn: code } };
    return { status: 502 as const, body: { ok: false, error: code, ...extra } };
  }
}

// ---------------------------------------------------------------- rutas

export const suggest = new Hono<AppEnv>();

// Sugerencias para un día: qué hay agendado, qué conviene hacer antes, cuánto
// se tarda de un lugar al siguiente y qué falta preparar.
suggest.get("/day", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const date = /^\d{4}-\d{2}-\d{2}$/.test(c.req.query("date") ?? "") ? (c.req.query("date") as string) : todayLocal();
  const refresh = c.req.query("refresh") === "1";
  const trip = await tripOnDate(acc, date);
  const user = [
    `Fecha: ${date}${date === todayLocal() ? " (hoy)" : ""}`,
    await placeAndWeather(acc),
    `Agenda del día:\n${await agendaText(acc, date, lang)}`,
    await pendingText(acc),
    trip ? `Ese día está de viaje:\n${tripText(trip, lang, date)}` : "",
    trip
      ? "Sugiere qué visitar cerca y a qué hora, cuánto se tarda entre los lugares del día y qué conviene tener listo."
      : "Sugiere cómo ordenar el día, a qué hora conviene salir para cada cosa y qué falta preparar.",
  ]
    .filter(Boolean)
    .join("\n\n");
  // Solo se busca en internet si ese día está de viaje: ahí es donde los datos
  // de ahora (horarios, trayectos) valen lo que cuestan.
  const r = await serve(
    { accountId: acc, key: `day:${date}:${lang}${trip ? `:${trip.id}` : ""}`, lang, user, search: trip ? "force" : "off" },
    refresh,
    { date, trip: trip?.id ?? "" },
  );
  return c.json(r.body, r.status);
});

// Sugerencias de un viaje: qué visitar cerca y en qué horario, y qué falta en
// la lista de cosas para llevar según el destino, las fechas y lo que ya anotó.
suggest.get("/trip", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const id = (c.req.query("id") ?? "").toString();
  const refresh = c.req.query("refresh") === "1";
  const trip = await getTrip(acc, id);
  if (!trip) return c.json({ ok: false, error: "not found" }, 404);
  const today = todayLocal();
  const user = [
    `Hoy es ${today}.`,
    tripText(trip, lang, today <= trip.start ? undefined : today),
    "Sugiere qué visitar cerca de los lugares del viaje y en qué horario conviene ir,",
    "cuánto se tarda de un lugar al siguiente y qué conviene reservar antes.",
    "En FALTA pon SOLO cosas que hay que llevar y todavía no están anotadas,",
    "pensando en el destino, la época del año, el clima, cuántos días dura y qué tipo de actividades hay.",
  ].join("\n\n");
  const r = await serve(
    { accountId: acc, key: `trip:${trip.id}:${lang}:${today}`, lang, user, search: "force" },
    refresh,
    { id: trip.id, name: trip.name },
  );
  return c.json(r.body, r.status);
});

// Para /board y las pruebas: qué hay cacheado y cuánto presupuesto queda hoy.
suggest.get("/status", async (c) => {
  const store = await loadAll(accountOf(c));
  const day = todayLocal();
  return c.json({
    ok: true,
    maxPerDay: MAX_PER_DAY,
    usedToday: store.budget.date === day ? store.budget.count : 0,
    entries: Object.values(store.entries).map((e) => ({ key: e.key, at: e.at, lines: e.lines.length, searched: e.searched })),
  });
});
