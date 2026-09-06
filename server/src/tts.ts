// Voz del aparato con Piper (local, sin tokens). Un proceso de Piper por
// idioma, que se queda vivo con el modelo cargado: la primera frase tarda ~1 s,
// las siguientes ~0,2 s. El WAV de Piper (22050 Hz) se baja a 16 kHz y se
// comprime a IMA ADPCM (4 bits por muestra): 4 s de voz = 32 KB, que el
// aparato baja en menos de medio segundo y decodifica en unas líneas.
//
//   GET /api/tts?text=...&lang=xx   -> audio/adpcm (16 kHz mono, ver Adpcm.h del firmware)
//   synthesize(text, lang)          -> Uint8Array, usada por voice.ts para
//                                      mandar la voz junto con la respuesta.
//
// Env: PIPER_BIN (default /opt/piper/piper), PIPER_VOICES (default /opt/piper/voices),
//      TTS_ENABLED=0 apaga todo. Voces por idioma en VOICES; se bajan en el Dockerfile.
import { Hono } from "hono";
import { spawn, type ChildProcess } from "node:child_process";
import { mkdir, readFile, unlink } from "node:fs/promises";
import { existsSync } from "node:fs";
import { normalizeLang, type Lang } from "./lang";

const PIPER_BIN = process.env.PIPER_BIN ?? "/opt/piper/piper";
const VOICES_DIR = process.env.PIPER_VOICES ?? "/opt/piper/voices";
const OUT_DIR = process.env.PIPER_OUT ?? "/tmp/piper-out";
const ENABLED = process.env.TTS_ENABLED !== "0";
export const TARGET_RATE = 16000;
const MAX_CHARS = 400;

export const VOICES: Record<Lang, string> = {
  es: "es_MX-ald-medium",
  en: "en_US-lessac-medium",
  fr: "fr_FR-siwis-medium",
  de: "de_DE-thorsten-medium",
  pt: "pt_BR-faber-medium",
  ru: "ru_RU-irina-medium",
};

type Worker = { proc: ChildProcess; queue: ((line: string) => void)[]; buffer: string };
const workers = new Map<Lang, Worker>();

export function ttsAvailable(lang: Lang): boolean {
  return ENABLED && existsSync(PIPER_BIN) && existsSync(`${VOICES_DIR}/${VOICES[lang]}.onnx`);
}

function worker(lang: Lang): Worker {
  const existing = workers.get(lang);
  if (existing && existing.proc.exitCode === null) return existing;
  const proc = spawn(PIPER_BIN, ["--model", `${VOICES_DIR}/${VOICES[lang]}.onnx`, "--json-input", "--output_dir", OUT_DIR], {
    stdio: ["pipe", "pipe", "ignore"],
  });
  const w: Worker = { proc, queue: [], buffer: "" };
  proc.stdout!.setEncoding("utf8");
  proc.stdout!.on("data", (chunk: string) => {
    w.buffer += chunk;
    let nl: number;
    while ((nl = w.buffer.indexOf("\n")) >= 0) {
      const line = w.buffer.slice(0, nl).trim();
      w.buffer = w.buffer.slice(nl + 1);
      const resolve = w.queue.shift();
      if (resolve) resolve(line);
    }
  });
  proc.on("exit", () => {
    for (const r of w.queue) r("");
    w.queue = [];
    if (workers.get(lang) === w) workers.delete(lang);
  });
  workers.set(lang, w);
  return w;
}

// Piper escribe un WAV por línea y avisa la ruta por stdout. Serializado por
// idioma: una frase a la vez, en orden.
async function piperWav(text: string, lang: Lang): Promise<Buffer | null> {
  await mkdir(OUT_DIR, { recursive: true });
  const w = worker(lang);
  const path = await new Promise<string>((resolve) => {
    w.queue.push(resolve);
    w.proc.stdin!.write(JSON.stringify({ text }) + "\n");
  });
  if (!path) return null;
  try {
    const wav = await readFile(path);
    await unlink(path).catch(() => {});
    return wav;
  } catch {
    return null;
  }
}

function wavPcm(wav: Buffer): { rate: number; pcm: Int16Array } | null {
  if (wav.length < 44 || wav.toString("ascii", 0, 4) !== "RIFF") return null;
  let pos = 12;
  let rate = 0;
  let channels = 1;
  while (pos + 8 <= wav.length) {
    const id = wav.toString("ascii", pos, pos + 4);
    const size = wav.readUInt32LE(pos + 4);
    if (id === "fmt ") {
      channels = wav.readUInt16LE(pos + 10);
      rate = wav.readUInt32LE(pos + 12);
    } else if (id === "data") {
      const bytes = Math.min(size, wav.length - pos - 8);
      const samples = new Int16Array(Math.floor(bytes / 2 / channels));
      for (let i = 0; i < samples.length; i++) samples[i] = wav.readInt16LE(pos + 8 + i * 2 * channels);
      return rate ? { rate, pcm: samples } : null;
    }
    pos += 8 + size + (size & 1);
  }
  return null;
}

