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
import { searchWeb, asksForSearch, asksForReasoning, formatResults } from "./websearch";
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
// Anthropic decide el modelo, con las compatibles decide asksForSearch);
// "force" busca sí o sí (el clasificador de voz ya dijo que hace falta).
type Options = {
  system: string;
  user: string;
  maxTokens?: number;
  cached?: string;
  search?: "off" | "auto" | "force";
  reason?: boolean;  // el usuario pidio expresamente que razone
  lang?: Lang;
  // `memories`: lo que el usuario pidió que el aparato recuerde de él. Es el
  // bloque MÁS estable de todos (cambia solo cuando dicta una memoria nueva),
  // así que va primero y cacheado: en Anthropic con cache_control, en las
  // compatibles con OpenAI al principio del system, que es donde pega la caché
  // de prefijo del proveedor. Ver store.ts (memoryLines / rememberFact).
  memories?: string[];
};

// El bloque de memoria como texto. Se arma igual para los dos caminos para que
// el prefijo cacheado sea byte a byte el mismo entre consultas.
export function memoryBlock(memories: string[] | undefined): string {
  const list = (memories ?? []).map((m) => m.trim()).filter(Boolean);
  if (!list.length) return "";
  return [
    "Lo que sabes de esta persona (te lo pidió recordar):",
    ...list.map((m) => `- ${m}`),
    "Úsalo cuando venga al caso y no lo menciones si no hace falta. Si algo de acá contradice lo que dice ahora, manda lo que dice ahora.",
  ].join("\n");
}

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

// ── Búsqueda incorporada del proveedor ──────────────────────────────────────
// Groq también la trae, y sale MUCHO más barata que la de Anthropic (va adentro
// del precio de los tokens, no USD 0,01 por búsqueda):
//   groq/compound y groq/compound-mini  → sistemas agénticos: buscan solos, no
//       hay que declarar nada. Las fuentes vuelven en
//       choices[0].message.executed_tools[].search_results.results[].
//   openai/gpt-oss-*  → herramienta propia: tools: [{ type: "browser_search" }].
//       Las citas vienen incrustadas en el texto como 【2†L6-L10】 y hay que
//       sacarlas antes de mandarlas a una pantalla de tinta.
// Cualquier otro compatible con OpenAI (DeepSeek, OpenAI) no tiene nada: ahí
// busca el servidor (websearch.ts) y los resultados van en el prompt.
export type BuiltInSearch = "compound" | "browser" | "none";

export function providerSearchKind(baseUrl: string, model: string): BuiltInSearch {
  let host = "";
  try {
    host = new URL(baseUrl).hostname.toLowerCase();
  } catch {
    return "none";
  }
  if (!host.endsWith("groq.com")) return "none";
  if (/^groq\/compound/.test(model)) return "compound";
  if (/^openai\/gpt-oss/.test(model)) return "browser";
  return "none";
}

// País que Groq usa para dar más peso a los resultados locales.
const SEARCH_COUNTRY: Record<Lang, string> = {
  es: "argentina", en: "united states", fr: "france", de: "germany", pt: "brazil", ru: "russia",
};

// gpt-oss deja las citas incrustadas en el texto ("...32 % share 【2†L6-L10】").
// En la pantalla del aparato eso es basura.
function stripInlineCitations(text: string): string {
  return text
    .replace(/【[^】]*】/g, "")
    .replace(/\[\d+\u2020[^\]]*\]/g, "")
    .replace(/[ \t]{2,}/g, " ")
    .replace(/ +([.,;:!?])/g, "$1")
    .trim();
}

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

// Para mostrar en /board qué búsqueda le toca a lo que está elegido.
export function searchToolLabel(model: string): string {
  return webSearchTool(model, 1).type;
}

// Lo mismo pero contando el proveedor entero, en castellano, para la tarjeta de IA.
export async function searchKindLabel(): Promise<string> {
  const c = (await config()).llm;
  if (c.provider === "anthropic") return `Anthropic, del lado del servidor (${searchToolLabel(c.model)}), USD 0,01 por búsqueda`;
  const kind = providerSearchKind(c.baseUrl, c.model);
  if (kind === "compound") return "Groq Compound: busca solo, incluida en el precio de los tokens";
  if (kind === "browser") return "Groq browser_search del gpt-oss: incluida en el precio de los tokens";
  return "la hace este servidor (Google Noticias + DuckDuckGo, o Tavily/Brave con clave)";
}

export async function currentModel(): Promise<string> {
  return (await config()).llm.model;
}

