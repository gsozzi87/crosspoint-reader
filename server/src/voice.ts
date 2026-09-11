// El botón de voz del hub: una sola grabación, el servidor decide qué es.
//
//   POST /api/voice   (Bearer del aparato; body audio/wav 16 kHz mono)
//   200: cuerpo binario "application/x-ws397-voice": [uint32 LE largo del JSON][JSON][audio ADPCM opcional]
//        JSON = { ok: true, text, intent, reply, saved?: [{ kind, list?, title, when? }], timerSeconds, audio: bytes }
//        El audio (Piper, ver tts.ts) es la reply hablada cuando es corta (o una traducción / confirmación):
//        viaja en el mismo pedido para que suene junto con el texto, sin una segunda conexión TLS.
//     text   = lo que se entendió
//     intent = question | reminder | task | shopping | note | timer | alarm | translate
//     reply  = texto corto para la pantalla (y para leer por el parlante cuando haya TTS)
//   4xx/5xx: { ok: false, error }
//
// Claude clasifica con salida estructurada (JSON con esquema) y en la misma
// llamada redacta la respuesta: una pregunta se contesta con conocimiento
// general; un recordatorio, tarea, compra o nota se guarda en el
// store y se confirma; temporizador y alarma todavía no se ejecutan en el
// aparato, así que se avisa. Una grabación puede traer una acción y una
// pregunta a la vez: se hacen las dos.
//
// Modelo: el que esté elegido en /board → Ajustes (Claude, Groq, DeepSeek u otra
// API compatible con OpenAI). Ver src/llm.ts y src/config.ts.
import { Hono } from "hono";
import Anthropic from "@anthropic-ai/sdk";
import { transcribeWav, toWav, NoSpeechError, NO_SPEECH, NO_SPEECH_MSG } from "./transcribe";
import { load, mutate, nextId, resolveList, listLabel, whenLabel, pendingReminders, localToEpoch, epochToLocal, advanceRepeat, normalizeRepeat, repeatText, repeatToWire, alignToRepeat, NO_REPEAT, memoryLines, rememberFact, timeZone, DEFAULT_LISTS, SHOPPING_LIST, type Repeat } from "./store";
import { LANGUAGE_NAME, defaultTranslateTarget, normalizeLang, type Lang } from "./lang";
import { synthesize } from "./tts";
import { chatJson, chatText, chatSearch, LlmError } from "./llm";
import { sourcesLine } from "./websearch";
import { redactSecrets } from "./net";
import type { ContentfulStatusCode } from "hono/utils/http-status";
import { accountOf, type AppEnv } from "./tenant";

// La zona es la de la cuenta del pedido (el lugar que eligió para el clima).

export const voice = new Hono<AppEnv>();

function framed(json: object, audio: Uint8Array | null): Response {
  const head = Buffer.from(JSON.stringify(json), "utf8");
  const len = Buffer.alloc(4);
  len.writeUInt32LE(head.length, 0);
  const body = Buffer.concat([len, head, audio ? Buffer.from(audio) : Buffer.alloc(0)]);
  return new Response(body, { headers: { "Content-Type": "application/x-ws397-voice", "Content-Length": String(body.length) } });
}

const SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["intent", "reply", "needsWeb", "actions"],
  properties: {
    intent: {
      type: "string",
      enum: ["question", "reminder", "task", "shopping", "note", "timer", "alarm", "translate", "memory"],
      description: "Intención principal de lo dicho.",
    },
    reply: {
      type: "string",
      description:
        "Texto para la pantalla: la respuesta si es pregunta o traducción, o una confirmación de una línea de lo guardado. Texto plano, sin markdown.",
    },
    needsWeb: {
      type: "boolean",
      description:
        "true SOLO si para contestar bien hace falta información actual de internet (resultados deportivos, precios y cotizaciones, noticias, quién ocupa un cargo hoy, estrenos, versiones, algo posterior a tu entrenamiento). false para todo lo demás, incluidas las órdenes y las preguntas de conocimiento general estable.",
    },
    actions: {
      type: "array",
      description: "Lo que hay que guardar. Vacío si es solo una pregunta.",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["kind", "text", "list", "dueAt", "repeat", "seconds"],
        properties: {
          kind: { type: "string", enum: ["reminder", "task", "shopping", "note", "timer", "alarm", "memory"] },
          text: { type: "string", description: "Título de la tarea/recordatorio, ítem de compra o texto de la nota." },
          list: {
            type: ["string", "null"],
            description: "Solo hay dos listas y no se pueden crear más: la de compras y la de tareas. Poné el nombre de una de esas dos, o null.",
          },
          dueAt: {
            type: ["string", "null"],
            description: "Fecha y hora local del recordatorio o vencimiento como YYYY-MM-DDTHH:MM, o YYYY-MM-DD si no dijo hora; null si no tiene.",
          },
          repeat: {
            type: "object",
            additionalProperties: false,
            required: ["kind", "days", "interval", "until"],
            description: "Cada cuánto se repite. Si el usuario no dice nada de repetir, kind = none.",
            properties: {
              kind: {
                type: "string",
                enum: ["none", "daily", "weekdays", "weekly", "monthly", "yearly"],
                description:
                  "none = una sola vez. daily = todos los días. weekdays = de lunes a viernes (días hábiles). " +
                  "weekly = uno o varios días de la semana. monthly = el mismo día de cada mes. yearly = una vez al año.",
              },
              days: {
                type: ["array", "null"],
                items: { type: "integer" },
                description: "Solo para weekly: días de la semana, 0=domingo, 1=lunes ... 6=sábado. null si no nombró días.",
              },
              interval: {
                type: ["integer", "null"],
                description: "Cada cuántos días/semanas/meses/años ('cada dos semanas' = 2). null o 1 si no dijo.",
              },
              until: {
                type: ["string", "null"],
                description: "Hasta cuándo se repite, YYYY-MM-DD, si el usuario lo dijo ('hasta fin de mes'); null si no.",
              },
            },
          },
          seconds: { type: ["integer", "null"], description: "Duración en segundos para timer; null si no aplica." },
        },
      },
    },
  },
} as const;

// La memoria del usuario NO va en este texto: viaja en la opción `memories` de
// llm.ts, que la pone en el bloque cacheado (ver store.ts / llm.ts).
function systemPrompt(now: string, weekday: string, lists: string[], lang: Lang): string {
  return [
    "Sos el asistente por voz de un aparato de tinta electrónica sin teclado. Recibís una frase transcripta",
    "de voz (puede traer errores de reconocimiento; interpretala con sentido común y no comentes la transcripción)",
    "y devolvés JSON según el esquema.",
    `Ahora es ${now} (${weekday}), zona ${timeZone()}. Resolvé fechas relativas (mañana, el jueves, la semana que viene) a fecha absoluta.`,
    `Hay exactamente DOS listas y no se pueden crear más: "${lists[0]}" (lo que se compra) y "${lists[1]}" (todo lo demás por hacer).`,
    "Si el usuario nombra cualquier otra lista, ignorá ese nombre: lo que sea una compra va a la de compras y todo lo demás a la de tareas.",
    "Reglas: 'recordame', 'avisame', 'despertame' → reminder. En dueAt poné la hora SOLO si el usuario la dijo; si dijo",
    "el día pero no la hora ('mañana', 'el jueves'), poné la fecha sola (YYYY-MM-DD, sin T) y NUNCA inventes una hora.",
    "'Comprar X', 'compras:' o",
    "artículos sueltos → shopping, un ítem por producto (\"leche y huevos\" son dos acciones). 'Agregá', 'anotá que tengo que',",
    "'tengo que', 'hay que' → task (va a la lista de tareas). 'Nota:', 'anotá' → note.",
    "'Poné N minutos', 'temporizador', 'pomodoro' (25 min) → timer con seconds y reply corta ('Listo, 10 minutos'). " +
    "OJO con la unidad: `seconds` va SIEMPRE en SEGUNDOS. '20 segundos' → 20 (no 1200). '10 minutos' → 600. " +
    "'un minuto y medio' → 90. 'media hora' → 1800. 'pomodoro' → 1500. Repetí en la reply la misma unidad que dijo el usuario.",
    "'Alarma a las', 'despertame a las' → alarm con dueAt (la próxima ocurrencia de esa hora) y reply corta.",
    "REPETICIONES: 'todos los días' → {kind:daily}. 'todos los días hábiles', 'de lunes a viernes', 'entre semana' →",
    "{kind:weekdays}. 'los lunes y miércoles', 'cada martes' → {kind:weekly, days:[1,3]} (0=domingo, 1=lunes ... 6=sábado).",
    "'cada dos semanas' → {kind:weekly, interval:2}; 'un día sí y uno no', 'cada dos días' → {kind:daily, interval:2}.",
    "'todos los meses', 'el 5 de cada mes' → {kind:monthly}; 'todos los años', cumpleaños y aniversarios → {kind:yearly}.",
    "'hasta fin de mes', 'hasta el viernes' → until con la fecha absoluta YYYY-MM-DD. Si no dice nada de repetir, kind = none.",
    "Con una repetición semanal poné en dueAt el PRIMER día que corresponde (el próximo de esos días) con la hora dicha.",
    "'Acordate que', 'tené presente que', 'mi ... es ...' (un dato sobre el usuario o su vida) → memory con text = el dato en una frase.",
    "Si está corrigiendo algo que ya sabés de él ('ya no vivo en México', 'ahora trabajo en otro lado'), también es memory:",
    "escribí el dato NUEVO completo en text y el servidor pisa el viejo.",
    "needsWeb va en true SOLO si el usuario PIDIO EXPRESAMENTE que busque en internet",
    "(busca, busca en internet, fijate en internet, averigua). Que la pregunta sea de actualidad NO alcanza:",
    "si no pidio buscar, contesta con lo que sabes y aclara que el dato puede estar desactualizado.",
    "Buscar cuesta plata y el usuario pidio decidirlo el. En todo lo demas va false.",
    `'Traducí', 'cómo se dice' (o su equivalente en el idioma del usuario) → translate y reply es SOLO la traducción, al idioma que pida; si no dice a cuál, a ${defaultTranslateTarget(lang)}. Cualquier otra cosa (duda, dato, explicación) → question`,
    "y reply la contesta con conocimiento general, corta y directa. Si la frase trae una acción y una pregunta, guardá la",
    "acción en actions y contestá la pregunta en reply. Si es ambiguo entre acción y pregunta, elegí task y decilo.",
    `El usuario habla en ${LANGUAGE_NAME[lang]}: los títulos de las acciones y reply van en ese idioma (salvo la traducción). Texto plano, sin markdown ni listas. Máximo 120 palabras salvo que pida más.`,
  ].join(" ");
}