// Lineal, alcanza para voz (22050 -> 16000).
function resample(pcm: Int16Array, from: number, to: number): Int16Array {
  if (from === to) return pcm;
  const n = Math.floor((pcm.length * to) / from);
  const out = new Int16Array(n);
  const step = from / to;
  for (let i = 0; i < n; i++) {
    const x = i * step;
    const i0 = Math.floor(x);
    const i1 = Math.min(i0 + 1, pcm.length - 1);
    const f = x - i0;
    out[i] = Math.round(pcm[i0] * (1 - f) + pcm[i1] * f);
  }
  return out;
}

// IMA ADPCM, una sola corrida sin bloques (el aparato decodifica con el mismo
// estado inicial). Cabecera propia de 8 bytes: "ADPC" + samples (uint32 LE).
const STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
  143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
  1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

export function encodeAdpcm(pcm: Int16Array): Uint8Array {
  const out = new Uint8Array(8 + Math.ceil(pcm.length / 2));
  out.set([0x41, 0x44, 0x50, 0x43]); // "ADPC"
  new DataView(out.buffer).setUint32(4, pcm.length, true);
  let predictor = 0;
  let index = 0;
  let step = STEP_TABLE[0];
  for (let i = 0; i < pcm.length; i++) {
    let diff = pcm[i] - predictor;
    let code = 0;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    let delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    predictor += code & 8 ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;
    index += INDEX_TABLE[code];
    if (index < 0) index = 0;
    else if (index > 88) index = 88;
    step = STEP_TABLE[index];
    const pos = 8 + (i >> 1);
    if (i & 1) out[pos] |= code << 4;
    else out[pos] = code;
  }
  return out;
}

// Para probar el codec del lado del servidor (mismo algoritmo que Adpcm.cpp del firmware).
export function decodeAdpcm(data: Uint8Array): Int16Array {
  const samples = new DataView(data.buffer, data.byteOffset).getUint32(4, true);
  const out = new Int16Array(samples);
  let predictor = 0, index = 0, step = STEP_TABLE[0];
  for (let i = 0; i < samples; i++) {
    const byte = data[8 + (i >> 1)];
    const code = i & 1 ? byte >> 4 : byte & 15;
    let delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor += code & 8 ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;
    out[i] = predictor;
    index += INDEX_TABLE[code];
    if (index < 0) index = 0;
    else if (index > 88) index = 88;
    step = STEP_TABLE[index];
  }
  return out;
}

// Texto -> ADPCM 16 kHz. null si Piper no está o falló. maxSeconds recorta
// (una respuesta larga no se habla entera: el aparato la muestra).
export async function synthesize(text: string, lang: Lang, maxSeconds = 12): Promise<Uint8Array | null> {
  if (!ttsAvailable(lang)) return null;
  const clean = text.replace(/\s+/g, " ").trim().slice(0, MAX_CHARS);
  if (!clean) return null;
  const t0 = Date.now();
  const wav = await piperWav(clean, lang);
  if (!wav) return null;
  const parsed = wavPcm(wav);
  if (!parsed) return null;
  let pcm = resample(parsed.pcm, parsed.rate, TARGET_RATE);
  const maxSamples = maxSeconds * TARGET_RATE;
  if (pcm.length > maxSamples) pcm = pcm.subarray(0, maxSamples);
  const out = encodeAdpcm(pcm);
  console.log(`tts ${lang}: ${clean.length} chars -> ${(pcm.length / TARGET_RATE).toFixed(1)} s, ${out.length} bytes, ${Date.now() - t0} ms`);
  return out;
}

// Calienta el proceso del idioma (al arrancar y al cambiar de idioma).
export function warmUp(lang: Lang): void {
  if (ttsAvailable(lang)) void synthesize(".", lang, 1);
}

export const tts = new Hono();

tts.get("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const text = (c.req.query("text") ?? "").trim();
  if (!text) return c.json({ ok: false, error: "text is required" }, 400);
  const audio = await synthesize(text, lang, Number(c.req.query("max") ?? 20));
  if (!audio) return c.json({ ok: false, error: "tts unavailable" }, 503);
  return new Response(Buffer.from(audio), { headers: { "Content-Type": "audio/adpcm", "Content-Length": String(audio.length) } });
});
