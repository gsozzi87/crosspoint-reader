// Traductor en modo conversación (mosaico Traductor del aparato).
//
//   POST /api/translate?from=xx&to=yy   (Bearer del aparato; body audio/wav)
//   200: cuerpo binario "application/x-ws397-voice" (mismo marco que /api/voice):
//        [uint32 LE largo JSON][JSON][ADPCM]
//        JSON = { ok: true, text, translation, from, to, audio: bytes }
//        El ADPCM es la traducción dicha por Piper en el idioma `to`.
//   4xx/5xx: { ok: false, error }
//
// Transcribe en `from`, traduce con el proveedor elegido en /board -> Ajustes
// (el mismo que usa todo lo demás) y sintetiza en `to`.
import { Hono } from "hono";
import { transcribeWav, toWav, NoSpeechError, NO_SPEECH, NO_SPEECH_MSG } from "./transcribe";
import { synthesize } from "./tts";
import { LANGUAGE_NAME, normalizeLang } from "./lang";
import { chatText, LlmError } from "./llm";
import { redactSecrets } from "./net";
import type { ContentfulStatusCode } from "hono/utils/http-status";

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
    text = await transcribeWav(toWav(await c.req.arrayBuffer(), c.req.header("content-type")), from);
  } catch (err) {
    // Silencio: se avisa en el idioma del que habla y no se traduce nada
    // (traducir "Gracias por ver el video" no le sirve a nadie).
    if (err instanceof NoSpeechError) {
      const reply = NO_SPEECH_MSG[from];
      const audio = await synthesize(reply, from, 6);
      return framed({ ok: true, text: "", translation: reply, from, to, code: NO_SPEECH, audio: audio?.length ?? 0 }, audio);
    }
    const msg = err instanceof Error ? err.message : "internal";
    return c.json({ ok: false, error: msg }, msg.startsWith("stt ") ? 502 : 400);
  }
  try {
    // Antes acá había un cliente de Anthropic propio con la clave del entorno:
    // el traductor le seguía pegando a Claude aunque en /board estuviera elegido
    // Groq o DeepSeek, y fallaba con "bad ANTHROPIC_API_KEY" sin explicar nada.
    const translation = (
      await chatText({
        system: [
          `Sos un traductor de conversación. Traducí del ${LANGUAGE_NAME[from]} al ${LANGUAGE_NAME[to]} lo que dice el usuario,`,
          "tal cual, con el mismo registro y sin agregar nada: ni comentarios, ni comillas, ni explicaciones.",
          "El texto llega transcripto de voz y puede traer errores de reconocimiento: interpretalo con sentido común.",
          "Respondé solo con la traducción, en texto plano.",
        ].join(" "),
        user: text,
        maxTokens: 400,
      })
    ).trim();
    if (!translation) return c.json({ ok: false, error: "empty translation", code: "bad_answer" }, 502);
    console.log(`translate ${from}->${to}: "${text}" -> "${translation}"`);
    const audio = await synthesize(translation, to, 12);
    return framed({ ok: true, text, translation, from, to, audio: audio?.length ?? 0 }, audio);
  } catch (err) {
    if (err instanceof LlmError) {
      console.error("translate llm:", err.message);
      return c.json({ ok: false, error: err.message, code: err.code }, err.status as ContentfulStatusCode);
    }
    console.error("translate:", err);
    return c.json({ ok: false, error: redactSecrets(String(err)).slice(0, 200), code: "internal" }, 500);
  }
});
