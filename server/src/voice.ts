// El botón de voz del hub: una sola grabación, el servidor decide qué es.
//
//   POST /api/voice   (Bearer del aparato; body audio/wav 16 kHz mono)
//   200: { ok: true, text, intent, reply, saved?: { kind, list?, title?, when? } }
//     text   = lo que se entendió
//     intent = question | reminder | task | shopping | note | message | timer | alarm | translate
//     reply  = texto corto para la pantalla (y para leer por el parlante cuando haya TTS)
//   4xx/5xx: { ok: false, error }
//
// Claude clasifica con salida estructurada (JSON con esquema) y en la misma
// llamada redacta la respuesta: una pregunta se contesta con conocimiento
// general; un recordatorio, tarea, compra, nota o mensaje se guarda en el
// store y se confirma; temporizador y alarma todavía no se ejecutan en el
// aparato, así que se avisa. Una grabación puede traer una acción y una
// pregunta a la vez: se hacen las dos.
//
// Modelo: VOICE_MODEL (default claude-haiku-4-5, el más barato; cualquier ID
// actual sirve, el pedido no usa parámetros específicos de modelo).
import { Hono } from "hono";
import Anthropic from "@anthropic-ai/sdk";
import { transcribeWav } from "./transcribe";
import { load, save, nextId, resolveList, whenLabel, pendingReminders, localToEpoch, epochToLocal, advanceRepeat, DEFAULT_LISTS } from "./store";
import { LANGUAGE_NAME, defaultTranslateTarget, normalizeLang, type Lang } from "./lang";

const MODEL = process.env.VOICE_MODEL ?? process.env.ASK_MODEL ?? "claude-haiku-4-5";
const TZ = process.env.HUB_TZ ?? "America/Argentina/Buenos_Aires";

const client = new Anthropic();

export const voice = new Hono();

const SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["intent", "reply", "actions"],
  properties: {
    intent: {
      type: "string",
      enum: ["question", "reminder", "task", "shopping", "note", "message", "timer", "alarm", "translate"],
      description: "Intención principal de lo dicho.",
    },
    reply: {
      type: "string",
      description:
        "Texto para la pantalla: la respuesta si es pregunta o traducción, o una confirmación de una línea de lo guardado. Texto plano, sin markdown.",
    },
    actions: {
      type: "array",
      description: "Lo que hay que guardar. Vacío si es solo una pregunta.",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["kind", "text", "list", "dueAt", "repeat", "seconds"],
        properties: {
          kind: { type: "string", enum: ["reminder", "task", "shopping", "note", "message", "timer", "alarm"] },
          text: { type: "string", description: "Título de la tarea/recordatorio, ítem de compra, texto de la nota o mensaje." },
          list: { type: ["string", "null"], description: "Nombre de la lista si el usuario la nombró o se deduce; null si no." },
          dueAt: {
            type: ["string", "null"],
            description: "Fecha y hora local del recordatorio o vencimiento como YYYY-MM-DDTHH:MM, o YYYY-MM-DD si no dijo hora; null si no tiene.",
          },
          repeat: { type: "string", enum: ["none", "daily", "weekly", "monthly"] },
          seconds: { type: ["integer", "null"], description: "Duración en segundos para timer; null si no aplica." },
        },
      },
    },
  },
} as const;

function systemPrompt(now: string, weekday: string, lists: string[], lang: Lang): string {
  return [
    "Sos el asistente por voz de un aparato de tinta electrónica sin teclado. Recibís una frase transcripta",
    "de voz (puede traer errores de reconocimiento; interpretala con sentido común y no comentes la transcripción)",
    "y devolvés JSON según el esquema.",
    `Ahora es ${now} (${weekday}), zona ${TZ}. Resolvé fechas relativas (mañana, el jueves, la semana que viene) a fecha absoluta.`,
    `Listas de tareas existentes: ${lists.join(", ")}. Si el usuario nombra una que no existe, usá ese nombre igual (se crea).`,
    "Reglas: 'recordame', 'avisame', 'despertame' o algo con hora concreta → reminder (con dueAt). 'Comprar X', 'compras:' o",
    "artículos sueltos → shopping, un ítem por producto (\"leche y huevos\" son dos acciones). 'Agregá a <lista>', 'en trabajo:',",
    "'tengo que', 'hay que' → task (list si la nombró; si no, null y va a Entrada). 'Nota:', 'anotá' → note. 'Mensaje para',",
    "'dejá dicho', 'avisale a' → message. 'Poné N minutos', 'temporizador', 'pomodoro' → timer con seconds. 'Alarma a las' → alarm;",
    "para timer y alarm, reply avisa (en el idioma del usuario) que el aparato todavía no los ejecuta.",
    `'Traducí', 'cómo se dice' (o su equivalente en el idioma del usuario) → translate y reply es SOLO la traducción, al idioma que pida; si no dice a cuál, a ${defaultTranslateTarget(lang)}. Cualquier otra cosa (duda, dato, explicación) → question`,
    "y reply la contesta con conocimiento general, corta y directa. Si la frase trae una acción y una pregunta, guardá la",
    "acción en actions y contestá la pregunta en reply. Si es ambiguo entre acción y pregunta, elegí task en Entrada y decilo.",
    `El usuario habla en ${LANGUAGE_NAME[lang]}: los títulos de las acciones y reply van en ese idioma (salvo la traducción). Texto plano, sin markdown ni listas. Máximo 120 palabras salvo que pida más.`,
  ].join(" ");
}

type Action = { kind: string; text: string; list: string | null; dueAt: string | null; repeat: "none" | "daily" | "weekly" | "monthly"; seconds: number | null };
type Parsed = { intent: string; reply: string; actions: Action[] };