const ASK_TIME: Record<Lang, string> = {
  es: "¿A qué hora te lo recuerdo?",
  en: "At what time should I remind you?",
  fr: "À quelle heure je te le rappelle ?",
  de: "Um wie viel Uhr soll ich dich erinnern?",
  pt: "A que horas te lembro?",
  ru: "Во сколько напомнить?",
};

// Respuesta a "¿a qué hora?": "a las nueve", "14:30", "ocho y media". Se
// resuelve sin LLM cuando alcanza con los dígitos, y con él si no.
// Desfase de la zona de la cuenta respecto de UTC, para saber qué hora es "ahora" en casa.
function tzOffsetMsLocal(): number {
  const tz = timeZone();
  const at = Date.now();
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: tz, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(at));
  const get = (t: string) => Number(parts.find((p) => p.type === t)?.value ?? 0);
  return Date.UTC(get("year"), get("month") - 1, get("day"), get("hour"), get("minute"), get("second")) - Math.floor(at / 1000) * 1000;
}

async function parseTimeReply(text: string, lang: Lang, baseDate = ""): Promise<string | null> {
  // El día es el que ya había dicho ("mañana"); si no dijo ninguno, hoy cuando la
  // hora todavía no pasó y mañana si ya pasó.
  const dayFor = (h: number, m: number) => {
    if (baseDate) return baseDate;
    const now = new Date(Date.now() + tzOffsetMsLocal());
    const past = h < now.getUTCHours() || (h === now.getUTCHours() && m <= now.getUTCMinutes());
    return new Date(Date.now() + (past ? 86_400_000 : 0) + tzOffsetMsLocal()).toISOString().slice(0, 10);
  };
  const t = text.toLowerCase();
  const digits = /(\d{1,2})\s*[:.]?\s*(\d{2})?/.exec(t);
  if (digits) {
    let h = Number(digits[1]);
    const m = digits[2] ? Number(digits[2]) : 0;
    if (h >= 0 && h <= 23 && m >= 0 && m < 60) {
      if (/\b(pm|tarde|noche|abend|soir|вечера|нoчи)\b/.test(t) && h < 12) h += 12;
      return `${dayFor(h, m)}T${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`;
    }
  }
  try {
    const out = await chatText({
      system: "Devolvé solo la hora que dice el usuario en formato HH:MM de 24 horas, sin nada más. Si no se entiende, devolvé 09:00.",
      user: text,
      maxTokens: 40,
    });
    const m2 = /(\d{1,2}):(\d{2})/.exec(out);
    if (!m2) return null;
    const h = Number(m2[1]);
    const m = Number(m2[2]);
    return `${dayFor(h, m)}T${m2[1].padStart(2, "0")}:${m2[2]}`;
  } catch {
    return null;
  }
}

