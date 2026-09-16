import { Hono } from "hono";
import type { AppEnv } from "./tenant";
import { chatSearch } from "./llm";
import { researchEpub } from "./epub";
import { limitBody, readBody } from "./net";
import { normalizeLang } from "./lang";

export const research = new Hono<AppEnv>();

research.post("/epub", limitBody(16 * 1024), async (c) => {
  const body = await readBody(c);
  const topic = String(body.topic ?? "").trim().slice(0, 180);
  const lang = normalizeLang(body.lang);
  if (topic.length < 3) return c.json({ ok: false, error: "falta el tema" }, 400);
  try {
    const result = await chatSearch({
      lang,
      search: "force",
      maxTokens: 5000,
      system: [
        "Eres un investigador y editor de libros breves.",
        "Escribe en español internacional neutro cuando el idioma sea español; evita regionalismos, voseo y giros argentinos.",
        "Organiza la explicación con títulos claros, contexto, datos verificables, distintos puntos de vista y una conclusión.",
        "No inventes fuentes ni hechos. No uses tablas, porque el resultado se leerá en tinta electrónica.",
      ].join(" "),
      user: `Investiga a fondo este tema y redacta un libro breve y autosuficiente: ${topic}`,
    });
    const epub = researchEpub(topic, result.text, result.sources, lang);
    return new Response(epub, {
      status: 200,
      headers: {
        "Content-Type": "application/epub+zip",
        "Content-Length": String(epub.byteLength),
        "Cache-Control": "no-store",
      },
    });
  } catch (err) {
    console.error("research epub:", err);
    return c.json({ ok: false, error: err instanceof Error ? err.message : "internal" }, 502);
  }
});
