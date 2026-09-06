// Traductor en modo conversación (mosaico Traductor del aparato).
//
//   POST /api/translate?from=xx&to=yy   (Bearer del aparato; body audio/wav)
//   200: cuerpo binario "application/x-ws397-voice" (mismo marco que /api/voice):
//        [uint32 LE largo JSON][JSON][ADPCM]
//        JSON = { ok: true, text, translation, from, to, audio: bytes }
//        El ADPCM es la traducción dicha por Piper en el idioma `to`.
//   4xx/5xx: { ok: false, error }
//
// Transcribe en `from`, traduce con Claude (solo la traducción, sin comentarios)
// y sintetiza en `to`. Modelo: VOICE_MODEL / ASK_MODEL, default claude-haiku-4-5.
import { Hono } from "hono";
import Anthropic from "@anthropic-ai/sdk";
import { transcribeWav } from "./transcribe";
import { synthesize } from "./tts";
import { LANGUAGE_NAME, normalizeLang } from "./lang";

const MODEL = process.env.VOICE_MODEL ?? process.env.ASK_MODEL ?? "claude-haiku-4-5";
const client = new Anthropic();

export const translate = new Hono();

function framed(json: object, audio: Uint8Array | null): Response {
  const head = Buffer.from(JSON.stringify(json), "utf8");
  const len = Buffer.alloc(4);
  len.writeUInt32LE(head.length, 0);
  const body = Buffer.concat([len, head, audio ? Buffer.from(audio) : Buffer.alloc(0)]);
  return new Response(body, { headers: { "Content-Type": "application/x-ws397-voice", "Content-Length": String(body.length) } });
}

translate.post("/", async (c) => {
  const from = normalizeLang(c.req.query("from"));
  const to = normalizeLang(c.req.query("to"));
  let text: string;
  try {
    text = await transcribeWav(await c.req.arrayBuffer(), from);
  } catch (err) {
    const msg = err instanceof Error ? err.message : "internal";
    return c.json({ ok: false, error: msg }, msg.startsWith("stt ") ? 502 : 400);
  }
  try {
    const response = await client.messages.create({
      model: MODEL,
      max_tokens: 400,
      system: [
        `Sos un traductor de conversación. Traducí del ${LANGUAGE_NAME[from]} al ${LANGUAGE_NAME[to]} lo que dice el usuario,`,
        "tal cual, con el mismo registro y sin agregar nada: ni comentarios, ni comillas, ni explicaciones.",
        "El texto llega transcripto de voz y puede traer errores de reconocimiento: interpretalo con sentido común.",
        "Respondé solo con la traducción, en texto plano.",
      ].join(" "),
      messages: [{ role: "user", content: text }],
    });
    if (response.stop_reason === "refusal") return c.json({ ok: false, error: "refused" }, 422);
    let translation = "";
    for (const block of response.content) if (block.type === "text") translation += block.text;
    translation = translation.trim();
    if (!translation) return c.json({ ok: false, error: "empty translation" }, 502);
    console.log(`translate ${from}->${to}: "${text}" -> "${translation}"`);
    const audio = await synthesize(translation, to, 12);
    return framed({ ok: true, text, translation, from, to, audio: audio?.length ?? 0 }, audio);
  } catch (err) {
    if (err instanceof Anthropic.RateLimitError) return c.json({ ok: false, error: "rate limited" }, 429);
    if (err instanceof Anthropic.AuthenticationError) return c.json({ ok: false, error: "bad ANTHROPIC_API_KEY" }, 500);
    if (err instanceof Anthropic.APIError) return c.json({ ok: false, error: `claude ${err.status}: ${err.message}` }, 502);
    console.error("translate:", err);
    return c.json({ ok: false, error: "internal" }, 500);
  }
});
