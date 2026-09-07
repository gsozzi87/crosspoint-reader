// "Preguntarle al libro": el aparato manda el texto del capítulo que está
// leyendo más una pregunta y acá la responde el modelo configurado. Las claves
// viven solo en el servidor: el lector nunca las ve.
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
// Modelo: el elegido en /board -> Ajustes (Claude, Groq, DeepSeek o cualquier
// API compatible con OpenAI). Ver src/llm.ts y src/config.ts.
//
// El capítulo va en el system prompt con cache_control: las preguntas
// sucesivas sobre el mismo capítulo reusan el prefijo cacheado (~90 % menos
// tokens de entrada). La pregunta y la página actual van en el mensaje del
// usuario, después.
import { Hono } from "hono";
import Anthropic from "@anthropic-ai/sdk";
import { LANGUAGE_NAME, normalizeLang } from "./lang";
import { load } from "./store";
import { chatText, providerLabel, LlmError } from "./llm";

const MAX_TEXT = 32_000; // chars; el aparato recorta antes, esto es defensa
const MAX_PAGE = 8_000;
const MAX_QUESTION = 500;

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
    const answer = (
      await chatText({
        system: general
          ? generalPrompt(lang, ((await load()).memories ?? []).slice(-40).map((m) => m.text))
          : systemPrompt(book, chapter, lang),
        cached: general ? undefined : `<leido_hasta_aca>\n${text}\n</leido_hasta_aca>`,
        user:
          page && !general
            ? `El lector está en esta página:\n<pagina>\n${page}\n</pagina>\n\nPregunta: ${question}`
            : question,
        maxTokens: 1024,
      })
    ).trim();
    return c.json({ ok: true, answer, model: await providerLabel() });
  } catch (err) {
    if (err instanceof LlmError) {
      console.error("ask llm:", err.message);
      return c.json({ ok: false, error: err.message }, 502);
    }
    if (err instanceof Anthropic.RateLimitError) return c.json({ ok: false, error: "rate limited" }, 429);
    if (err instanceof Anthropic.AuthenticationError) return c.json({ ok: false, error: "falta o no sirve la clave del modelo" }, 500);
    if (err instanceof Anthropic.APIError) return c.json({ ok: false, error: `modelo ${err.status}: ${err.message}` }, 502);
    console.error("ask:", err);
    return c.json({ ok: false, error: String(err).slice(0, 200) }, 500);
  }
});