type RepeatAction = { kind?: string; days?: number[] | null; interval?: number | null; until?: string | null };
type Action = { kind: string; text: string; list: string | null; dueAt: string | null; repeat: RepeatAction | string | null; seconds: number | null };

// Lo que devuelve el modelo -> Repeat del store. El `until` viene como fecha
// ("hasta fin de mes") y el store lo guarda en epoch, así que se convierte acá.
export function repeatFromAction(raw: Action["repeat"]): Repeat {
  if (!raw || typeof raw === "string") return normalizeRepeat(raw);
  const until = typeof raw.until === "string" && /^\d{4}-\d{2}-\d{2}$/.test(raw.until) ? localToEpoch(raw.until + "T23:59") : 0;
  return normalizeRepeat({ kind: raw.kind, days: raw.days ?? undefined, interval: raw.interval ?? undefined, until: until || undefined });
}
type Parsed = { intent: string; reply: string; needsWeb: boolean; actions: Action[] };

async function classify(acc: number, text: string, lang: Lang): Promise<Parsed> {
  const store = await load(acc);
  const now = new Date();
  const local = now.toLocaleString("sv-SE", { timeZone: timeZone() }).slice(0, 16).replace(" ", "T");
  const weekday = now.toLocaleDateString("en-US", { weekday: "long", timeZone: timeZone() });
  const lists = DEFAULT_LISTS;  // son dos y son fijas: compras y tareas
  const raw = await chatJson<Partial<Parsed>>(
    { system: systemPrompt(local, weekday, lists, lang), memories: memoryLines(store), user: text, maxTokens: 1024 },
    SCHEMA,
  );
  // El esquema no obliga a nadie: un modelo compatible puede volver sin reply
  // (y `parsed.reply.length` era un 500) o con actions que no son array.
  return {
    intent: typeof raw.intent === "string" ? raw.intent : "question",
    reply: typeof raw.reply === "string" ? raw.reply : "",
    needsWeb: raw.needsWeb === true,
    actions: Array.isArray(raw.actions) ? raw.actions : [],
  };
}

// Segunda vuelta para las preguntas de actualidad: el clasificador ya dijo que
// hace falta internet, así que acá se contesta de nuevo con búsqueda (con
// Anthropic la hace el modelo; con las compatibles busca el servidor). Cuesta
// una llamada más, por eso solo se hace cuando el modelo lo pidió.
async function answerWithSearch(acc: number, question: string, lang: Lang): Promise<{ screen: string; spoken: string } | null> {
  try {
    const memories = memoryLines(await load(acc));
    const hoy = new Date().toLocaleDateString("es-AR", { timeZone: timeZone(), day: "2-digit", month: "long", year: "numeric" });
    const r = await chatSearch({
      memories,
      system: [
        `Hoy es ${hoy}.`,
        "Sos el asistente por voz de un aparato de tinta electrónica. La pregunta es sobre algo actual:",
        "buscá en internet antes de contestar en vez de responder de memoria y decí de cuándo es el dato.",
        "La pregunta llega transcripta de voz: puede traer errores; interpretala con sentido común.",
        `Idioma: ${LANGUAGE_NAME[lang]}. Texto plano, sin markdown ni listas. Máximo 90 palabras.`,
      ].filter(Boolean).join(" "),
      user: question,
      maxTokens: 800,
      search: "force",
      lang,
    });
    const spoken = r.text.trim();
    if (!spoken) return null;
    // Las fuentes van a la pantalla, no al parlante: nadie quiere escuchar
    // "punto com" al final de cada respuesta.
    const line = r.searched ? sourcesLine(r.sources, lang) : "";
    return { screen: [spoken, line].filter(Boolean).join("\n\n"), spoken };
  } catch (err) {
    console.error("voice búsqueda:", err);
    return null;
  }
}

