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
import { decodeAdpcm, TARGET_RATE } from "./tts";
import { config } from "./config";
import { checkUrl, redactSecrets } from "./net";

// El servicio de transcripción se elige desde /board -> Ajustes (Groq es gratis
// y el más rápido); las variables de entorno quedan como valor por defecto.
const MAX_BYTES = 2_000_000;

export const transcribe = new Hono();

// Reutilizable desde voice.ts: WAV -> texto. Lanza Error con el detalle si falla.
// El aparato sube ADPCM (una cuarta parte de un WAV, que es lo que más tarda
// en la subida): acá se vuelve a PCM y se le pone cabecera WAV para el STT.
export function adpcmToWav(data: Uint8Array): ArrayBuffer {
  // El tope se mira sobre lo que llegó, no sobre lo expandido: el ADPCM ocupa
  // un cuarto, así que 2 MB de cuerpo son 8 MB de PCM reservados antes de que
  // nadie chequee nada.
  if (data.byteLength > MAX_BYTES / 4) throw new Error("audio too large");
  const pcm = decodeAdpcm(data);
  const bytes = pcm.length * 2;
  const out = new ArrayBuffer(44 + bytes);
  const dv = new DataView(out);
  const ascii = (off: number, s: string) => { for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); };
  ascii(0, "RIFF"); dv.setUint32(4, 36 + bytes, true); ascii(8, "WAVEfmt ");
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
  dv.setUint32(24, TARGET_RATE, true); dv.setUint32(28, TARGET_RATE * 2, true);
  dv.setUint16(32, 2, true); dv.setUint16(34, 16, true);
  ascii(36, "data"); dv.setUint32(40, bytes, true);
  new Int16Array(out, 44).set(pcm);
  return out;
}

// Acepta WAV o ADPCM (por el content-type o la firma "ADPC").
export function toWav(body: ArrayBuffer, contentType?: string | null): ArrayBuffer {
  if (body.byteLength > MAX_BYTES) throw new Error("audio too large");
  const head = new Uint8Array(body.slice(0, 4));
  const isAdpcm = (contentType ?? "").includes("adpcm") ||
    (head.length === 4 && head[0] === 0x41 && head[1] === 0x44 && head[2] === 0x50 && head[3] === 0x43);
  return isAdpcm ? adpcmToWav(new Uint8Array(body)) : body;
}

export async function transcribeWav(audio: ArrayBuffer, lang: Lang = "es"): Promise<string> {
  const stt = (await config()).stt;
  if (!stt.key) throw new Error("falta la clave de transcripción (web → Ajustes)");
  const url = checkUrl(stt.baseUrl, { allowLocal: true });
  if (!url.ok) throw new Error(`la URL de transcripción no sirve: ${url.error}`);
  if (audio.byteLength < 1_000) throw new Error("audio too short");
  if (audio.byteLength > MAX_BYTES) throw new Error("audio too large");
  const form = new FormData();
  form.append("file", new Blob([audio], { type: "audio/wav" }), "question.wav");
  form.append("model", stt.model);
  form.append("language", lang);
  form.append("response_format", "json");
  const res = await fetch(`${stt.baseUrl.replace(/\/+$/, "")}/audio/transcriptions`, {
    method: "POST",
    headers: { Authorization: `Bearer ${stt.key}` },
    body: form,
    signal: AbortSignal.timeout(60_000),
  });
  if (!res.ok) {
    // El error del servicio vuelve al aparato y al log: sin tachar la clave,
    // un 401 de algunos proveedores la escupe entera.
    const detail = redactSecrets(await res.text(), stt.key).slice(0, 300);
    console.error("transcribe:", res.status, detail);
    throw new Error(`stt ${res.status}: ${detail.slice(0, 120)}`);
  }
  const data = (await res.json()) as { text?: string };
  const text = (data.text ?? "").trim();
  if (!text) throw new Error("nothing recognised");
  return text;
}

transcribe.post("/", async (c) => {
  try {
    const body = toWav(await c.req.arrayBuffer(), c.req.header("content-type"));
    const text = await transcribeWav(body, normalizeLang(c.req.query("lang")));
    return c.json({ ok: true, text });
  } catch (err) {
    const msg = err instanceof Error ? err.message : "internal";
    const status = msg === "audio too short" ? 400 : msg === "audio too large" ? 413 : msg === "nothing recognised" ? 422 : msg.startsWith("stt ") ? 502 : 500;
    if (status === 500) console.error("transcribe:", err);
    return c.json({ ok: false, error: msg }, status);
  }
});
