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
//   200: { ok: true, text: "", code: "no_speech", error: "No escuché nada..." }  ← no habló nadie
//   4xx/5xx: { ok: false, error }
//
// OJO con el silencio: Whisper NUNCA devuelve vacío. Si le mandás una grabación
// muda inventa la frase que más veces vio en los subtítulos con los que lo
// entrenaron: "Subtítulos realizados por la comunidad de Amara.org", "Gracias
// por ver el video", "Thanks for watching", "♪". El aparato abría Hablar, el
// usuario no decía nada y el asistente contestaba algo sobre amara.org. Por eso
// hay dos filtros: la energía del audio ANTES de gastar la llamada al STT
// (hasSpeech) y la lista de frases inventadas DESPUÉS (isHallucination).
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

// ── Silencio ────────────────────────────────────────────────────────────────
// Mínimos de audio, ajustables desde el entorno porque dependen de la ganancia
// del micrófono de la placa (el PGA del ES8311 todavía está en el default).
const MIN_SECONDS = Number(process.env.STT_MIN_SECONDS ?? 0.4);
const MIN_PEAK = Number(process.env.STT_MIN_PEAK ?? 350);   // amplitud, 16 bits (≈ -39 dBFS)
const MIN_RMS = Number(process.env.STT_MIN_RMS ?? 90);      // RMS de los tramos con voz (≈ -51 dBFS)

export type Loudness = { seconds: number; rate: number; peak: number; rms: number; floor: number; voiced: number };

// PCM 16 bits del WAV (mono; si viene estéreo se toma el canal izquierdo).
function wavPcm(wav: ArrayBuffer): { pcm: Int16Array; rate: number } | null {
  const dv = new DataView(wav);
  if (wav.byteLength < 44) return null;
  const tag = (off: number) => String.fromCharCode(dv.getUint8(off), dv.getUint8(off + 1), dv.getUint8(off + 2), dv.getUint8(off + 3));
  if (tag(0) !== "RIFF" || tag(8) !== "WAVE") return null;
  let pos = 12;
  let rate = TARGET_RATE;
  let channels = 1;
  let bits = 16;
  while (pos + 8 <= wav.byteLength) {
    const id = tag(pos);
    const size = dv.getUint32(pos + 4, true);
    if (id === "fmt ") {
      channels = Math.max(1, dv.getUint16(pos + 10, true));
      rate = dv.getUint32(pos + 12, true) || TARGET_RATE;
      bits = dv.getUint16(pos + 22, true) || 16;
    } else if (id === "data") {
      if (bits !== 16) return null;  // el aparato manda siempre 16 bits; otra cosa no se juzga
      const bytes = Math.min(size, wav.byteLength - pos - 8);
      const n = Math.floor(bytes / 2 / channels);
      const pcm = new Int16Array(n);
      for (let i = 0; i < n; i++) pcm[i] = dv.getInt16(pos + 8 + i * 2 * channels, true);
      return { pcm, rate };
    }
    pos += 8 + size + (size & 1);
  }
  return null;
}

// Energía por tramos de 20 ms: el piso de ruido es el percentil 10 y la voz el
// 90. Se mira el contraste (voz contra ruido) Y un mínimo absoluto: solo el
// contraste haría pasar un cuarto en silencio con un poco de zumbido, y solo el
// mínimo absoluto dependería de la ganancia del micrófono.
export function loudness(wav: ArrayBuffer): Loudness | null {
  const parsed = wavPcm(wav);
  if (!parsed) return null;
  const { pcm, rate } = parsed;
  const frame = Math.max(1, Math.round(rate * 0.02));
  const frames: number[] = [];
  let peak = 0;
  let sumSq = 0;
  for (let i = 0; i < pcm.length; i += frame) {
    const end = Math.min(i + frame, pcm.length);
    let acc = 0;
    for (let j = i; j < end; j++) {
      const v = pcm[j];
      acc += v * v;
      const a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    sumSq += acc;
    frames.push(Math.sqrt(acc / Math.max(1, end - i)));
  }
  if (!frames.length) return { seconds: 0, rate, peak: 0, rms: 0, floor: 0, voiced: 0 };
  const sorted = [...frames].sort((a, b) => a - b);
  const at = (q: number) => sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * q))];
  const floor = at(0.1);
  const p90 = at(0.9);
  const gate = Math.max(MIN_RMS, floor * 2.2, p90 * 0.35);
  const voiced = frames.filter((f) => f >= gate).length;
  return { seconds: pcm.length / rate, rate, peak, rms: Math.sqrt(sumSq / Math.max(1, pcm.length)), floor, voiced };
}