async function execute(acc: number, parsed: Parsed, spoken: string, lang: Lang) {
  const saved: { kind: string; list?: string; title: string; when?: string; repeatText?: string }[] = [];
  const stamp = new Date().toISOString();
  // Bajo candado y sin ninguna llamada al modelo adentro: lo que hay acá es
  // puro armado de objetos, así que la transacción dura microsegundos.
  await mutate(acc, (store) => {
  for (const a of parsed.actions ?? []) {
    const title = (a.text ?? "").trim().slice(0, 200);
    if (!title) continue;
    switch (a.kind) {
      case "reminder": {
        const dueAt = a.dueAt && /^\d{4}-\d{2}-\d{2}/.test(a.dueAt) ? a.dueAt : null;
        const repeat = repeatFromAction(a.repeat);
        // "Los martes y jueves" dicho un lunes arranca el martes.
        const aligned = dueAt ? alignToRepeat(dueAt.slice(0, 10), repeat) + dueAt.slice(10) : null;
        store.reminders.push({ id: nextId(store), title, dueAt: aligned, repeat, done: false, createdAt: stamp });
        saved.push({ kind: "reminder", title, when: whenLabel(aligned, lang), repeatText: repeatText(repeat, aligned, lang) });
        break;
      }
      case "task": {
        const list = resolveList(store, a.list);
        store.lists[list].push({ id: nextId(store), text: title, done: false, dueDate: a.dueAt ? a.dueAt.slice(0, 10) : null, createdAt: stamp });
        saved.push({ kind: "task", list: listLabel(list, lang), title });
        break;
      }
      case "shopping": {
        const list = resolveList(store, a.list ?? SHOPPING_LIST);
        store.lists[list].push({ id: nextId(store), text: title, done: false, dueDate: null, createdAt: stamp });
        saved.push({ kind: "shopping", list: listLabel(list, lang), title });
        break;
      }
      // "message" ya no existe como intención (se sacó la pizarra del producto);
      // si un modelo viejo o terco la devuelve igual, se guarda como nota.
      case "note":
      case "message":
        store.notes.push({ id: nextId(store), text: title, createdAt: stamp });
        saved.push({ kind: "note", title });
        break;
      case "memory": {
        // rememberFact pisa la memoria más parecida: "ya no vivo en México" no
        // puede quedar guardado al lado de "vivo en México".
        const { replaced } = rememberFact(store, title);
        if (replaced) console.log(`voice: memoria actualizada, pisa «${replaced}»`);
        saved.push({ kind: "memory", title });
        break;
      }
      case "alarm": {
        // Una alarma es un recordatorio: despierta el aparato por el timer de
        // deep sleep y suena aunque esté dormido.
        const dueAt = a.dueAt && /^\d{4}-\d{2}-\d{2}T/.test(a.dueAt) ? a.dueAt : null;
        if (!dueAt) break;
        const repeat = repeatFromAction(a.repeat);
        const aligned = alignToRepeat(dueAt.slice(0, 10), repeat) + dueAt.slice(10);
        store.reminders.push({ id: nextId(store), title: title || "Alarma", dueAt: aligned, repeat, done: false, createdAt: stamp });
        saved.push({ kind: "reminder", title: title || "Alarma", when: whenLabel(aligned, lang), repeatText: repeatText(repeat, aligned, lang) });
        break;
      }
      default:
        break; // timer corre en el aparato (timerSeconds)
    }
  }
  });
  console.log(`voice: "${spoken}" -> ${parsed.intent}`, saved.map((s) => `${s.kind}:${s.title}`).join(" | "));
  return saved;
}

