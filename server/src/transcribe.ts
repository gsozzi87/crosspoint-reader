// Transcripción de voz para el aparato (no tiene teclado: toda pregunta entra
// por el mic). Recibe un WAV y devuelve el texto, usando cualquier servicio
// con la API de transcripción compatible con OpenAI:
//
//   STT_API_KEY   (o OPENAI_API_KEY)  → key del servicio
//   STT_BASE_URL  default https://api.openai.com/v1
//                 Groq (más barato / free tier): https://api.groq.com/openai/v1
//   STT_MODEL     default whisper-1   (Groq: whisper-large-v3-turbo)
//   ?lang=xx      idioma de la UI del aparato (es, en, zh, fr, de, pt, ru); default es
//
//   POST /api/transcribe   (Bearer del aparato, lo chequea api.ts)
//   body: audio/wav (16 kHz mono 16-bit, hasta ~10 s = 320 KB)
//   200: { ok: true, text }
//   4xx/5xx: { ok: false, error }
import { Hono } from "hono";
import { normalizeLang, type Lang } from "./lang";

const API_KEY = process.env.STT_API_KEY ?? process.env.OPENAI_API_KEY ?? "";
const BASE_URL = (process.env.STT_BASE_URL ?? "https://api.openai.com/v1").replace(/\/+$/, "");
const MODEL = process.env.STT_MODEL ?? "whisper-1";
const MAX_BYTES = 2_000_000;

export const transcribe = new Hono();

// Reutilizable desde voice.ts: WAV -> texto. Lanza Error con el detalle si falla.
export async function transcribeWav(audio: ArrayBuffer, lang: Lang = "es"): Promise<string> {
  if (!API_KEY) throw new Error("STT_API_KEY not set");
  if (audio.byteLength < 1_000) throw new Error("audio too short");
  if (audio.byteLength > MAX_BYTES) throw new Error("audio too large");
  const form = new FormData();
  form.append("file", new Blob([audio], { type: "audio/wav" }), "question.wav");
  form.append("model", MODEL);
  form.append("language", lang);
  form.append("response_format", "json");
  const res = await fetch(`${BASE_URL}/audio/transcriptions`, {
    method: "POST",
    headers: { Authorization: `Bearer ${API_KEY}` },
    body: form,
  });
  if (!res.ok) {
    const detail = (await res.text()).slice(0, 300);
    console.error("transcribe:", res.status, detail);
    throw new Error(`stt ${res.status}`);
  }
  const data = (await res.json()) as { text?: string };
  const text = (data.text ?? "").trim();
  if (!text) throw new Error("nothing recognised");
  return text;
}

transcribe.post("/", async (c) => {
  try {
    const text = await transcribeWav(await c.req.arrayBuffer(), normalizeLang(c.req.query("lang")));
    return c.json({ ok: true, text });
  } catch (err) {
    const msg = err instanceof Error ? err.message : "internal";
    const status = msg === "audio too short" ? 400 : msg === "audio too large" ? 413 : msg === "nothing recognised" ? 422 : msg.startsWith("stt ") ? 502 : 500;
    if (status === 500) console.error("transcribe:", err);
    return c.json({ ok: false, error: msg }, status);
  }
});
