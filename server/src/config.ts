// Configuración editable desde la web (/board → Ajustes), guardada en el volumen
// de Railway. Antes todo esto eran variables de entorno y había que entrar a
// Railway para cambiar de proveedor de IA o de modelo.
//
// Las claves se guardan acá y NO se devuelven nunca por la API: el board solo
// ve si hay clave puesta o no. Las variables de entorno siguen sirviendo como
// valor por defecto (y como red de seguridad si el archivo se pierde).
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const FILE = process.env.CONFIG_FILE ?? "/data/config.json";

export type LlmConfig = {
  provider: "anthropic" | "openai";  // "openai" = cualquier API compatible (Groq, DeepSeek, OpenAI, ...)
  baseUrl: string;                   // solo para "openai"
  model: string;
  key: string;
};
export type SttConfig = { baseUrl: string; model: string; key: string };

export type Config = {
  llm: LlmConfig;
  stt: SttConfig;
  deviceToken: string;  // vacío = solo vale el DEVICE_TOKEN del entorno
};

// Combos conocidos, para elegir de una lista en la web en vez de escribir URLs.
export const PRESETS: Record<string, { label: string; provider: "anthropic" | "openai"; baseUrl: string; models: string[]; sttBaseUrl?: string; sttModels?: string[] }> = {
  anthropic: {
    label: "Anthropic (Claude)",
    provider: "anthropic",
    baseUrl: "",
    models: ["claude-haiku-4-5", "claude-sonnet-4-5"],
  },
  groq: {
    label: "Groq (gratis, muy rápido)",
    provider: "openai",
    baseUrl: "https://api.groq.com/openai/v1",
    models: ["llama-3.3-70b-versatile", "llama-3.1-8b-instant", "openai/gpt-oss-120b"],
    sttBaseUrl: "https://api.groq.com/openai/v1",
    sttModels: ["whisper-large-v3-turbo", "whisper-large-v3"],
  },
  deepseek: {
    label: "DeepSeek (muy barato)",
    provider: "openai",
    baseUrl: "https://api.deepseek.com/v1",
    models: ["deepseek-chat", "deepseek-reasoner"],
  },
  openai: {
    label: "OpenAI",
    provider: "openai",
    baseUrl: "https://api.openai.com/v1",
    models: ["gpt-4o-mini", "gpt-4o"],
    sttBaseUrl: "https://api.openai.com/v1",
    sttModels: ["whisper-1", "gpt-4o-mini-transcribe"],
  },
};

function defaults(): Config {
  return {
    llm: {
      provider: "anthropic",
      baseUrl: "",
      model: process.env.ASK_MODEL ?? process.env.VOICE_MODEL ?? "claude-haiku-4-5",
      key: process.env.ANTHROPIC_API_KEY ?? "",
    },
    stt: {
      baseUrl: (process.env.STT_BASE_URL ?? "https://api.openai.com/v1").replace(/\/+$/, ""),
      model: process.env.STT_MODEL ?? "whisper-1",
      key: process.env.STT_API_KEY ?? process.env.OPENAI_API_KEY ?? "",
    },
    deviceToken: "",
  };
}

let cache: Config | null = null;

export async function config(): Promise<Config> {
  if (cache) return cache;
  const base = defaults();
  try {
    const saved = JSON.parse(await readFile(FILE, "utf8")) as Partial<Config>;
    cache = {
      llm: { ...base.llm, ...(saved.llm ?? {}) },
      stt: { ...base.stt, ...(saved.stt ?? {}) },
      deviceToken: saved.deviceToken ?? "",
    };
    // Una clave vacía en el archivo no pisa la del entorno.
    if (!cache.llm.key) cache.llm.key = base.llm.key;
    if (!cache.stt.key) cache.stt.key = base.stt.key;
  } catch {
    cache = base;
  }
  return cache;
}

export async function saveConfig(next: Config): Promise<void> {
  cache = next;
  await mkdir(dirname(FILE), { recursive: true });
  await writeFile(FILE, JSON.stringify(next, null, 2));
}

// Lo que puede ver la web: todo menos las claves (solo si están puestas).
export async function publicConfig() {
  const c = await config();
  return {
    llm: { provider: c.llm.provider, baseUrl: c.llm.baseUrl, model: c.llm.model, hasKey: !!c.llm.key },
    stt: { baseUrl: c.stt.baseUrl, model: c.stt.model, hasKey: !!c.stt.key },
    deviceTokenSet: !!c.deviceToken,
    presets: PRESETS,
  };
}