// ¿Hay alguien hablando? Falso = ni se manda al STT.
export function hasSpeech(wav: ArrayBuffer): { ok: boolean; why: string; info: Loudness | null } {
  const l = loudness(wav);
  if (!l) return { ok: true, why: "", info: null };  // formato raro: que decida el STT
  if (l.seconds < MIN_SECONDS) return { ok: false, why: `dura ${l.seconds.toFixed(2)} s`, info: l };
  if (l.peak < MIN_PEAK) return { ok: false, why: `pico ${l.peak} < ${MIN_PEAK}`, info: l };
  // 6 tramos de 20 ms = 120 ms de voz: menos que eso no es ni una palabra.
  if (l.voiced < 6) return { ok: false, why: `${l.voiced} tramos con voz`, info: l };
  return { ok: true, why: "", info: l };
}

// ── Alucinaciones ───────────────────────────────────────────────────────────
// Frases que Whisper escupe cuando no escuchó nada. Si TODO lo que devolvió es
// una de estas, es silencio. Se comparan normalizadas (sin tildes, sin signos).
const HALLUCINATIONS = [
  // amara.org, la más famosa de todas, en los seis idiomas
  "subtitulos realizados por la comunidad de amara org",
  "subtitulos por la comunidad de amara org",
  "subtitulado por la comunidad de amara org",
  "mas subtitulos en amara org",
  "subtitles by the amara org community",
  "subtitles by the amara org community subtitles by the amara org community",
  "sous titres realises par la communaute d amara org",
  "sous titres realises par l amara org community",
  "untertitel der amara org gemeinschaft",
  "untertitel im auftrag des zdf",
  "untertitelung im auftrag des zdf",
  "legendas pela comunidade amara org",
  "legendado pela comunidade amara org",
  "субтитры сообщества amara org",
  "субтитры создавал dimatorzok",
  "субтитры сделал dimatorzok",
  // "gracias por ver" y sus primos
  "gracias por ver el video", "gracias por ver este video", "gracias por ver", "gracias",
  "muchas gracias", "muchas gracias por ver el video", "nos vemos en el proximo video",
  "hasta la proxima", "hasta el proximo video", "no olvides suscribirte",
  "suscribete al canal", "suscribanse al canal", "dale like y suscribete",
  "thanks for watching", "thank you for watching", "thanks for watching the video",
  "thank you", "thank you very much", "thanks", "see you next time", "bye", "bye bye",
  "please subscribe", "subscribe to my channel", "like and subscribe",
  "merci d avoir regarde cette video", "merci d avoir regarde", "merci", "merci beaucoup",
  "abonnez vous", "a bientot", "sous titrage societe radio canada",
  "vielen dank", "vielen dank furs zuschauen", "danke furs zuschauen", "danke", "bis zum nachsten mal",
  "abonniert den kanal", "untertitel von stephanie geiges",
  "obrigado por assistir", "obrigado", "obrigado por assistirem", "inscreva se no canal",
  "ate a proxima", "muito obrigado",
  "спасибо за просмотр", "спасибо", "подписывайтесь на канал", "продолжение следует",
  "редактор субтитров а семкин", "корректор а егорова", "до новых встреч",
];
const HALLUCINATION_SET = new Set(HALLUCINATIONS);

// Etiquetas de ruido. Estas NO alcanzan por sí solas: "música" dicho a secas es
// una orden razonable (el hub tiene un mosaico Música), así que solo cuentan
// como alucinación cuando vienen entre corchetes o paréntesis, que es como las
// escribe Whisper cuando etiqueta un sonido: "[Música]", "(aplausos)".
const NOISE_TAGS = new Set([
  "musica", "musica de fondo", "music", "musique", "musik", "aplausos", "applause", "applaudissements",
  "risas", "laughter", "lachen", "silencio", "silence", "stille", "ruido", "noise", "tos", "suspiro",
  "музыка", "аплодисменты", "смех", "тишина",
]);

// Sin tildes, sin signos, en minúsculas y con un solo espacio. Los corchetes de
// "[Música]" y los paréntesis de "(música de fondo)" se van con los signos.
function foldText(s: string): string {
  return s
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "")
    .toLowerCase()
    .replace(/[^\p{Letter}\p{Number}\s]+/gu, " ")
    .replace(/\s+/g, " ")
    .trim();
}

