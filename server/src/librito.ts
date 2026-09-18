// Librito: un EPUB a medida para leer en quince minutos (app de Lua `librito`).
//
// Cuatro servicios, los que cumple `apps.ts` bajo POST /api/apps/call:
//   librito.enfoque   {tema}                       → tres formas de encarar el tema
//   librito.indice    {tema, enfoque, minutos?}    → título y 5 a 8 capítulos calibrados
//   librito.ajustar   {…, capitulos[{activo}], pedido} → el índice con el pedido dictado aplicado
//   librito.escribir  {titulo, tema, enfoque, capitulos, minutos} → un trabajo (jobId)
//
// Los tres primeros son síncronos y con salida estructurada (menos de 25 s).
// El cuarto tarda minutos: escribe capítulo por capítulo con streaming, con el
// índice entero y un resumen de lo ya escrito en cada llamada para que no se
// repita ni se contradiga, cierra con "Para seguir leyendo" y arma el EPUB a
// mano (epub.ts). El aparato pregunta `job.status` y se baja el archivo.
//
// El largo lo manda el usuario en minutos (mínimo 15) a 200 palabras por
// minuto; la cuenta de palabras por capítulo la cierra el servidor
// (`calibrate`), porque el modelo suma mal más veces de las que uno cree.
import type Anthropic from "@anthropic-ai/sdk";
import { appsJson, appsProse } from "./appsLlm";
import { startJob, type JobFile } from "./appsJobs";
import { buildEpub, slugify } from "./epub";
import { LANGUAGE_NAME, type Lang } from "./lang";
// Solo el tipo: `apps.ts` importa la tabla de acá y esto importa el tipo de
// allá; en tiempo de ejecución el import se borra y no hay ciclo.
import type { Service } from "./apps";

const WORDS_PER_MINUTE = 200;
const MIN_MINUTES = 15;
const MAX_MINUTES = 120;
const MIN_CHAPTERS = 5;
const MAX_CHAPTERS = 8;
const MIN_CHAPTER_WORDS = 150;
// Tokens por palabra con margen, y un colchón para el pensamiento adaptativo,
// que sale del mismo `max_tokens`: sin él un capítulo largo se cortaba a mitad.
const TOKENS_PER_WORD = 2.2;
const MIN_PROSE_TOKENS = 1500;
const THINKING_HEADROOM = 1000;

// Lo que ve el usuario en el aparato, por idioma (español neutro).
const T: Record<Lang, { chapter: (n: number, total: number, title: string) => string; closing: string; closingStep: string; building: string; librito: string; untitled: string }> = {
  es: { chapter: (n, t, s) => `Capítulo ${n} de ${t}: ${s}`, closing: "Para seguir leyendo", closingStep: "Cierre", building: "Armando el libro", librito: "Librito", untitled: "Sin título" },
  en: { chapter: (n, t, s) => `Chapter ${n} of ${t}: ${s}`, closing: "Further reading", closingStep: "Closing", building: "Building the book", librito: "Booklet", untitled: "Untitled" },
  fr: { chapter: (n, t, s) => `Chapitre ${n} sur ${t} : ${s}`, closing: "Pour aller plus loin", closingStep: "Conclusion", building: "Assemblage du livre", librito: "Livret", untitled: "Sans titre" },
  de: { chapter: (n, t, s) => `Kapitel ${n} von ${t}: ${s}`, closing: "Zum Weiterlesen", closingStep: "Schluss", building: "Buch wird zusammengestellt", librito: "Büchlein", untitled: "Ohne Titel" },
  pt: { chapter: (n, t, s) => `Capítulo ${n} de ${t}: ${s}`, closing: "Para continuar lendo", closingStep: "Encerramento", building: "Montando o livro", librito: "Livrinho", untitled: "Sem título" },
  ru: { chapter: (n, t, s) => `Глава ${n} из ${t}: ${s}`, closing: "Что почитать дальше", closingStep: "Заключение", building: "Сборка книги", librito: "Книжечка", untitled: "Без названия" },
};

export type Enfoque = { id: string; titulo: string; linea: string };
export type Capitulo = { n: number; titulo: string; linea: string; palabras: number };

// ── Validación de argumentos ───────────────────────────────────────────────

function str(v: unknown, max: number): string {
  return typeof v === "string" ? v.replace(/\s+/g, " ").trim().slice(0, max) : "";
}

