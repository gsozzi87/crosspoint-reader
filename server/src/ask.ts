// "Preguntarle al libro": el aparato manda el texto del capítulo que está
// leyendo más una pregunta; acá lo responde Claude. La API key de Anthropic
// vive solo en Railway (ANTHROPIC_API_KEY): el lector nunca la ve.
//
//   POST /api/ask   (Bearer del aparato, lo chequea api.ts)
//   body: { book, chapter, text, page?, question, lang? }
//     Sin text (ni book): modo general, el mosaico "Preguntar" del hub. La
//     pregunta se responde con conocimiento general, sin libro de por medio.
//     text     = texto plano del capítulo leído hasta acá (el aparato manda las
//                últimas páginas, ~24 KB como mucho)
//     page     = texto de la página en la que está el lector (opcional)
//     question = pregunta del lector (ya transcripta si vino por voz)
//     lang     = "es" (default) | "en"
//   200: { ok: true, answer, model, usage: { input, output, cached } }
//   4xx/5xx: { ok: false, error }
//
// Modelo: claude-haiku-4-5 por defecto (el más barato: $1 / $5 por millón de
// tokens). ASK_MODEL lo cambia; el pedido no usa parámetros específicos de un
// modelo, así que cualquier ID actual sirve tal cual.
//
// El capítulo va en el system prompt con cache_control: las preguntas
// sucesivas sobre el mismo capítulo reusan el prefijo cacheado (~90 % menos
// tokens de entrada). La pregunta y la página actual van en el mensaje del
// usuario, después.
import { Hono } from "hono";
import Anthropic from "@anthropic-ai/sdk";
import { LANGUAGE_NAME, normalizeLang } from "./lang";
import { load } from "./store";

const MODEL = process.env.ASK_MODEL ?? "claude-haiku-4-5";
const MAX_TEXT = 32_000; // chars; el aparato recorta antes, esto es defensa
const MAX_PAGE = 8_000;
const MAX_QUESTION = 500;

const client = new Anthropic();

export const ask = new Hono();

function systemPrompt(book: string, chapter: string, lang: string): string {
  const language = LANGUAGE_NAME[normalizeLang(lang)];
  return [
    "Sos un compañero de lectura dentro de un lector de libros electrónico de tinta electrónica.",
    `El usuario está leyendo "${book}"${chapter ? `, capítulo "${chapter}"` : ""}.`,
    "Primero respondé con lo que aparece en el texto adjunto (lo que el lector ya leyó). Si el texto no",
    "alcanza, respondé igual con conocimiento general en dos o tres frases y aclará en una línea que el",
    "libro todavía no lo trató. Nunca adelantes nada de lo que pasa después en la obra aunque la conozcas",
    "(sin spoilers): si la pregunta es sobre la trama futura, decí que todavía no llegó a esa parte.",
    "La pregunta llega transcripta de voz: puede traer errores de reconocimiento; interpretala con",
    "sentido común y no comentes la transcripción.",
    `Idioma: ${language}. Texto plano, sin markdown, sin títulos ni listas con viñetas.`,
    "La pantalla es chica: máximo 120 palabras salvo que el lector pida algo más largo.",
  ].join(" ");
}

function generalPrompt(lang: string, memories: string[]): string {
  const language = LANGUAGE_NAME[normalizeLang(lang)];
  return [
    memories.length ? `Cosas que el usuario te pidió que recuerdes: ${memories.map((m) => `«${m}»`).join(" ")}` : "",
    "Sos el asistente por voz de un lector de libros electrónico de tinta electrónica.",
    "Respondé la pregunta de forma directa y útil con conocimiento general.",
    "La pregunta llega transcripta de voz: puede traer errores de reconocimiento; interpretala con",
    "sentido común y no comentes la transcripción.",
    `Idioma: ${language}. Texto plano, sin markdown, sin títulos ni listas con viñetas.`,
    "La pantalla es chica: máximo 120 palabras salvo que el usuario pida algo más largo.",
  ].join(" ");
}

ask.post("/", async (c) => {
  let body: { book?: string; chapter?: string; text?: string; page?: string; question?: string; lang?: string };
  try {
    body = await c.req.json();
  } catch {
    return c.json({ ok: false, error: "invalid json" }, 400);
  }
  const book = (body.book ?? "").toString().slice(0, 200);
  const chapter = (body.chapter ?? "").toString().slice(0, 200);
  const text = (body.text ?? "").toString().slice(0, MAX_TEXT);
  const page = (body.page ?? "").toString().slice(0, MAX_PAGE);
  const question = (body.question ?? "").toString().trim().slice(0, MAX_QUESTION);
  const lang = normalizeLang(body.lang);
  if (!question) return c.json({ ok: false, error: "question is required" }, 400);
  const general = !text;

  try {
    const response = await client.messages.create({
      model: MODEL,
      max_tokens: 1024,
      system: general
        ? generalPrompt(lang, ((await load()).memories ?? []).slice(-40).map((m) => m.text))
        : [
            { type: "text", text: systemPrompt(book, chapter, lang) },
            {
              type: "text",
              text: `<leido_hasta_aca>\n${text}\n</leido_hasta_aca>`,
              cache_control: { type: "ephemeral" },
            },
          ],
      messages: [
        {
          role: "user",
          content: page && !general
            ? `El lector está en esta página:\n<pagina>\n${page}\n</pagina>\n\nPregunta: ${question}`
            : question,
        },
      ],
    });

    if (response.stop_reason === "refusal") {
      return c.json({ ok: false, error: "refused" }, 422);
    }
    let answer = "";
    for (const block of response.content) {
      if (block.type === "text") answer += block.text;
    }
    answer = answer.trim();
    return c.json({
      ok: true,
      answer,
      model: response.model,
      usage: {
        input: response.usage.input_tokens,
        output: response.usage.output_tokens,
        cached: response.usage.cache_read_input_tokens ?? 0,
      },
    });
  } catch (err) {
    if (err instanceof Anthropic.RateLimitError) return c.json({ ok: false, error: "rate limited" }, 429);
    if (err instanceof Anthropic.AuthenticationError) return c.json({ ok: false, error: "bad ANTHROPIC_API_KEY" }, 500);
    if (err instanceof Anthropic.APIError) return c.json({ ok: false, error: `claude ${err.status}: ${err.message}` }, 502);
    console.error("ask:", err);
    return c.json({ ok: false, error: "internal" }, 500);
  }
});
