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
import { searchWeb, needsFreshInfo, formatResults } from "./websearch";
import type { Lang } from "./lang";

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
// `search`: "off" no busca nunca; "auto" deja que se busque si hace falta (con
// Anthropic decide el modelo, con las compatibles decide needsFreshInfo);
// "force" busca sí o sí (el clasificador de voz ya dijo que hace falta).
type Options = {
  system: string;
  user: string;
  maxTokens?: number;
  cached?: string;
  search?: "off" | "auto" | "force";
  lang?: Lang;
};

// `source`: el nombre del medio cuando la dirección es la de un intermediario
// (Google Noticias redirige, así que el dominio de la URL no es la fuente).
export type Source = { title: string; url: string; source?: string };
export type ChatResult = { text: string; sources: Source[]; searched: boolean; searchNote?: string };

// Instrucción que acompaña a los resultados cuando busca el servidor.
const SEARCH_HINT = [
  "Arriba del mensaje tenés resultados de una búsqueda en internet hecha recién.",
  "Usalos para todo lo que dependa de la fecha: son más nuevos que lo que sabés de memoria y le ganan a tu memoria.",
  "No inventes nada que no esté ahí; si los resultados no alcanzan para contestar, decilo en una línea.",
].join(" ");

// La búsqueda de Anthropic es una herramienta de su propio servidor: se declara
// y la ejecuta él, no hay que hacer ningún ciclo de llamadas acá. La variante
// nueva solo existe en los modelos nuevos; para el resto (el default del
// proyecto es claude-haiku-4-5) va la básica.
type SearchTool = Anthropic.Messages.WebSearchTool20250305 | Anthropic.Messages.WebSearchTool20260209;

function webSearchTool(model: string, maxUses: number): SearchTool {
  const modern = /(opus-5|sonnet-5|opus-4-8|opus-4-7|opus-4-6|sonnet-4-6)/.test(model);
  return modern
    ? { type: "web_search_20260209", name: "web_search", max_uses: maxUses }
    : { type: "web_search_20250305", name: "web_search", max_uses: maxUses };
}

// Para mostrar en /board cuál de las dos variantes le toca al modelo elegido.
export function searchToolLabel(model: string): string {
  return webSearchTool(model, 1).type;
}

export async function currentModel(): Promise<string> {
  return (await config()).llm.model;
}

async function anthropicRun(o: Options, schema: object | undefined, search: boolean): Promise<ChatResult> {
  const cfg = await config();
  const c = cfg.llm;
  if (!c.key) throw new LlmError("falta la clave del proveedor (web → Ajustes)", 500, "no_key");
  // Sin timeout el SDK espera ~10 minutos y el aparato queda colgado en
  // "Pensando..." hasta que se le acaba la paciencia (o la batería).
  const client = new Anthropic({ apiKey: c.key, timeout: 90_000, maxRetries: 1 });
  const tools = search ? [webSearchTool(c.model, cfg.search.maxUses)] : undefined;
  const messages: Anthropic.MessageParam[] = [{ role: "user", content: o.user }];
  let text = "";
  const sources: Source[] = [];
  let searched = false;
  let searchNote: string | undefined;
  // Con la búsqueda puesta la respuesta puede venir en varios tramos:
  // stop_reason "pause_turn" quiere decir "seguí desde acá".
  for (let turn = 0; turn < 4; turn++) {
    const res = await client.messages.create({
      model: c.model,
      max_tokens: o.maxTokens ?? 1024,
      system: o.cached
        ? [
            { type: "text" as const, text: o.system },
            { type: "text" as const, text: o.cached, cache_control: { type: "ephemeral" as const } },
          ]
        : o.system,
      messages,
      ...(tools ? { tools } : {}),
      // El SDK tipa el esquema con su propia forma; el nuestro es un JSON Schema
      // común, así que se pasa tal cual.
      ...(schema ? ({ output_config: { format: { type: "json_schema", schema } } } as never) : {}),
    });
    if (res.stop_reason === "refusal") throw new LlmError("el modelo se negó a responder", 400, "refused");
    for (const block of res.content) {
      if (block.type === "text") text += block.text;
      else if (block.type === "web_search_tool_result") {
        // .content es la lista de resultados cuando salió bien y un objeto de
        // error cuando no (viene con HTTP 200, sin excepción): hay que mirar
        // cuál de las dos cosas es ANTES de recorrerla.
        const content = block.content;
        if (Array.isArray(content)) {
          searched = true;
          for (const r of content) if (r.type === "web_search_result") sources.push({ title: r.title, url: r.url });
        } else {
          searchNote = content.error_code;
          console.error("búsqueda web:", content.error_code);
        }
      }
    }
    if (res.stop_reason !== "pause_turn") break;
    messages.push({ role: "assistant", content: res.content as unknown as Anthropic.ContentBlockParam[] });
  }
  return { text, sources, searched, searchNote };
}

async function anthropicText(o: Options, schema?: object): Promise<string> {
  return (await anthropicRun(o, schema, false)).text;
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
  return (await chatSearch(o)).text;
}

// Igual que chatText pero contando qué se buscó y de dónde salió, para poder
// mostrar las fuentes en el aparato.
export async function chatSearch(o: Options): Promise<ChatResult> {
  const cfg = await config();
  const on = (o.search ?? "off") !== "off" && cfg.search.enabled;
  // Anthropic busca solo: se le declara la herramienta y él decide si la usa,
  // así que no se gasta una búsqueda en una pregunta que no la necesita.
  if (cfg.llm.provider === "anthropic") return anthropicRun(o, undefined, on);

  // Las APIs compatibles con OpenAI no tienen nada parecido: busca el servidor y
  // los resultados van adentro del prompt.
  const lang: Lang = o.lang ?? "es";
  let opts = o;
  let sources: Source[] = [];
  let searched = false;
  let searchNote: string | undefined;
  if (on && (o.search === "force" || needsFreshInfo(o.user, lang))) {
    const found = await searchWeb(o.user, lang, 5);
    if (found.results.length) {
      searched = true;
      sources = found.results.map((r) => ({ title: r.title, url: r.url, source: r.source }));
      opts = {
        ...o,
        system: `${o.system} ${SEARCH_HINT}`,
        user: `${formatResults(o.user, found.results)}\n\n${o.user}`,
      };
    } else {
      searchNote = found.error;
    }
  }
  return { text: await openAiText(opts), sources, searched, searchNote };
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