// ¿Lo que devolvió el STT es una alucinación de silencio y no lo que dijo el usuario?
export function isHallucination(text: string): boolean {
  const raw = text.trim();
  if (!raw) return true;
  // Solo signos, notas musicales o puntos suspensivos: "♪", "...", "[ ]".
  if (!/[\p{Letter}\p{Number}]/u.test(raw)) return true;
  const f = foldText(raw);
  if (!f || f.length < 2) return true;
  // Solo una etiqueta de ruido y entre corchetes o paréntesis: "[Música]".
  if (/^[\s([*_-]*[^\])*_]+[\s)\]*_.-]*$/.test(raw) && /^[([*_]/.test(raw.trim()) && NOISE_TAGS.has(f)) return true;
  // Todo lo que devolvió es una cadena de frases inventadas, pegadas o
  // repetidas: "Subtítulos realizados por la comunidad de Amara.org. ¡Gracias
  // por ver el video!" o "Gracias. Gracias. Gracias.". Como foldText ya se
  // comió los puntos, se consume de izquierda a derecha con la frase más larga
  // que encaje; si no queda nada, no dijo nada.
  return consumedByHallucinations(f);
}

// Máximo de palabras de una frase de la lista (para no probar de más).
const MAX_PHRASE_WORDS = Math.max(...HALLUCINATIONS.map((h) => h.split(" ").length));

function consumedByHallucinations(folded: string): boolean {
  const words = folded.split(" ");
  let i = 0;
  while (i < words.length) {
    let taken = 0;
    for (let n = Math.min(MAX_PHRASE_WORDS, words.length - i); n >= 1; n--) {
      if (HALLUCINATION_SET.has(words.slice(i, i + n).join(" "))) { taken = n; break; }
    }
    if (!taken) return false;
    i += taken;
  }
  return true;
}

// El texto que se le muestra (y se le dice) al usuario cuando no habló.
export const NO_SPEECH = "no_speech";
export const NO_SPEECH_MSG: Record<Lang, string> = {
  es: "No escuché nada, inténtalo de nuevo.",
  en: "I didn't hear anything, please try again.",
  fr: "Je n'ai rien entendu, essaie encore.",
  de: "Ich habe nichts gehört, versuche es noch einmal.",
  pt: "Não ouvi nada, tenta de novo.",
  ru: "Я ничего не услышал, попробуй ещё раз.",
};

// Error propio: los llamadores lo distinguen de un fallo del STT.
export class NoSpeechError extends Error {
  constructor(readonly why = "") {
    super(NO_SPEECH);
  }
}

export async function transcribeWav(audio: ArrayBuffer, lang: Lang = "es"): Promise<string> {
  const stt = (await config()).stt;
  if (!stt.key) throw new Error("falta la clave de transcripción (web → Ajustes)");
  const url = checkUrl(stt.baseUrl, { allowLocal: true });
  if (!url.ok) throw new Error(`la URL de transcripción no sirve: ${url.error}`);
  if (audio.byteLength < 1_000) throw new Error("audio too short");
  if (audio.byteLength > MAX_BYTES) throw new Error("audio too large");
  // Antes de gastar la llamada (y de que el modelo invente amara.org).
  const speech = hasSpeech(audio);
  if (!speech.ok) {
    console.log(`transcribe: silencio, no se manda al STT (${speech.why})`);
    throw new NoSpeechError(speech.why);
  }
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
  if (!text) throw new NoSpeechError("el STT devolvió vacío");
  if (isHallucination(text)) {
    console.log(`transcribe: alucinación de silencio descartada: "${text.slice(0, 80)}"`);
    throw new NoSpeechError(`alucinación: ${text.slice(0, 60)}`);
  }
  return text;
}

transcribe.post("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  try {
    const body = toWav(await c.req.arrayBuffer(), c.req.header("content-type"));
    const text = await transcribeWav(body, lang);
    return c.json({ ok: true, text });
  } catch (err) {
    // Que no haya hablado nadie NO es un error del servidor: vuelve con 200 y
    // texto vacío para que el aparato muestre el mensaje en vez de un
    // "http error (422)" que no dice nada.
    if (err instanceof NoSpeechError) {
      return c.json({ ok: true, text: "", code: NO_SPEECH, error: NO_SPEECH_MSG[lang], why: err.why });
    }
    const msg = err instanceof Error ? err.message : "internal";
    const status = msg === "audio too short" ? 400 : msg === "audio too large" ? 413 : msg.startsWith("stt ") ? 502 : 500;
    if (status === 500) console.error("transcribe:", err);
    return c.json({ ok: false, error: msg }, status);
  }
});
