// Configuración editable desde la web (/board → Ajustes), guardada en el volumen
// de Railway. Antes todo esto eran variables de entorno y había que entrar a
// Railway para cambiar de proveedor de IA o de modelo.
//
// Las claves se guardan acá y NO se devuelven nunca por la API: el board solo
// ve si hay clave puesta o no. Las variables de entorno siguen sirviendo como
// valor por defecto (y como red de seguridad si el archivo se pierde).
import { readJsonSafe, writeJsonAtomic } from "./fsjson";

const FILE = process.env.CONFIG_FILE ?? "/data/config.json";

export type LlmConfig = {
  provider: "anthropic" | "openai";  // "openai" = cualquier API compatible (Groq, DeepSeek, OpenAI, ...)
  baseUrl: string;                   // solo para "openai"
  model: string;
  key: string;
};
export type SttConfig = { baseUrl: string; model: string; key: string };

// Búsqueda en internet para contestar cosas actuales. Con Anthropic la hace el
// propio modelo (herramienta del lado del servidor de Anthropic, ~USD 10 por
// cada 1000 búsquedas más los tokens de los resultados). Con un proveedor
// compatible con OpenAI esa herramienta no existe, así que busca el servidor y
// le pasa los resultados al modelo en el prompt: sin clave usa Google Noticias
// y DuckDuckGo (gratis), y con clave puede usar Tavily o Brave.
export type SearchConfig = {
  enabled: boolean;
  maxUses: number;                        // tope de búsquedas por respuesta (Anthropic)
  provider: "free" | "tavily" | "brave";  // buscador para los proveedores compatibles con OpenAI
  key: string;                            // clave del buscador; vacía = gratis, sin clave
};

export type Config = {
  llm: LlmConfig;
  stt: SttConfig;
  search: SearchConfig;
  deviceToken: string;  // vacío = solo vale el DEVICE_TOKEN del entorno
};

// Combos conocidos, para elegir de una lista en la web en vez de escribir URLs.
// Catálogos y precios revisados el 2026-09-08. Los modelos viejos de Groq
// (llama-3.3-70b-versatile, llama-3.1-8b-instant) pasaron a Enterprise / Contact
// Sales, así que con una cuenta normal de desarrollador ya no responden: los
// reemplazan los gpt-oss, que están abiertos. En DeepSeek, deepseek-chat y
// deepseek-reasoner quedaron atrás: hoy la familia es v4.
export const PRESETS: Record<string, { label: string; provider: "anthropic" | "openai"; baseUrl: string; models: string[]; sttBaseUrl?: string; sttModels?: string[] }> = {
  anthropic: {
    label: "Anthropic (Claude)",
    provider: "anthropic",
    baseUrl: "",
    models: ["claude-haiku-4-5", "claude-sonnet-4-5"],
  },
  groq: {
    label: "Groq (el más rápido y el más barato)",
    provider: "openai",
    baseUrl: "https://api.groq.com/openai/v1",
    // groq/compound y groq/compound-mini son "sistemas": traen búsqueda en
    // internet y ejecución de código del lado de Groq. Los gpt-oss traen su
    // propia búsqueda (browser_search). Ver llm.ts → providerSearchKind.
    models: ["openai/gpt-oss-120b", "openai/gpt-oss-20b", "groq/compound", "groq/compound-mini"],
    sttBaseUrl: "https://api.groq.com/openai/v1",
    sttModels: ["whisper-large-v3-turbo", "whisper-large-v3"],
  },
  deepseek: {
    label: "DeepSeek (muy barato, más barato aún fuera de hora pico)",
    // También aceptan https://api.deepseek.com/v1 y, en formato Anthropic,
    // https://api.deepseek.com/anthropic.
    provider: "openai",
    baseUrl: "https://api.deepseek.com",
    models: ["deepseek-v4-flash", "deepseek-v4-pro", "deepseek-v4-flash-vision-exp"],
  },
  openai: {
    label: "OpenAI",
    provider: "openai",
    baseUrl: "https://api.openai.com/v1",
    models: ["gpt-4o-mini", "gpt-4o"],
    sttBaseUrl: "https://api.openai.com/v1",
    sttModels: ["whisper-1", "gpt-4o-mini-transcribe"],
  },
};