function minutesOf(v: unknown): number {
  const n = Math.round(Number(v));
  if (!Number.isFinite(n) || n < MIN_MINUTES) return MIN_MINUTES;
  return Math.min(MAX_MINUTES, n);
}

function needText(v: unknown, max: number, what: string): string {
  const s = str(v, max);
  if (!s) throw new Error(`falta ${what}`);
  return s;
}

// ── La cuenta de palabras la cierra el servidor ────────────────────────────

/**
 * Reparte `total` palabras entre los capítulos en proporción a lo que pidió
 * cada uno (o parejo si no pidió nada), en múltiplos de 10 y con un piso por
 * capítulo. La suma da exactamente `total`: el resto va al último.
 */
export function calibrate(caps: { titulo: string; linea: string; palabras?: number }[], total: number): Capitulo[] {
  if (!caps.length) return [];
  const weights = caps.map((c) => Math.max(1, Number(c.palabras) || 0));
  const allZero = caps.every((c) => !(Number(c.palabras) > 0));
  const sum = allZero ? caps.length : weights.reduce((a, b) => a + b, 0);
  const out: Capitulo[] = caps.map((c, i) => ({
    n: i + 1,
    titulo: c.titulo,
    linea: c.linea,
    palabras: Math.max(MIN_CHAPTER_WORDS, Math.round(((allZero ? 1 : weights[i]) / sum) * total / 10) * 10),
  }));
  const got = out.reduce((a, c) => a + c.palabras, 0);
  const last = out[out.length - 1];
  last.palabras = Math.max(MIN_CHAPTER_WORDS, last.palabras + (total - got));
  return out;
}

type RawCap = { titulo?: unknown; linea?: unknown; palabras?: unknown; activo?: unknown };

function cleanCaps(raw: unknown, max = MAX_CHAPTERS): { titulo: string; linea: string; palabras?: number; activo: boolean }[] {
  if (!Array.isArray(raw)) return [];
  const out: { titulo: string; linea: string; palabras?: number; activo: boolean }[] = [];
  for (const c of raw as RawCap[]) {
    if (!c || typeof c !== "object") continue;
    const titulo = str(c.titulo, 50);
    if (!titulo) continue;
    const palabras = Number(c.palabras);
    out.push({
      titulo,
      linea: str(c.linea, 100),
      palabras: Number.isFinite(palabras) && palabras > 0 ? Math.round(palabras) : undefined,
      activo: c.activo !== false,
    });
    if (out.length >= max) break;
  }
  return out;
}

// ── Prompts ────────────────────────────────────────────────────────────────

function editorSystem(lang: Lang): string {
  return [
    "Eres el editor de una colección de libritos de divulgación: textos serios, bien documentados y amenos, pensados para leerse de corrido en unos quince minutos.",
    "El usuario dicta por voz, así que el tema puede venir coloquial, incompleto o con errores de transcripción: interpreta lo que quiso decir.",
    `Todo lo que devuelvas —títulos, líneas, descripciones— va en ${LANGUAGE_NAME[lang]}. Texto plano, sin markdown ni comillas decorativas.`,
  ].join(" ");
}

const ENFOQUE_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["tema", "enfoques"],
  properties: {
    tema: { type: "string", description: "El tema normalizado como título corto (hasta 60 caracteres), con mayúscula inicial y sin punto final." },
    enfoques: {
      type: "array",
      description: "Exactamente tres formas realmente distintas de encarar el tema, cada una viable en unas 3000 palabras.",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["id", "titulo", "linea"],
        properties: {
          id: { type: "string", description: "Identificador corto en minúsculas y sin espacios (por ejemplo 'causas', 'vida-cotidiana', 'legado')." },
          titulo: { type: "string", description: "Nombre del enfoque, hasta 40 caracteres." },
          linea: { type: "string", description: "Una línea de hasta 90 caracteres que dice qué cubriría ese enfoque." },
        },
      },
    },
  },
};

const INDICE_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: ["titulo", "capitulos"],
  properties: {
    titulo: { type: "string", description: "Título del librito, atractivo y concreto, hasta 60 caracteres." },
    capitulos: {
      type: "array",
      description: "Entre 5 y 8 capítulos en orden de lectura. Cada uno cubre una parte distinta del tema; entre todos lo agotan sin repetirse.",
      items: {
        type: "object",
        additionalProperties: false,
        required: ["titulo", "linea", "palabras"],
        properties: {
          titulo: { type: "string", description: "Título del capítulo, hasta 50 caracteres, sin numerar." },
          linea: { type: "string", description: "Una línea de hasta 100 caracteres con lo que cuenta el capítulo." },
          palabras: { type: "integer", description: "Largo del capítulo en palabras. La suma de todos los capítulos tiene que dar el total pedido." },
        },
      },
    },
  },
};

