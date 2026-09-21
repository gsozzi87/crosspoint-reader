// REV-084: CUÁNTOS TOKENS GASTA CADA COSA, POR MODELO Y POR DÍA.
//
// `usage.ts` cuenta LLAMADAS, y con eso no se puede contestar la única pregunta
// que importa cuando el proveedor devuelve 429: en qué se fue el cupo. Una
// llamada de Hablar son ~600 tokens y una traducción de un paper de PubMed
// ~3000; contarlas iguales es no contar. Y los límites del tramo gratis de Groq
// son de TOKENS (8K por minuto, 200K por día) y son POR MODELO, así que la
// unidad tiene que ser (día, modelo, subsistema).
//
// Cada respuesta del proveedor ya trae `usage`; lo único que faltaba era
// guardarlo. Los reintentos cuentan también, porque también gastan.
//
// Vive en memoria con un volcado al volumen: es diagnóstico, no un dato del
// producto, pero tiene que sobrevivir a un redespliegue o no sirve para mirar
// "lo de ayer". El volcado va debounceado: escribir un archivo por llamada al
// modelo es la clase de cosa que convierte un pico de uso en un disco lleno.
import { readJsonSafe, writeJsonAtomic } from "./fsjson";

const FILE = process.env.TOKENS_FILE ?? "/data/tokens.json";
const KEEP_DAYS = 14;
const SAVE_DEBOUNCE_MS = 30_000;

export type TokenRow = {
  day: string;        // YYYY-MM-DD (UTC, igual que los topes del proveedor)
  model: string;
  subsystem: string;  // "hablar", "noticias", "papers", "traductor", "apps", …
  calls: number;
  prompt: number;
  completion: number;
  reasoning: number;
  total: number;
};

type Table = Record<string, TokenRow>;

let table: Table = {};
let loaded = false;
let saveTimer: ReturnType<typeof setTimeout> | null = null;

function today(): string {
  return new Date().toISOString().slice(0, 10);
}

function keyOf(day: string, model: string, subsystem: string): string {
  return `${day}|${model}|${subsystem}`;
}

/** Tira lo más viejo que KEEP_DAYS. Se llama al cargar y al sumar. */
function prune(): void {
  const corte = new Date(Date.now() - KEEP_DAYS * 86400_000).toISOString().slice(0, 10);
  for (const k of Object.keys(table)) {
    if ((table[k]?.day ?? "") < corte) delete table[k];
  }
}

async function load(): Promise<void> {
  if (loaded) return;
  loaded = true;
  table = await readJsonSafe<Table>(FILE, {});
  prune();
}

function scheduleSave(): void {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    void writeJsonAtomic(FILE, table).catch((err) => console.error("tokens:", String(err).slice(0, 200)));
  }, SAVE_DEBOUNCE_MS);
  // No mantiene vivo el proceso por una escritura de diagnóstico.
  (saveTimer as unknown as { unref?: () => void }).unref?.();
}

export type TokenUsage = {
  prompt?: number;
  completion?: number;
  reasoning?: number;
  total?: number;
};

/**
 * Anota una llamada. NUNCA tira: que falle la contabilidad no puede tirar el
 * pedido, igual que en `usage.ts`.
 */
export function recordTokens(model: string, subsystem: string, u: TokenUsage): void {
  try {
    if (!loaded) void load();
    const day = today();
    const k = keyOf(day, model || "?", subsystem || "otro");
    const prompt = Math.max(0, Math.round(u.prompt ?? 0));
    const completion = Math.max(0, Math.round(u.completion ?? 0));
    const reasoning = Math.max(0, Math.round(u.reasoning ?? 0));
    // Algunos proveedores no mandan el total; se arma. El razonamiento ya viene
    // DENTRO de completion en la API de OpenAI, así que no se vuelve a sumar.
    const total = Math.max(0, Math.round(u.total ?? prompt + completion));
    const row = (table[k] ??= { day, model: model || "?", subsystem: subsystem || "otro", calls: 0, prompt: 0, completion: 0, reasoning: 0, total: 0 });
    row.calls++;
    row.prompt += prompt;
    row.completion += completion;
    row.reasoning += reasoning;
    row.total += total;
    prune();
    scheduleSave();
  } catch (err) {
    console.error("tokens:", String(err).slice(0, 200));
  }
}

/** Todo lo guardado, del día más nuevo al más viejo. Para `/board`. */
export async function tokenRows(): Promise<TokenRow[]> {
  await load();
  return Object.values(table).sort((a, b) => (a.day === b.day ? b.total - a.total : a.day < b.day ? 1 : -1));
}

/** Lo gastado HOY por ese modelo, que es contra lo que corre el tope diario. */
export async function todayTotalFor(model: string): Promise<number> {
  await load();
  const day = today();
  let n = 0;
  for (const row of Object.values(table)) {
    if (row.day === day && row.model === model) n += row.total;
  }
  return n;
}