// ── Cuánto sale cada consulta ───────────────────────────────────────────────
// El aparato se va a vender en volumen, así que el costo por consulta manda.
// Precios en dólares por millón de tokens (revisados el 2026-09-08). `offPeak`
// solo lo tiene DeepSeek, que cobra la mitad fuera de las horas pico.
export type ModelPrice = { in: number; out: number; offPeakIn?: number; offPeakOut?: number; note?: string };

export const MODEL_PRICES: Record<string, ModelPrice> = {
  "claude-haiku-4-5": { in: 1, out: 5 },
  "claude-sonnet-4-5": { in: 3, out: 15 },
  "openai/gpt-oss-120b": { in: 0.15, out: 0.6, note: "~500 tokens/s, trae búsqueda propia" },
  "openai/gpt-oss-20b": { in: 0.075, out: 0.3, note: "~1000 tokens/s, trae búsqueda propia" },
  "groq/compound": { in: 0.15, out: 0.6, note: "se cobran los modelos que use por dentro; búsqueda incluida" },
  "groq/compound-mini": { in: 0.15, out: 0.6, note: "una sola herramienta por pedido, 3x más rápido" },
  "deepseek-v4-flash": { in: 0.44, out: 1.32, offPeakIn: 0.22, offPeakOut: 0.66, note: "1M de contexto" },
  "deepseek-v4-pro": { in: 1.32, out: 3.96, offPeakIn: 0.66, offPeakOut: 1.98 },
  "deepseek-v4-flash-vision-exp": { in: 0.44, out: 1.32, offPeakIn: 0.22, offPeakOut: 0.66, note: "experimental, acepta imágenes" },
  "gpt-4o-mini": { in: 0.15, out: 0.6 },
  "gpt-4o": { in: 2.5, out: 10 },
};

// Transcripción: dólares por HORA de audio.
export const STT_PRICES: Record<string, number> = {
  "whisper-large-v3-turbo": 0.04,
  "whisper-large-v3": 0.111,
  "whisper-1": 0.36,            // OpenAI cobra USD 0,006 por minuto
  "gpt-4o-mini-transcribe": 0.18,
};

// Búsqueda en internet: dólares por búsqueda. En Anthropic es una herramienta
// aparte y sale MÁS que la respuesta entera (USD 10 por cada 1000 búsquedas);
// en Groq viene adentro del precio de los tokens.
export const SEARCH_PRICE_ANTHROPIC = 0.01;

// Horas pico de DeepSeek: 01:00-04:00 y 06:00-10:00 UTC de lunes a viernes.
// Fuera de eso sale la mitad.
export function deepSeekPeak(at: Date = new Date()): boolean {
  const day = at.getUTCDay();
  if (day === 0 || day === 6) return false;
  const h = at.getUTCHours();
  return (h >= 1 && h < 4) || (h >= 6 && h < 10);
}

// Costo de UNA consulta de voz: 5 s de audio, ~1500 tokens de entrada y 300 de
// salida. Es lo que hay que mirar para elegir proveedor.
export const QUERY_SHAPE = { seconds: 5, inTokens: 1500, outTokens: 300 };