type IndiceRaw = { titulo?: unknown; capitulos?: unknown };

function finishIndex(raw: IndiceRaw, fallbackTitle: string, minutos: number, lang: Lang) {
  const caps = cleanCaps(raw.capitulos);
  if (caps.length < 2) throw new Error("el modelo no devolvió un índice");
  if (caps.length < MIN_CHAPTERS) console.warn(`librito: el modelo propuso ${caps.length} capítulos (se piden ${MIN_CHAPTERS} a ${MAX_CHAPTERS})`);
  const titulo = str(raw.titulo, 60) || fallbackTitle || T[lang].untitled;
  return { ok: true, titulo, minutos, capitulos: calibrate(caps, minutos * WORDS_PER_MINUTE) };
}

// ── Servicios ──────────────────────────────────────────────────────────────

const enfoque: Service = async (ctx, args) => {
  const tema = needText(args.tema, 300, "el tema");
  const raw = await appsJson<{ tema?: unknown; enfoques?: unknown }>({
    accountId: ctx.accountId,
    system: editorSystem(ctx.lang),
    user: `Tema dictado por el usuario: "${tema}"\n\nNormaliza el tema y propón tres enfoques distintos.`,
    schema: ENFOQUE_SCHEMA,
    maxTokens: 1500,
  });
  const list = Array.isArray(raw.enfoques) ? raw.enfoques : [];
  const enfoques: Enfoque[] = [];
  for (const e of list as { id?: unknown; titulo?: unknown; linea?: unknown }[]) {
    if (!e || typeof e !== "object") continue;
    const titulo = str(e.titulo, 40);
    if (!titulo) continue;
    const id = str(e.id, 24).toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, "") || `e${enfoques.length + 1}`;
    enfoques.push({ id: enfoques.some((x) => x.id === id) ? `${id}-${enfoques.length + 1}` : id, titulo, linea: str(e.linea, 90) });
    if (enfoques.length >= 3) break;
  }
  if (!enfoques.length) throw new Error("el modelo no propuso enfoques");
  return { ok: true, tema: str(raw.tema, 60) || tema.slice(0, 60), enfoques };
};

const indice: Service = async (ctx, args) => {
  const tema = needText(args.tema, 300, "el tema");
  const enfoqueTxt = str(args.enfoque, 200);
  const minutos = minutesOf(args.minutos);
  const total = minutos * WORDS_PER_MINUTE;
  const raw = await appsJson<IndiceRaw>({
    accountId: ctx.accountId,
    system: editorSystem(ctx.lang),
    user: [
      `Tema: ${tema}`,
      enfoqueTxt ? `Enfoque elegido: ${enfoqueTxt}` : "",
      `Largo total: ${total} palabras (${minutos} minutos de lectura).`,
      `Propón el título del librito y un índice de ${MIN_CHAPTERS} a ${MAX_CHAPTERS} capítulos, con el largo en palabras de cada uno. La suma tiene que dar ${total}.`,
    ].filter(Boolean).join("\n"),
    schema: INDICE_SCHEMA,
    maxTokens: 2500,
  });
  return finishIndex(raw, tema, minutos, ctx.lang);
};

