// Un solo cliente para el modelo de texto, con el proveedor elegido desde la web
// (/board → Ajustes). Anthropic (Claude) o cualquier API compatible con OpenAI:
// Groq (gratis y muy rápido), DeepSeek (muy barato), OpenAI.
//
// Dos formas de uso, las únicas que necesita el proyecto:
//   chatText()  → una respuesta en texto plano (preguntar al libro, la hora)
//   chatJson()  → una respuesta que cumple un JSON Schema (clasificador de voz)
//
// En Anthropic el esquema va en output_config (salida estructurada nativa). En
// las APIs compatibles se pide response_format json_object y el esquema se
// explica en el system prompt: no todas soportan json_schema, y el objeto suelto
// más el esquema escrito alcanza para lo que pedimos.
import Anthropic from "@anthropic-ai/sdk";
import { config } from "./config";
import { checkUrl, redactSecrets } from "./net";

// `code` es para el aparato: "no_key" se muestra distinto que un fallo del
// proveedor, aunque los dos lleguen como texto.
export type LlmCode = "no_key" | "provider_error" | "refused" | "bad_answer";

export class LlmError extends Error {
  constructor(message: string, readonly status = 502, readonly code: LlmCode = "provider_error") {
    super(message);
  }
}

// `cached` es un bloque grande y estable (el capítulo del libro): en Anthropic va
// como segundo bloque de system con cache_control y las preguntas siguientes
// sobre el mismo capítulo casi no pagan entrada. En las APIs compatibles se pega
// al system y listo.
type Options = { system: string; user: string; maxTokens?: number; cached?: string };

export async function currentModel(): Promise<string> {
  return (await config()).llm.model;
}

async function anthropicText(o: Options, schema?: object): Promise<string> {
  const c = (await config()).llm;
  if (!c.key) throw new LlmError("falta la clave del proveedor (web → Ajustes)", 500, "no_key");
  // Sin timeout el SDK espera ~10 minutos y el aparato queda colgado en
  // "Pensando..." hasta que se le acaba la paciencia (o la batería).
  const client = new Anthropic({ apiKey: c.key, timeout: 90_000, maxRetries: 1 });
  const res = await client.messages.create({
    model: c.model,
    max_tokens: o.maxTokens ?? 1024,
    system: o.cached
      ? [
          { type: "text" as const, text: o.system },
          { type: "text" as const, text: o.cached, cache_control: { type: "ephemeral" as const } },
        ]
      : o.system,
    messages: [{ role: "user", content: o.user }],
    // El SDK tipa el esquema con su propia forma; el nuestro es un JSON Schema
    // común, así que se pasa tal cual.
    ...(schema ? ({ output_config: { format: { type: "json_schema", schema } } } as never) : {}),
  });
  if (res.stop_reason === "refusal") throw new LlmError("el modelo se negó a responder", 400, "refused");
  let out = "";
  for (const block of res.content) if (block.type === "text") out += block.text;
  return out;
}

async function openAiText(o: Options, schema?: object): Promise<string> {
  const c = (await config()).llm;
  if (!c.key) throw new LlmError("falta la clave del proveedor (web → Ajustes)", 500, "no_key");
  const base = o.cached ? `${o.system}\n\n${o.cached}` : o.system;
  const system = schema
    ? `${base}\n\nRespondé SOLO con un objeto JSON que cumpla este esquema, sin texto alrededor y sin bloques de código:\n${JSON.stringify(schema)}`
    : base;
  // Se revalida acá y no solo al guardar: un config.json viejo (o editado a
  // mano) no tiene que poder mandar el Bearer a la red interna.
  const url = checkUrl(c.baseUrl, { allowLocal: true });
  if (!url.ok) throw new LlmError(`la URL del proveedor no sirve: ${url.error}`, 500, "no_key");
  const res = await fetch(`${c.baseUrl.replace(/\/+$/, "")}/chat/completions`, {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${c.key}` },
    body: JSON.stringify({
      model: c.model,
      max_tokens: o.maxTokens ?? 1024,
      messages: [
        { role: "system", content: system },
        { role: "user", content: o.user },
      ],
      ...(schema ? { response_format: { type: "json_object" } } : {}),
    }),
    signal: AbortSignal.timeout(60_000),
  });
  // El cuerpo del proveedor vuelve al aparato y a la web: algunos repiten la
  // clave que les mandaste en el error de auth, así que se tacha primero.
  const body = redactSecrets(await res.text(), c.key);
  if (!res.ok) {
    throw new LlmError(
      `${url.url.host} ${res.status}: ${body.slice(0, 200)}`,
      res.status === 401 ? 500 : 502,
      res.status === 401 ? "no_key" : "provider_error",
    );
  }
  let data: { choices?: { message?: { content?: string } }[] };
  try {
    data = JSON.parse(body);
  } catch {
    throw new LlmError(`respuesta ilegible del proveedor: ${body.slice(0, 120)}`, 502, "bad_answer");
  }
  return data.choices?.[0]?.message?.content ?? "";
}

export async function chatText(o: Options): Promise<string> {
  const c = (await config()).llm;
  return c.provider === "anthropic" ? anthropicText(o) : openAiText(o);
}

// Nombre del proveedor para mostrar (logs, /api/ask).
export async function providerLabel(): Promise<string> {
  const c = (await config()).llm;
  if (c.provider === "anthropic") return `anthropic/${c.model}`;
  const u = checkUrl(c.baseUrl, { allowLocal: true });
  return `${u.ok ? u.url.host : "?"}/${c.model}`;
}

export async function chatJson<T>(o: Options, schema: object): Promise<T> {
  const c = (await config()).llm;
  const raw = c.provider === "anthropic" ? await anthropicText(o, schema) : await openAiText(o, schema);
  // Algunos modelos igual encierran el JSON en ```json … ```
  const clean = raw.trim().replace(/^```(?:json)?/i, "").replace(/```$/, "").trim();
  const start = clean.indexOf("{");
  const end = clean.lastIndexOf("}");
  if (start < 0 || end <= start) throw new LlmError(`el modelo no devolvió JSON: ${clean.slice(0, 120)}`, 502, "bad_answer");
  try {
    return JSON.parse(clean.slice(start, end + 1)) as T;
  } catch (err) {
    throw new LlmError(`JSON inválido del modelo: ${String(err).slice(0, 120)}`, 502, "bad_answer");
  }
}