voice.post("/", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const speak = c.req.query("speak") ?? "short";  // none | short | all (ajuste del aparato)
  // Segunda vuelta cuando le preguntamos la hora de un recordatorio: el
  // aparato reenvía el título pendiente y esta grabación es solo la hora.
  const pending = (c.req.query("pending") ?? "").slice(0, 200);
  // Día que ya había dicho el usuario en el primer turno ("mañana"), si lo dijo.
  const pendingDateRaw = (c.req.query("pendingDate") ?? "").slice(0, 10);
  const pendingDate = /^\d{4}-\d{2}-\d{2}$/.test(pendingDateRaw) ? pendingDateRaw : "";
  // Repetición que ya se había entendido en el primer turno, como JSON.
  let pendingRepeat = NO_REPEAT;
  try {
    const raw = c.req.query("pendingRepeat");
    if (raw) pendingRepeat = normalizeRepeat(JSON.parse(raw));
  } catch { /* lo que no parsea es "sin repetición" */ }
  const t0 = Date.now();
  let text: string;
  try {
    text = await transcribeWav(toWav(await c.req.arrayBuffer(), c.req.header("content-type")), lang);
  } catch (err) {
    // Nadie habló (o Whisper inventó lo de amara.org con el silencio): se
    // contesta "no escuché nada" y NO se le pregunta nada al modelo. Antes esa
    // frase inventada entraba como si el usuario la hubiera dicho y el aparato
    // devolvía una respuesta sobre subtítulos.
    if (err instanceof NoSpeechError) {
      const reply = NO_SPEECH_MSG[lang];
      const audio = speak !== "none" ? await synthesize(reply, lang, 6) : null;
      console.log(`voice: sin voz (${err.why})`);
      return framed(
        { ok: true, text: "", intent: NO_SPEECH, reply, saved: [], timerSeconds: 0,
          audio: audio?.length ?? 0, ms: { stt: Date.now() - t0, total: Date.now() - t0 } },
        audio,
      );
    }
    const msg = err instanceof Error ? err.message : "internal";
    return c.json({ ok: false, error: msg }, msg.startsWith("stt ") ? 502 : 400);
  }
  const tStt = Date.now();
  try {
    if (pending) {
      const dueAt = await parseTimeReply(text, lang, pendingDate);
      const aligned = dueAt ? alignToRepeat(dueAt.slice(0, 10), pendingRepeat) + dueAt.slice(10) : null;
      // parseTimeReply (que llama al modelo) queda AFUERA del candado.
      await mutate(acc, (store) => {
        store.reminders.push({ id: nextId(store), title: pending, dueAt: aligned, repeat: pendingRepeat, done: false, createdAt: new Date().toISOString() });
      });
      const label = whenLabel(aligned, lang);
      const repText = repeatText(pendingRepeat, aligned, lang);
      const reply = aligned ? `${pending} — ${label}` : pending;
      const audio = speak !== "none" ? await synthesize(reply, lang, 8) : null;
      console.log(`voice: hora de "${pending}" -> ${aligned} (${repText})`);
      return framed({ ok: true, text, intent: "reminder", reply, saved: [{ kind: "reminder", title: pending, when: label, repeatText: repText }], timerSeconds: 0, audio: audio?.length ?? 0, ms: { stt: tStt - t0, total: Date.now() - t0 } }, audio);
    }
    const parsed = await classify(acc, text, lang);
    // Recordatorio sin hora (con día o sin día): se pregunta en vez de inventarla,
    // y no se guarda nada todavía — antes execute() ya lo había guardado y el
    // segundo turno creaba un duplicado.
    const needsTime = (parsed.actions ?? []).find(
      (a) => (a.kind === "reminder" || a.kind === "alarm") && (!a.dueAt || !a.dueAt.includes("T")),
    );
    if (needsTime) {
      const reply = ASK_TIME[lang];
      const audio = speak !== "none" ? await synthesize(reply, lang, 6) : null;
      const day = needsTime.dueAt ? needsTime.dueAt.slice(0, 10) : "";
      // La repetición viaja en la respuesta para que el aparato la devuelva en
      // el segundo turno (?pendingRepeat=): si no, "recordame todos los días
      // sacar la basura" perdía el "todos los días" al preguntar la hora.
      const askRepeat = repeatFromAction(needsTime.repeat);
      console.log(`voice: falta la hora de "${needsTime.text}"${day ? ` (${day})` : ""}`);
      return framed(
        { ok: true, text, intent: "reminder", reply, askTime: needsTime.text, askDate: day, saved: [], timerSeconds: 0,
          askRepeat, askRepeatText: repeatText(askRepeat, needsTime.dueAt, lang),
          audio: audio?.length ?? 0, ms: { stt: tStt - t0, total: Date.now() - t0 } },
        audio,
      );
    }
    // Pregunta de actualidad: se vuelve a contestar con búsqueda.
    let spokenReply = parsed.reply;
    if (parsed.intent === "question" && parsed.needsWeb && !(parsed.actions ?? []).length) {
      const better = await answerWithSearch(acc, text, lang);
      if (better) {
        parsed.reply = better.screen;
        spokenReply = better.spoken;
      }
    }
    const saved = await execute(acc, parsed, text, lang);
    // Temporizador y alarma corren en el aparato: segundos hasta que suene.
    let timerSeconds = 0;
    for (const a of parsed.actions ?? []) {
      if (a.kind === "timer" && a.seconds && a.seconds > 0) timerSeconds = fixTimerUnit(Math.floor(a.seconds), text);
    }
    // Voz: confirmaciones y traducciones siempre; una respuesta a pregunta solo
    // si es corta (el resto se lee en pantalla). Máximo 8 s para que el aparato
    // la baje en menos de medio segundo.
    const tLlm = Date.now();
    const speakable = speak !== "none" && (speak === "all" || parsed.intent !== "question" || spokenReply.length <= 220);
    const audio = speakable ? await synthesize(spokenReply, lang, speak === "all" ? 45 : 20) : null;
    // 45 s con "leer siempre": con 15 se cortaba a mitad de la primera pantalla.
    // No mas, porque el aparato se guarda el audio ENTERO en memoria antes de
    // reproducirlo (45 s de ADPCM son ~360 KB) y no sabe reproducir mientras baja.
    const ms = { stt: tStt - t0, llm: tLlm - tStt, tts: Date.now() - tLlm, total: Date.now() - t0 };
    console.log(`voice ms: stt=${ms.stt} llm=${ms.llm} tts=${ms.tts} total=${ms.total}`);
    return framed({ ok: true, text, intent: parsed.intent, reply: parsed.reply, saved, timerSeconds, audio: audio?.length ?? 0, ms }, audio);
  } catch (err) {
    // Igual que en ask.ts: el aparato tiene que poder distinguir "falta la
    // clave" de "el proveedor falló".
    if (err instanceof LlmError) {
      console.error("voice llm:", err.message);
      return c.json({ ok: false, error: err.message, code: err.code }, err.status as ContentfulStatusCode);
    }
    if (err instanceof Anthropic.RateLimitError) return c.json({ ok: false, error: "rate limited", code: "rate_limited" }, 429);
    if (err instanceof Anthropic.AuthenticationError) return c.json({ ok: false, error: "falta o no sirve la clave del modelo", code: "no_key" }, 500);
    if (err instanceof Anthropic.APIError) return c.json({ ok: false, error: `modelo ${err.status}: ${redactSecrets(err.message)}`, code: "provider_error" }, 502);
    console.error("voice:", err);
    return c.json({ ok: false, error: redactSecrets(String(err)).slice(0, 200), code: "internal" }, 500);
  }
});