const ajustar: Service = async (ctx, args) => {
  const tema = needText(args.tema, 300, "el tema");
  const enfoqueTxt = str(args.enfoque, 200);
  const minutos = minutesOf(args.minutos);
  const total = minutos * WORDS_PER_MINUTE;
  const pedido = str(args.pedido, 500);
  const titulo = str(args.titulo, 60);
  const activos = cleanCaps(args.capitulos, 20).filter((c) => c.activo);
  // Sin pedido no hay nada que preguntarle al modelo: se descartan los
  // inactivos y se reparten las palabras de nuevo para que el total no baje.
  if (!pedido) {
    if (!activos.length) throw new Error("no queda ningún capítulo");
    return { ok: true, titulo: titulo || tema.slice(0, 60), minutos, capitulos: calibrate(activos, total) };
  }
  const current = activos.length
    ? activos.map((c, i) => `${i + 1}. ${c.titulo} — ${c.linea}${c.palabras ? ` (${c.palabras} palabras)` : ""}`).join("\n")
    : "(el usuario sacó todos los capítulos)";
  const raw = await appsJson<IndiceRaw>({
    accountId: ctx.accountId,
    system: editorSystem(ctx.lang),
    user: [
      `Tema: ${tema}`,
      enfoqueTxt ? `Enfoque elegido: ${enfoqueTxt}` : "",
      titulo ? `Título actual: ${titulo}` : "",
      `Índice actual (solo los capítulos que el usuario conservó):\n${current}`,
      `El usuario dictó este ajuste: "${pedido}"`,
      `Aplícalo (agregar, quitar, cambiar, acortar o alargar capítulos, más temas, otro orden) y devuelve el índice completo resultante, de ${MIN_CHAPTERS} a ${MAX_CHAPTERS} capítulos. Conserva lo que el usuario no pidió cambiar.`,
      `Largo total: ${total} palabras como mínimo (${minutos} minutos). La suma de los capítulos tiene que dar ${total}.`,
    ].filter(Boolean).join("\n\n"),
    schema: INDICE_SCHEMA,
    maxTokens: 2500,
  });
  return finishIndex(raw, titulo || tema, minutos, ctx.lang);
};

// ── Escribir: el trabajo ───────────────────────────────────────────────────

function styleSystem(lang: Lang): string {
  return [
    "Eres el autor de un librito de divulgación: serio, documentado y ameno, para leerse de corrido en un lector de tinta electrónica.",
    `Escribes en ${LANGUAGE_NAME[lang]}.`,
    "Reglas de forma, sin excepción: prosa en párrafos separados por una línea en blanco; nada de listas, viñetas, tablas, markdown, negritas, encabezados ni títulos dentro del texto; no repitas el título del capítulo; no anuncies lo que vas a contar (nada de 'en este capítulo veremos') ni resumas al final lo que acabas de contar; no menciones el índice ni los otros capítulos por su número.",
    "Reglas de fondo: precisión ante todo; fechas, nombres y cifras solo cuando estés seguro, y cuando haya debate entre historiadores o científicos, dilo. Cuenta con ejemplos concretos y detalles que se recuerdan. Entra en materia desde la primera frase.",
    "Continuidad: recibirás el índice entero y un resumen de cada capítulo ya escrito. No repitas lo que ya se contó; retoma solo lo que haga falta para seguir el hilo.",
    "Al final del capítulo, en una línea aparte, escribe 'RESUMEN:' seguido de dos o tres frases con lo que contaste (esa línea es para el editor, no para el lector, y siempre empieza exactamente con RESUMEN:).",
  ].join("\n");
}

function indexBlock(titulo: string, tema: string, enfoqueTxt: string, caps: Capitulo[]): string {
  return [
    `Título del librito: ${titulo}`,
    `Tema: ${tema}`,
    enfoqueTxt ? `Enfoque: ${enfoqueTxt}` : "",
    "Índice:",
    ...caps.map((c) => `${c.n}. ${c.titulo} — ${c.linea} (${c.palabras} palabras)`),
  ].filter(Boolean).join("\n");
}

