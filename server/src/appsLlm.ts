// El cliente de Anthropic de las apps de Lua (el Escritor), APARTE de llm.ts.
//
// Por qué no es el mismo: las apps escriben prosa larga —un capítulo son
// minutos y miles de tokens— con un modelo caro, y el dueño quiere ese gasto
// con su propia clave y contado aparte del de Hablar (`config.apps`, editable
// en /board → Ajustes → Avanzado → Apps de Lua). Sin clave, cada servicio que
// la necesite contesta con `AppsLlmError` código `no_key` y el aparato muestra
// el mensaje tal cual: "Carga la clave de las apps en la web".
//
// Dos formas de uso, las únicas que necesitan las apps:
//   appsJson()   → una respuesta que cumple un JSON Schema (enfoques, índice),
//                  una sola llamada sin streaming, tope 25 s: el aparato corta
//                  a los 40 y estos servicios son síncronos.
//   appsProse()  → un capítulo entero, con streaming y `finalMessage()`: un
//                  capítulo de 600 palabras tarda lo que tarda, y sin streaming
//                  el SDK y el proxy de Railway cortan la conexión muda.
//
// Hubo un tercero, `appsProseSearch()` (lo mismo con la búsqueda web de
// Anthropic declarada), que era de la guía de Viajes y se fue con ella. Si
// alguna app vuelve a necesitar búsqueda, está en el historial y `webSearchTool`
// sigue vivo en llm.ts (lo usa `chatSearch`).
//
// Solo Anthropic: el pensamiento adaptativo, la salida estructurada nativa y
// el streaming largo son del SDK; un proveedor compatible con OpenAI no entra
// acá a propósito (ver config.ts → APPS_MODELS).
import Anthropic from "@anthropic-ai/sdk";
import { config } from "./config";
import { addUsage } from "./usage";
import { redactSecrets } from "./net";

export type AppsLlmCode = "no_key" | "provider_error" | "refused" | "bad_answer";

export const NO_KEY_MSG = "Carga la clave de las apps en la web (Ajustes → Apps de Lua)";

export class AppsLlmError extends Error {
  constructor(message: string, readonly code: AppsLlmCode = "provider_error") {
    super(message);
  }
}

const JSON_TIMEOUT_MS = 25_000;
const PROSE_TIMEOUT_MS = 240_000;

type Client = { client: Anthropic; model: string; key: string };

async function clientFor(timeoutMs: number, maxRetries: number): Promise<Client> {
  const c = (await config()).apps;
  if (!c.key) throw new AppsLlmError(NO_KEY_MSG, "no_key");
  return { client: new Anthropic({ apiKey: c.key, timeout: timeoutMs, maxRetries }), model: c.model, key: c.key };
}

// Un error del SDK se traduce a algo corto y sin la clave adentro: el mensaje
// vuelve al aparato y a /board.
function translate(err: unknown, key: string): AppsLlmError {
  if (err instanceof AppsLlmError) return err;
  if (err instanceof Anthropic.AuthenticationError) return new AppsLlmError("la clave de las apps no es válida", "no_key");
  if (err instanceof Anthropic.APIError) {
    return new AppsLlmError(`Anthropic ${err.status ?? ""}: ${redactSecrets(err.message, key).slice(0, 160)}`.trim(), "provider_error");
  }
  const msg = err instanceof Error ? err.message : String(err);
  return new AppsLlmError(redactSecrets(msg, key).slice(0, 160), "provider_error");
}

function textOf(message: Anthropic.Message): string {
  let text = "";
  for (const block of message.content) if (block.type === "text") text += block.text;
  return text;
}

export type AppsJsonOptions = {
  system: string;
  user: string;
  schema: object;
  maxTokens?: number;
  // Para contarlo en `usage` (columna `apps_calls`); sin cuenta no se cuenta.
  accountId?: number;
};

/** Una llamada con salida estructurada: devuelve el JSON ya parseado. */
export async function appsJson<T>(o: AppsJsonOptions): Promise<T> {
  const { client, model, key } = await clientFor(JSON_TIMEOUT_MS, 0);
  let res: Anthropic.Message;
  try {
    res = await client.messages.create({
      model,
      max_tokens: Math.max(256, Math.min(8000, o.maxTokens ?? 2048)),
      thinking: { type: "adaptive" },
      system: o.system,
      messages: [{ role: "user", content: o.user }],
      // Salida estructurada nativa: el esquema es un JSON Schema común (sin
      // minItems/maxLength, que la API no acepta: los topes se verifican acá).
      output_config: { format: { type: "json_schema", schema: o.schema as Record<string, unknown> } },
    });
  } catch (err) {
    throw translate(err, key);
  }
  if (o.accountId !== undefined) void addUsage(o.accountId, { apps: 1 });
  if (res.stop_reason === "refusal") throw new AppsLlmError("el modelo se negó a responder", "refused");
  const raw = textOf(res).trim();
  const start = raw.indexOf("{");
  const end = raw.lastIndexOf("}");
  if (start < 0 || end <= start) throw new AppsLlmError(`el modelo no devolvió JSON: ${raw.slice(0, 120)}`, "bad_answer");
  try {
    return JSON.parse(raw.slice(start, end + 1)) as T;
  } catch (err) {
    throw new AppsLlmError(`JSON inválido del modelo: ${String(err).slice(0, 120)}`, "bad_answer");
  }
}

export type AppsProseOptions = {
  // Como lista de bloques cuando hay un prefijo estable para cachear (el
  // índice del librito se repite en cada capítulo: va con cache_control).
  system: string | Anthropic.TextBlockParam[];
  user: string;
  maxTokens: number;
  onProgress?: (chars: number) => void;
  accountId?: number;
};

/** Prosa larga con streaming. Devuelve el texto concatenado. */
export async function appsProse(o: AppsProseOptions): Promise<string> {
  const { client, model, key } = await clientFor(PROSE_TIMEOUT_MS, 1);
  let chars = 0;
  let message: Anthropic.Message;
  try {
    const stream = client.messages.stream({
      model,
      max_tokens: Math.max(1024, Math.min(32_000, Math.round(o.maxTokens))),
      thinking: { type: "adaptive" },
      system: o.system,
      messages: [{ role: "user", content: o.user }],
    });
    if (o.onProgress) {
      stream.on("text", (delta) => {
        chars += delta.length;
        o.onProgress?.(chars);
      });
    }
    message = await stream.finalMessage();
  } catch (err) {
    throw translate(err, key);
  }
  if (o.accountId !== undefined) void addUsage(o.accountId, { apps: 1 });
  if (message.stop_reason === "refusal") throw new AppsLlmError("el modelo se negó a escribir", "refused");
  // Cortado por max_tokens no es un error: el capítulo queda un poco más
  // corto y se dice en el log. Volver a pedirlo costaría lo mismo otra vez.
  if (message.stop_reason === "max_tokens") console.warn(`apps: prosa cortada por max_tokens (${o.maxTokens})`);
  return textOf(message);
}

/** Para mostrar en /board y en el log qué modelo escribe. */
export async function appsModelLabel(): Promise<string> {
  const c = (await config()).apps;
  return `anthropic/${c.model}${c.key ? "" : " (sin clave)"}`;
}