// Lo que el hub muestra y cachea: recordatorios pendientes (el primero es el
// próximo), las dos listas con sus ítems pendientes y las notas.
export async function hubSlice(accountId: number, lang: Lang) {
  const store = await load(accountId);
  return {
    // La repetición va de dos formas: `repeat`/`weekday`/`interval` es lo que
    // lee y edita el firmware (HubStore::Reminder), `repeatSpec` es el objeto
    // completo (lo usa /board, que sí puede con varios días), y `repeatText` es
    // la frase ya traducida que se muestra debajo del título — es lo que
    // contesta "¿me despierta mañana o de lunes a viernes?" sin interpretar nada.
    reminders: pendingReminders(store).slice(0, 20).map((r) => ({
      id: r.id,
      title: r.title,
      when: whenLabel(r.dueAt, lang),
      dueAt: localToEpoch(r.dueAt),
      at: r.dueAt,
      ...repeatToWire(r.repeat, r.dueAt),
      repeatSpec: normalizeRepeat(r.repeat),
      repeatText: repeatText(r.repeat, r.dueAt, lang),
    })),
    // `name` es el nombre visible en el idioma del aparato; `key` es la clave
    // canónica del store, que es lo que hay que devolver al mover un ítem.
    lists: DEFAULT_LISTS.map((key) => ({
      key,
      name: listLabel(key, lang),
      items: (store.lists[key] ?? []).filter((i) => !i.done).slice(0, 30).map((i) => ({ id: i.id, text: i.text })),
    })),
    notes: store.notes.slice(-20).reverse().map((n) => ({ id: n.id, text: n.text })),
  };
}