// El system de Anthropic como lista de bloques, del más estable al más volátil:
// memoria del usuario, instrucciones, y el bloque grande (el capítulo del
// libro). El cache_control va en el último de los estables: Anthropic cachea
// TODO el prefijo hasta ahí, así que una sola marca alcanza para los dos.
function systemBlocks(o: Options): string | Anthropic.TextBlockParam[] {
  const mem = memoryBlock(o.memories);
  if (!mem && !o.cached) return o.system;
  const blocks: Anthropic.TextBlockParam[] = [];
  if (mem) blocks.push({ type: "text", text: mem });
  blocks.push({ type: "text", text: o.system });
  if (o.cached) blocks.push({ type: "text", text: o.cached });
  blocks[blocks.length - 1].cache_control = { type: "ephemeral" };
  return blocks;
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
      system: systemBlocks(o),
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

type OpenAiChoice = {
  message?: {
    content?: string;
    // Groq: lo que ejecutó el sistema agéntico (búsqueda, código).
    executed_tools?: { type?: string; search_results?: { results?: { title?: string; url?: string; content?: string; score?: number }[] } }[];
  };
};

async function openAiRun(o: Options, schema: object | undefined, builtIn: BuiltInSearch): Promise<ChatResult> {
  const cfg = await config();
  const c = cfg.llm;
  if (!c.key) throw new LlmError("falta la clave del proveedor (web → Ajustes)", 500, "no_key");
  // La memoria va PRIMERA: DeepSeek y los demás cachean por prefijo exacto, así
  // que lo estable tiene que estar al principio o no se cachea nada.
  const mem = memoryBlock(o.memories);
  const base = [mem, o.system, o.cached].filter(Boolean).join("\n\n");
  const system = schema
    ? `${base}\n\nRespondé SOLO con un objeto JSON que cumpla este esquema, sin texto alrededor y sin bloques de código:\n${JSON.stringify(schema)}`
    : base;
  // Se revalida acá y no solo al guardar: un config.json viejo (o editado a
  // mano) no tiene que poder mandar el Bearer a la red interna.
  const url = checkUrl(c.baseUrl, { allowLocal: true });
  if (!url.ok) throw new LlmError(`la URL del proveedor no sirve: ${url.error}`, 500, "no_key");
  const lang: Lang = o.lang ?? "es";
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
      // El esquema y la búsqueda incorporada no conviven (Groq lo dice de
      // browser_search), pero tampoco hace falta: el clasificador nunca busca.
      // Los sistemas compound tampoco aceptan response_format, así que ahí el
      // esquema queda solo en el system (que ya lo explica entero).
      ...(schema && !/^groq\/compound/.test(c.model) ? { response_format: { type: "json_object" } } : {}),
      ...(builtIn === "compound"
        ? { search_settings: { country: SEARCH_COUNTRY[lang] } }
        : {}),
      // Razonar solo cuando el usuario lo pidio: DeepSeek v4 y los gpt-oss
      // piensan por defecto y eso son varios segundos mas de "Pensando...".
      ...(builtIn === "none" ? { reasoning_effort: o.reason ? "high" : "low" } : {}),
      ...(builtIn === "browser"
        ? {
            tools: [{ type: "browser_search" }],
            tool_choice: o.search === "force" ? "required" : "auto",
            // Con esfuerzo alto se pone a navegar de más y se come los tokens.
            reasoning_effort: o.reason ? "high" : "low",
          }
        : {}),
    }),
    signal: AbortSignal.timeout(builtIn === "none" ? 60_000 : 90_000),
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
  let data: { choices?: OpenAiChoice[] };
  try {
    data = JSON.parse(body);
  } catch {
    throw new LlmError(`respuesta ilegible del proveedor: ${body.slice(0, 120)}`, 502, "bad_answer");
  }
  const msg = data.choices?.[0]?.message;
  let text = msg?.content ?? "";
  const sources: Source[] = [];
  for (const t of msg?.executed_tools ?? []) {
    for (const r of t.search_results?.results ?? []) {
      if (r.url) sources.push({ title: (r.title ?? "").slice(0, 160), url: r.url });
    }
  }
  if (builtIn === "browser") text = stripInlineCitations(text);
  const searched = builtIn !== "none" && (sources.length > 0 || (builtIn === "browser" && /【|\u2020/.test(msg?.content ?? "")));
  return { text, sources: sources.slice(0, 8), searched };
}

async function openAiText(o: Options, schema?: object): Promise<string> {
  return (await openAiRun(o, schema, "none")).text;
}

export async function chatText(o: Options): Promise<string> {
  return (await chatSearch(o)).text;
}

// Igual que chatText pero contando qué se buscó y de dónde salió, para poder
// mostrar las fuentes en el aparato.
export async function chatSearch(o: Options): Promise<ChatResult> {
  const cfg = await config();
  const lang0: Lang = o.lang ?? "es";
  // 1.5.41: buscar SOLO si el usuario lo pidió expresamente ("busca...").
  // "force" lo pide quien llama; "auto" ya no adivina: se mira la frase.
  const asked = o.search === "force" || (o.search === "auto" && asksForSearch(o.user, lang0));
  const on = asked && cfg.search.enabled;
  // Idem razonar: solo si lo dijo. Cuesta tiempo y el aparato ya tarda.
  const reason = asksForReasoning(o.user, lang0);
  // Anthropic busca solo: se le declara la herramienta y él decide si la usa,
  // así que no se gasta una búsqueda en una pregunta que no la necesita.
  if (cfg.llm.provider === "anthropic") return anthropicRun(o, undefined, on);

  // Groq sí la trae: se la deja hacer a él (sale lo mismo que los tokens y
  // llega mucho mejor que rasguñar DuckDuckGo desde acá).
  const builtIn = on ? providerSearchKind(cfg.llm.baseUrl, cfg.llm.model) : "none";
  if (builtIn !== "none") {
    const r = await openAiRun({ ...o, reason }, undefined, builtIn);
    // Si el sistema decidió no buscar, la respuesta igual sirve: es la misma
    // llamada, no se gastó nada de más.
    return r;
  }

  // El resto (DeepSeek, OpenAI) no tiene nada parecido: busca el servidor y los
  // resultados van adentro del prompt.
  const lang: Lang = o.lang ?? "es";
  let opts = o;
  let sources: Source[] = [];
  let searched = false;
  let searchNote: string | undefined;
  if (on) {
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