async function classify(text: string, lang: Lang): Promise<Parsed> {
  const store = await load();
  const now = new Date();
  const local = now.toLocaleString("sv-SE", { timeZone: TZ }).slice(0, 16).replace(" ", "T");
  const weekday = now.toLocaleDateString("en-US", { weekday: "long", timeZone: TZ });
  const lists = Array.from(new Set([...DEFAULT_LISTS, ...Object.keys(store.lists)]));
  const response = await client.messages.create({
    model: MODEL,
    max_tokens: 1024,
    system: systemPrompt(local, weekday, lists, lang),
    messages: [{ role: "user", content: text }],
    output_config: { format: { type: "json_schema", schema: SCHEMA } },
  });
  if (response.stop_reason === "refusal") throw new Error("refused");
  let json = "";
  for (const block of response.content) if (block.type === "text") json += block.text;
  return JSON.parse(json) as Parsed;
}

async function execute(parsed: Parsed, spoken: string, lang: Lang) {
  const store = await load();
  const saved: { kind: string; list?: string; title: string; when?: string }[] = [];
  const stamp = new Date().toISOString();
  for (const a of parsed.actions ?? []) {
    const title = (a.text ?? "").trim().slice(0, 200);
    if (!title) continue;
    switch (a.kind) {
      case "reminder": {
        const dueAt = a.dueAt && /^\d{4}-\d{2}-\d{2}/.test(a.dueAt) ? a.dueAt : null;
        store.reminders.push({ id: nextId(store), title, dueAt, repeat: a.repeat ?? "none", done: false, createdAt: stamp });
        saved.push({ kind: "reminder", title, when: whenLabel(dueAt, lang) });
        break;
      }
      case "task": {
        const list = resolveList(store, a.list, true);
        store.lists[list].push({ id: nextId(store), text: title, done: false, dueDate: a.dueAt ? a.dueAt.slice(0, 10) : null, createdAt: stamp });
        saved.push({ kind: "task", list, title });
        break;
      }
      case "shopping": {
        const list = resolveList(store, a.list ?? "Compras", true);
        store.lists[list].push({ id: nextId(store), text: title, done: false, dueDate: null, createdAt: stamp });
        saved.push({ kind: "shopping", list, title });
        break;
      }
      case "note":
        store.notes.push({ id: nextId(store), text: title, createdAt: stamp });
        saved.push({ kind: "note", title });
        break;
      case "message":
        store.messages.push({ id: nextId(store), from: "voz", text: title, createdAt: stamp, read: false });
        saved.push({ kind: "message", title });
        break;
      default:
        break; // timer / alarm: todavía no se ejecutan (Fase 2.8 / 2.9)
    }
  }
  if (saved.length) await save(store);
  console.log(`voice: "${spoken}" -> ${parsed.intent}`, saved.map((s) => `${s.kind}:${s.title}`).join(" | "));
  return saved;
}

voice.post("/", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  let text: string;
  try {
    text = await transcribeWav(await c.req.arrayBuffer(), lang);
  } catch (err) {
    const msg = err instanceof Error ? err.message : "internal";
    return c.json({ ok: false, error: msg }, msg.startsWith("stt ") ? 502 : 400);
  }
  try {
    const parsed = await classify(text, lang);
    const saved = await execute(parsed, text, lang);
    const reply = parsed.reply;
    return c.json({ ok: true, text, intent: parsed.intent, reply, saved });
  } catch (err) {
    if (err instanceof Anthropic.RateLimitError) return c.json({ ok: false, error: "rate limited" }, 429);
    if (err instanceof Anthropic.AuthenticationError) return c.json({ ok: false, error: "bad ANTHROPIC_API_KEY" }, 500);
    if (err instanceof Anthropic.APIError) return c.json({ ok: false, error: `claude ${err.status}: ${err.message}` }, 502);
    console.error("voice:", err);
    return c.json({ ok: false, error: "internal" }, 500);
  }
});

// Lo que el hub muestra y cachea: recordatorios pendientes (el primero es el
// próximo), listas con sus ítems pendientes y mensajes sin leer.
export async function hubSlice(lang: Lang) {
  const store = await load();
  return {
    reminders: pendingReminders(store).slice(0, 20).map((r) => ({ id: r.id, title: r.title, when: whenLabel(r.dueAt, lang), dueAt: localToEpoch(r.dueAt) })),
    lists: Object.entries(store.lists).map(([name, items]) => ({
      name,
      items: items.filter((i) => !i.done).slice(0, 30).map((i) => ({ id: i.id, text: i.text })),
    })),
    messages: store.messages.filter((m) => !m.read).slice(-5).map((m) => ({ from: m.from, text: m.text })),
  };
}

// Tildar (o posponer `snoozeSeconds`) desde el aparato. Idempotente: llega
// repetido desde la cola offline. Un recordatorio con repetición no se cierra:
// pasa al próximo ciclo.
export async function markDone(kind: "reminder" | "item", id: number, snoozeSeconds = 0): Promise<boolean> {
  const store = await load();
  let found = false;
  if (kind === "reminder") {
    for (const r of store.reminders) {
      if (r.id !== id) continue;
      found = true;
      if (snoozeSeconds > 0) r.dueAt = epochToLocal(Math.floor(Date.now() / 1000) + snoozeSeconds);
      else if (!advanceRepeat(r)) r.done = true;
    }
  } else {
    for (const items of Object.values(store.lists)) for (const i of items) if (i.id === id) { i.done = true; found = true; }
  }
  if (found) await save(store);
  return found;
}