/** Separa la prosa de la línea RESUMEN: y la parte en párrafos limpios. */
export function splitChapter(text: string, chapterTitle: string): { paragraphs: string[]; summary: string } {
  let summary = "";
  const lines = text.replace(/\r/g, "").split("\n");
  for (let i = lines.length - 1; i >= 0; i--) {
    const m = /^\s*\**\s*(RESUMEN|SUMMARY|RÉSUMÉ|ZUSAMMENFASSUNG|RESUMO|РЕЗЮМЕ)\s*:\s*\**\s*(.*)$/i.exec(lines[i]);
    if (m) {
      summary = m[2].trim();
      // Un resumen partido en dos renglones: lo que sigue también es resumen.
      summary = [summary, ...lines.slice(i + 1)].join(" ").replace(/\s+/g, " ").trim();
      lines.length = i;
      break;
    }
  }
  const paragraphs = lines
    .join("\n")
    .split(/\n\s*\n/)
    .map((p) => p.replace(/\s*\n\s*/g, " ").replace(/^[#>*\-\s]+/, "").replace(/\*\*/g, "").trim())
    .filter(Boolean);
  // El modelo a veces repite el título como primer renglón pese a la regla.
  const norm = (s: string) => s.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, "");
  if (paragraphs.length > 1 && norm(paragraphs[0]) === norm(chapterTitle)) paragraphs.shift();
  return { paragraphs, summary: summary.slice(0, 600) };
}

const escribir: Service = async (ctx, args) => {
  const tema = needText(args.tema, 300, "el tema");
  const enfoqueTxt = str(args.enfoque, 200);
  const minutos = minutesOf(args.minutos);
  const titulo = str(args.titulo, 60) || tema.slice(0, 60);
  const given = cleanCaps(args.capitulos, 10).filter((c) => c.activo);
  if (!given.length) throw new Error("falta el índice");
  const caps = calibrate(given, minutos * WORDS_PER_MINUTE);
  const lang = ctx.lang;
  const t = T[lang];
  const label = (n: number, s: string) => t.chapter(n, caps.length, s);

  const jobId = await startJob(ctx.accountId, label(1, caps[0].titulo), async (job): Promise<JobFile[]> => {
    // Lo estable va primero y cacheado: reglas + índice se repiten en cada
    // capítulo byte a byte; los resúmenes cambian y van en el mensaje.
    const system: Anthropic.TextBlockParam[] = [
      { type: "text", text: styleSystem(lang) },
      { type: "text", text: indexBlock(titulo, tema, enfoqueTxt, caps), cache_control: { type: "ephemeral" } },
    ];
    const chapters: { title: string; paragraphs: string[] }[] = [];
    const summaries: string[] = [];
    const t0 = Date.now();

    for (const cap of caps) {
      await job.setProgress(cap.n, caps.length + 1, label(cap.n, cap.titulo));
      const done = summaries.length
        ? `Capítulos ya escritos (resumen de cada uno):\n${summaries.map((s, i) => `${i + 1}. ${caps[i].titulo}: ${s}`).join("\n")}`
        : "Es el primer capítulo: todavía no hay nada escrito.";
      const text = await appsProse({
        accountId: ctx.accountId,
        system,
        user: `${done}\n\nEscribe ahora el capítulo ${cap.n}, "${cap.titulo}" (${cap.linea}), de unas ${cap.palabras} palabras. Termina con la línea RESUMEN:.`,
        maxTokens: Math.max(MIN_PROSE_TOKENS, Math.round(cap.palabras * TOKENS_PER_WORD)) + THINKING_HEADROOM,
      });
      const { paragraphs, summary } = splitChapter(text, cap.titulo);
      if (!paragraphs.length) throw new Error(`el capítulo ${cap.n} salió vacío`);
      chapters.push({ title: cap.titulo, paragraphs });
      summaries.push(summary || paragraphs[0].slice(0, 300));
      console.log(`librito ${job.id}: capítulo ${cap.n}/${caps.length} · ${paragraphs.join(" ").split(/\s+/).length} palabras · ${Math.round((Date.now() - t0) / 1000)} s`);
    }

    // El cierre: obras y autores reales para seguir, sin direcciones.
    await job.setProgress(caps.length + 1, caps.length + 1, t.closingStep);
    const closingText = await appsProse({
      accountId: ctx.accountId,
      system,
      user: [
        `Capítulos escritos (resumen de cada uno):\n${summaries.map((s, i) => `${i + 1}. ${caps[i].titulo}: ${s}`).join("\n")}`,
        `Escribe ahora el cierre "${t.closing}": entre tres y cinco obras o autores REALES y reconocidos para profundizar en el tema, en prosa (un párrafo corto por obra, o un solo párrafo que las hilvane), diciendo qué aporta cada una. Sin direcciones de internet, sin listas ni viñetas, sin inventar títulos. Sin línea RESUMEN.`,
      ].join("\n\n"),
      maxTokens: MIN_PROSE_TOKENS + THINKING_HEADROOM,
    });
    const closing = splitChapter(closingText, t.closing).paragraphs;

    const bytes = buildEpub({
      title: titulo,
      author: t.librito,
      subtitle: t.librito,
      lang,
      chapters,
      closing: closing.length ? { title: t.closing, paragraphs: closing } : undefined,
    });
    const file = await job.saveFile(`${slugify(titulo)}.epub`, bytes);
    console.log(`librito ${job.id}: "${titulo}" listo · ${caps.length} capítulos · ${bytes.byteLength} bytes · ${Math.round((Date.now() - t0) / 1000)} s`);
    return [file];
  });

  return { ok: true, jobId };
};

export const LIBRITO_SERVICES: Record<string, Service> = {
  "librito.enfoque": enfoque,
  "librito.indice": indice,
  "librito.ajustar": ajustar,
  "librito.escribir": escribir,
};