export function queryCost(model: string, sttModel: string, at: Date = new Date()) {
  const p = MODEL_PRICES[model];
  const peak = deepSeekPeak(at);
  const inRate = p ? (!peak && p.offPeakIn !== undefined ? p.offPeakIn : p.in) : 0;
  const outRate = p ? (!peak && p.offPeakOut !== undefined ? p.offPeakOut : p.out) : 0;
  const llm = ((QUERY_SHAPE.inTokens * inRate) + (QUERY_SHAPE.outTokens * outRate)) / 1_000_000;
  const perHour = STT_PRICES[sttModel] ?? 0;
  const stt = (perHour * QUERY_SHAPE.seconds) / 3600;
  return {
    known: !!p,
    llm,
    stt,
    total: llm + stt,
    peak: p?.offPeakIn !== undefined ? peak : null,  // null = a este proveedor no le importa la hora
    note: p?.note ?? "",
  };
}

function defaults(): Config {
  return {
    llm: {
      provider: "anthropic",
      baseUrl: "",
      model: process.env.ASK_MODEL ?? process.env.VOICE_MODEL ?? "claude-haiku-4-5",
      key: process.env.ANTHROPIC_API_KEY ?? "",
    },
    stt: {
      baseUrl: (process.env.STT_BASE_URL ?? "https://api.openai.com/v1").replace(/\/+$/, ""),
      model: process.env.STT_MODEL ?? "whisper-1",
      key: process.env.STT_API_KEY ?? process.env.OPENAI_API_KEY ?? "",
    },
    search: {
      enabled: (process.env.WEB_SEARCH ?? "1") !== "0",
      maxUses: Math.max(1, Math.min(10, Number(process.env.WEB_SEARCH_MAX_USES ?? 3) || 3)),
      provider: (process.env.SEARCH_PROVIDER as SearchConfig["provider"]) ?? "free",
      key: process.env.SEARCH_API_KEY ?? "",
    },
    deviceToken: "",
  };
}

function merge(saved: Partial<Config> | null): Config {
  const base = defaults();
  const obj = <T>(v: unknown): Partial<T> => (v && typeof v === "object" && !Array.isArray(v) ? (v as Partial<T>) : {});
  const out: Config = {
    llm: { ...base.llm, ...obj<LlmConfig>(saved?.llm) },
    stt: { ...base.stt, ...obj<SttConfig>(saved?.stt) },
    search: { ...base.search, ...obj<SearchConfig>(saved?.search) },
    deviceToken: typeof saved?.deviceToken === "string" ? saved.deviceToken : "",
  };
  // Una clave vacía en el archivo no pisa la del entorno.
  if (!out.llm.key) out.llm.key = base.llm.key;
  if (!out.stt.key) out.stt.key = base.stt.key;
  if (!out.search.key) out.search.key = base.search.key;
  if (!["free", "tavily", "brave"].includes(out.search.provider)) out.search.provider = "free";
  out.search.maxUses = Math.max(1, Math.min(10, Math.round(Number(out.search.maxUses) || 3)));
  return out;
}

let cache: Config | null = null;
let loading: Promise<Config> | null = null;

export function config(): Promise<Config> {
  if (cache) return Promise.resolve(cache);
  // Igual que el store: se cachea la promesa, así dos pedidos juntos no leen el
  // archivo dos veces ni se quedan con dos copias distintas de las claves.
  loading ??= readJsonSafe<Partial<Config> | null>(FILE, null).then((saved) => {
    cache = merge(saved);
    loading = null;
    return cache;
  });
  return loading;
}

export async function saveConfig(next: Config): Promise<void> {
  cache = next;
  await writeJsonAtomic(FILE, next);
}

// Lo que puede ver la web: todo menos las claves (solo si están puestas).
export async function publicConfig() {
  const c = await config();
  return {
    llm: { provider: c.llm.provider, baseUrl: c.llm.baseUrl, model: c.llm.model, hasKey: !!c.llm.key },
    stt: { baseUrl: c.stt.baseUrl, model: c.stt.model, hasKey: !!c.stt.key },
    search: { enabled: c.search.enabled, maxUses: c.search.maxUses, provider: c.search.provider, hasKey: !!c.search.key },
    deviceTokenSet: !!c.deviceToken,
    presets: PRESETS,
  };
}