// Ediciones desde el aparato (menú de la lista: mover, fecha, borrar; borrar nota).
// Red de contención de la unidad del temporizador: el clasificador a veces
// devuelve "20 segundos" como 1200 (multiplica por 60 igual). Si el usuario dijo
// segundos y no dijo minutos ni horas, y el número es un múltiplo redondo de 60,
// se divide. Al revés no hace falta: decir minutos y que devuelva segundos
// sueltos no se vio nunca.
const SECOND_WORDS = /\b(segundos?|seconds?|secondes?|sekunden?|segundos?|секунд\w*)\b/i;
const MINUTE_WORDS = /\b(minutos?|minutes?|minuten?|hora|horas|hour|hours|stunde\w*|час\w*|мин\w*)\b/i;

export function fixTimerUnit(seconds: number, said: string): number {
  const capped = Math.min(seconds, 24 * 3600);
  if (SECOND_WORDS.test(said) && !MINUTE_WORDS.test(said) && capped >= 60 && capped % 60 === 0) {
    return capped / 60;
  }
  return capped;
}

export async function editEntry(accountId: number, body: { kind?: string; id?: number; action?: string; list?: string; dueDate?: string | null }): Promise<boolean> {
  // Todo adentro del candado: antes era leer, filtrar y guardar el documento
  // entero, así que un borrado y un alta a la vez se pisaban.
  return mutate(accountId, (store) => {
  const id = Number(body.id);
  if (body.kind === "feed") {
    const before = (store.feeds ?? []).length;
    store.feeds = (store.feeds ?? []).filter((f) => f.id !== id);
    return store.feeds.length !== before;
  }
  if (body.kind === "memory") {
    const before = (store.memories ?? []).length;
    store.memories = (store.memories ?? []).filter((x) => x.id !== id);
    return store.memories.length !== before;
  }
  if (body.kind === "reminder") {
    const before = store.reminders.length;
    store.reminders = store.reminders.filter((r) => r.id !== id);
    return store.reminders.length !== before;
  }
  if (body.kind === "note") {
    const before = store.notes.length;
    store.notes = store.notes.filter((n) => n.id !== id);
    return store.notes.length !== before;
  }
  for (const [name, items] of Object.entries(store.lists)) {
    const idx = items.findIndex((i) => i.id === id);
    if (idx < 0) continue;
    const item = items[idx];
    if (body.action === "delete") {
      items.splice(idx, 1);
    } else if (body.action === "move") {
      const target = resolveList(store, body.list);
      if (target !== name) {
        items.splice(idx, 1);
        store.lists[target].push(item);
      }
    } else if (body.action === "date") {
      item.dueDate = body.dueDate && /^\d{4}-\d{2}-\d{2}$/.test(body.dueDate) ? body.dueDate : null;
    } else {
      return false;
    }
    return true;
  }
  return false;
  });
}

// Tildar (o posponer `snoozeSeconds`) desde el aparato.
//
// Idempotente, pero no lo era del todo: para un recordatorio CON repetición,
// `advanceRepeat()` corre la fecha cada vez que se entra, sin comparar contra
// nada. El aparato reintenta hasta tres veces (y la cola offline reproduce de
// nuevo) con el mismo pedido, así que un reintento que llega porque se perdió
// la RESPUESTA —no el pedido— avanzaba un ciclo de más: una ocurrencia que el
// usuario nunca ve, sin error y sin rastro.
//
// `at` es el `dueAt` de la ocurrencia que el aparato está tildando (viaja en
// `GET /api/hub` como epoch UTC). Un replay llega con el `at` de la ocurrencia
// vieja, que ya no coincide con la del store, y no avanza nada. Solo se aplica
// a los repetidos: sin `at` (web, firmware viejo) se comporta como antes.
export async function markDone(accountId: number, kind: "reminder" | "item", id: number, snoozeSeconds = 0, at = 0): Promise<boolean> {
  return mutate(accountId, (store) => {
  let found = false;
  if (kind === "reminder") {
    for (const r of store.reminders) {
      if (r.id !== id) continue;
      if (at > 0 && snoozeSeconds === 0 && normalizeRepeat(r.repeat).kind !== "none" && r.dueAt && localToEpoch(r.dueAt) !== at) {
        // Ya se aplicó: esto es el reintento del mismo tilde.
        found = true;
        continue;
      }
      found = true;
      if (snoozeSeconds > 0) r.dueAt = epochToLocal(Math.floor(Date.now() / 1000) + snoozeSeconds);
      else if (!advanceRepeat(r)) r.done = true;
    }
  } else {
    for (const items of Object.values(store.lists)) for (const i of items) if (i.id === id) { i.done = true; found = true; }
  }
  return found;
  });
}
