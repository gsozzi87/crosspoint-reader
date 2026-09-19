// Libros: la app de Lua que le pide libros a un bot de Telegram
// (docs/ws397/LIBROS_CONTRATO.md). El aparato habla por `POST /api/apps/call`
// con `app: "libros"`; el servidor le escribe al bot como la cuenta de Telegram
// del dueño (telegram.ts) y lee lo que contesta con las funciones puras de
// librosParse.ts.
//
// Servicios (`LIBROS_SERVICES`, se enchufan en apps.ts como los de viajes):
//   libros.estado          → {connected, bot}
//   libros.buscar {q, spelled?} → {results:[{title, code}], corrected?, spelled?}
//                            (hasta 10; [] si no encontró). Lo dictado llega con
//                            errores de transcripción ("angeles mastretas"): si
//                            el bot no encuentra nada, el modelo barato corrige
//                            el nombre y se busca otra vez (`corrected` dice con
//                            qué). Con `spelled:true` lo dicho es el nombre
//                            deletreado: se arma la palabra sin modelo
//                            (lettersToWord) y se pasa por el corrector ANTES de
//                            buscar (`spelled` es la palabra armada).
//   libros.ficha  {code}   → {kind:"card", title, author, year, pages, genre, desc, formats}
//                            o {kind:"list", results, msg, nav, page?} cuando lo
//                            elegido era un AUTOR y el bot contesta con SU
//                            catálogo en vez de una ficha (`Ángeles Mastretta
//                            [11]`). Quién decide es classifyReply.
//   libros.mas {msg, dir}  → otra página de esa misma lista: aprieta la flecha
//                            del mensaje `msg` (dir = first|prev|next|last) y
//                            devuelve {kind:"list", results, msg, nav, page?}
//   libros.bajar  {code, format?} → {jobId}  (trabajo: ficha → botón → archivo → saveFile, tope 40 MB)
//
// Todo error vuelve como {ok:false, error, code}: `no_telegram` sin sesión
// (la app dice "conecta Telegram en la web" antes de pedir el micrófono),
// `telegram_busy` con otro pedido en curso, y los demás de telegram.ts. Los
// textos salen en el idioma del pedido (`lang` en la query).
//
// Rutas de la web (`telegramBoard`, montado en board.ts bajo /api/board/telegram):
//   GET  /api/board/telegram            → estado (sin el api hash: sólo `hasHash`)
//   POST /api/board/telegram/config     {apiId?, apiHash?, phone?, bot?}
//   POST /api/board/telegram/code       → manda el código al teléfono
//   POST /api/board/telegram/signin     {code?, password?}
//   POST /api/board/telegram/logout
//   POST /api/board/telegram/test       {q} → lo mismo que libros.buscar
import { Hono, type Context } from "hono";
import type { Service } from "./apps";
import { startJob, type JobFile } from "./appsJobs";
import type { Lang } from "./lang";
import { normalizeLang } from "./lang";
import { readBody } from "./net";
import { accountOf, viaOf, type AppEnv } from "./tenant";
import { chatText } from "./llm";
import { classifyReply, fileNameFor, formatOfLabel, lettersToWord, navOfLabel, pageOfLabel, parseCard, parseList, fold, type Card, type ListResult, type NavDir } from "./librosParse";
import { askBot, getStatus, logOut, saveConfig, sendCode, signIn, TelegramError, withBot, type BotMessage, type BotSession } from "./telegram";

const REPLY_TIMEOUT_MS = 20_000;
const FILE_TIMEOUT_MS = 120_000;
const MAX_FILE_BYTES = 40 * 1024 * 1024;
// Una página del bot trae diez; el tope es de esta capa y va con holgura para
// que una página más larga no se pierda en silencio (que es el defecto que
// tenía la lista del autor: lo que no entraba, no existía).
const MAX_RESULTS = 20;
const CODE_RE = /^\/[A-Za-z0-9_]+$/;
const DIRS: NavDir[] = ["first", "prev", "next", "last"];

// ---------------------------------------------------------------- textos

const T: Record<Lang, {
  noTelegram: string;
  busy: string;
  connect: string;
  botUnknown: string;
  noReply: string;
  noFile: string;
  tooBig: string;
  sessionLost: string;
  noQuery: string;
  badCode: string;
  noMore: string;
  noFormat: (f: string) => string;
  askingCard: string;
  waitingFile: string;
  saving: string;
}> = {
  es: {
    noTelegram: "Conecta Telegram en la web (Ajustes → Avanzado → Telegram)",
    busy: "Telegram está ocupado con otro pedido; espera un momento",
    connect: "No se pudo conectar con Telegram",
    botUnknown: "El bot no existe en Telegram: revisa el nombre en la web",
    noReply: "El bot no contestó",
    noFile: "El bot no mandó el archivo",
    tooBig: "El archivo es demasiado grande (tope 40 MB)",
    sessionLost: "Telegram cerró la sesión: entra de nuevo desde la web",
    noQuery: "Di el título o el autor",
    badCode: "Ese libro no vale: busca de nuevo",
    noMore: "No hay más páginas",
    noFormat: (f) => `Este bot no da ${f.toUpperCase()}`,
    askingCard: "Pidiendo la ficha…",
    waitingFile: "Esperando el archivo…",
    saving: "Guardando…",
  },
  en: {
    noTelegram: "Connect Telegram on the web (Settings → Advanced → Telegram)",
    busy: "Telegram is busy with another request; wait a moment",
    connect: "Could not connect to Telegram",
    botUnknown: "That bot does not exist on Telegram: check the name on the web",
    noReply: "The bot did not answer",
    noFile: "The bot did not send the file",
    tooBig: "The file is too big (40 MB limit)",
    sessionLost: "Telegram closed the session: sign in again from the web",
    noQuery: "Say the title or the author",
    badCode: "That book is not valid: search again",
    noMore: "There are no more pages",
    noFormat: (f) => `This bot does not offer ${f.toUpperCase()}`,
    askingCard: "Asking for the book card…",
    waitingFile: "Waiting for the file…",
    saving: "Saving…",
  },
  fr: {
    noTelegram: "Connecte Telegram sur le web (Réglages → Avancé → Telegram)",
    busy: "Telegram est occupé avec une autre demande ; patiente un instant",
    connect: "Impossible de se connecter à Telegram",
    botUnknown: "Ce bot n'existe pas sur Telegram : vérifie le nom sur le web",
    noReply: "Le bot n'a pas répondu",
    noFile: "Le bot n'a pas envoyé le fichier",
    tooBig: "Le fichier est trop gros (limite 40 Mo)",
    sessionLost: "Telegram a fermé la session : reconnecte-toi depuis le web",
    noQuery: "Dis le titre ou l'auteur",
    badCode: "Ce livre n'est pas valide : cherche à nouveau",
    noMore: "Il n'y a pas d'autres pages",
    noFormat: (f) => `Ce bot ne propose pas de ${f.toUpperCase()}`,
    askingCard: "Demande de la fiche…",
    waitingFile: "En attente du fichier…",
    saving: "Enregistrement…",
  },
  de: {
    noTelegram: "Verbinde Telegram im Web (Einstellungen → Erweitert → Telegram)",
    busy: "Telegram ist mit einer anderen Anfrage beschäftigt; warte kurz",
    connect: "Keine Verbindung zu Telegram möglich",
    botUnknown: "Diesen Bot gibt es auf Telegram nicht: prüfe den Namen im Web",
    noReply: "Der Bot hat nicht geantwortet",
    noFile: "Der Bot hat die Datei nicht geschickt",
    tooBig: "Die Datei ist zu groß (Grenze 40 MB)",
    sessionLost: "Telegram hat die Sitzung beendet: melde dich im Web neu an",
    noQuery: "Sag den Titel oder den Autor",
    badCode: "Dieses Buch ist ungültig: suche erneut",
    noMore: "Es gibt keine weiteren Seiten",
    noFormat: (f) => `Dieser Bot bietet kein ${f.toUpperCase()} an`,
    askingCard: "Buchkarte wird angefragt…",
    waitingFile: "Warte auf die Datei…",
    saving: "Wird gespeichert…",
  },
  pt: {
    noTelegram: "Conecte o Telegram na web (Ajustes → Avançado → Telegram)",
    busy: "O Telegram está ocupado com outro pedido; espere um momento",
    connect: "Não foi possível conectar ao Telegram",
    botUnknown: "Esse bot não existe no Telegram: confira o nome na web",
    noReply: "O bot não respondeu",
    noFile: "O bot não enviou o arquivo",
    tooBig: "O arquivo é grande demais (limite de 40 MB)",
    sessionLost: "O Telegram encerrou a sessão: entre de novo pela web",
    noQuery: "Diga o título ou o autor",
    badCode: "Esse livro não vale: busque de novo",
    noMore: "Não há mais páginas",
    noFormat: (f) => `Este bot não oferece ${f.toUpperCase()}`,
    askingCard: "Pedindo a ficha…",
    waitingFile: "Esperando o arquivo…",
    saving: "Salvando…",
  },
  ru: {
    noTelegram: "Подключите Telegram на сайте (Настройки → Дополнительно → Telegram)",
    busy: "Telegram занят другим запросом; подождите немного",
    connect: "Не удалось подключиться к Telegram",
    botUnknown: "Такого бота нет в Telegram: проверьте имя на сайте",
    noReply: "Бот не ответил",
    noFile: "Бот не прислал файл",
    tooBig: "Файл слишком большой (предел 40 МБ)",
    sessionLost: "Telegram закрыл сессию: войдите заново на сайте",
    noQuery: "Скажите название или автора",
    badCode: "Эта книга недействительна: поищите снова",
    noMore: "Больше страниц нет",
    noFormat: (f) => `Этот бот не даёт ${f.toUpperCase()}`,
    askingCard: "Запрашиваю карточку…",
    waitingFile: "Жду файл…",
    saving: "Сохраняю…",
  },
};

function localized(err: unknown, lang: Lang): { error: string; code: string } {
  const t = T[lang];
  if (err instanceof TelegramError) {
    switch (err.code) {
      case "no_telegram": return { error: t.noTelegram, code: err.code };
      case "telegram_busy": return { error: t.busy, code: err.code };
      case "connect": return { error: t.connect, code: err.code };
      case "bot_unknown": return { error: t.botUnknown, code: err.code };
      case "no_reply": return { error: t.noReply, code: err.code };
      case "no_file": return { error: t.noFile, code: err.code };
      case "too_big": return { error: t.tooBig, code: err.code };
      case "session_lost": return { error: t.sessionLost, code: err.code };
      default: return { error: err.message.slice(0, 200), code: err.code };
    }
  }
  return { error: (err instanceof Error ? err.message : String(err)).slice(0, 200) || "falló", code: "error" };
}

// Cada servicio devuelve {ok:false, error, code} en vez de tirar: apps.ts pone
// `code: "error"` a toda excepción que no sea del modelo, y acá el código
// importa (la app distingue `no_telegram`).
function guarded(fn: Service): Service {
  return async (ctx, args) => {
    try {
      return await fn(ctx, args);
    } catch (err) {
      const { error, code } = localized(err, ctx.lang);
      console.error(`libros: cuenta ${ctx.accountId}: ${code}: ${err instanceof Error ? err.message : String(err)}`);
      return { ok: false, error, code };
    }
  };
}

function argStr(v: unknown, max: number): string {
  return typeof v === "string" ? v.trim().slice(0, max) : "";
}

// ---------------------------------------------------------------- lo que dice el bot

// El mensaje que ES la ficha: el que trae botones; si ninguno, el más largo.
function pickCard(msgs: BotMessage[]): BotMessage | null {
  const withButtons = msgs.filter((m) => m.buttons.length);
  if (withButtons.length) return withButtons[withButtons.length - 1];
  let best: BotMessage | null = null;
  for (const m of msgs) if (!best || m.text.length > best.text.length) best = m;
  return best;
}

type Results = ListResult[];

// Una lista de resultados tal como viaja al aparato: lo que se lee, más de
// qué mensaje salió y hacia dónde se puede pasar de página. `msg` es el
// mensaje que TIENE las flechas: es el que `libros.mas` vuelve a apretar.
type ListOut = { results: Results; msg: number; nav: NavDir[]; page?: { at: number; of: number } };

function listOf(msgs: BotMessage[]): ListOut {
  const text = msgs.map((m) => m.text).join("\n");
  const results = parseList(text).slice(0, MAX_RESULTS);
  // El que manda la navegación es el último mensaje con flechas: si el bot
  // partió la lista en dos, los botones van en el de abajo.
  let navMsg: BotMessage | null = null;
  for (const m of msgs) if (m.buttons.some((b) => navOfLabel(b.text))) navMsg = m;
  const nav: NavDir[] = [];
  let page: { at: number; of: number } | null = null;
  if (navMsg) {
    for (const b of navMsg.buttons) {
      const d = navOfLabel(b.text);
      if (d && !nav.includes(d)) nav.push(d);
      if (!page) page = pageOfLabel(b.text);
    }
  }
  // El número de página puede estar en el texto ("Página 2 de 5") y no en un botón.
  if (!page) for (const line of text.split("\n")) if ((page = pageOfLabel(line))) break;
  const out: ListOut = { results, msg: navMsg ? navMsg.id : 0, nav };
  if (page) out.page = page;
  return out;
}

async function searchWith(s: BotSession, q: string): Promise<ListOut> {
  const msgs = await s.waitReply(await s.send(q), { timeoutMs: REPLY_TIMEOUT_MS });
  return listOf(msgs);
}

export async function search(accountId: number, q: string): Promise<Results> {
  return withBot(accountId, async (s) => (await searchWith(s, q)).results);
}

// ---------------------------------------------------------------- el corrector

// El aparato no tiene teclado: el nombre entra por voz y el transcriptor no
// conoce a los autores ("ángeles mastretta" → "angeles mastretas"). El modelo
// barato de siempre (chatText, el mismo proveedor que Hablar) sí los conoce.
// Se le pide SOLO la consulta corregida; cualquier otra cosa (sin clave, fallo
// del proveedor, una respuesta que no parece una consulta) devuelve null y la
// búsqueda sigue como si el corrector no existiera: nunca falla por él.
const CORRECT_PROMPT = [
  "The user dictated a book title and/or author name to search a library.",
  "Fix obvious transcription errors using your knowledge of real authors and books",
  "(e.g. 'angeles mastretas' → 'Ángeles Mastretta').",
  "Return ONLY the corrected query, nothing else: no quotes, no explanation.",
  "If it already looks right, return it unchanged.",
].join(" ");

function sameQuery(a: string, b: string): boolean {
  const norm = (s: string) => fold(s).replace(/[^\p{L}\p{N}]+/gu, " ").trim();
  return norm(a) === norm(b);
}

/** La consulta corregida por el modelo, o null si no cambia o no se pudo. */
export async function correctQuery(q: string, lang: Lang): Promise<string | null> {
  try {
    const raw = await chatText({ system: CORRECT_PROMPT, user: q, maxTokens: 80, search: "off", lang });
    const out = raw.trim().split("\n")[0].trim().replace(/^["'«»“”‘’]+|["'«»“”‘’.]+$/g, "").trim();
    if (!out || out.length > 200 || out.length > q.length * 3 + 20) return null;
    if (sameQuery(out, q)) return null;
    return out;
  } catch (err) {
    console.log(`libros: corrector sin usar: ${err instanceof Error ? err.message : String(err)}`);
    return null;
  }
}

// ---------------------------------------------------------------- servicios

const estado: Service = async (ctx) => {
  const st = await getStatus(ctx.accountId);
  return { connected: st.loggedIn && st.configured, bot: st.bot };
};

// Todo adentro de UN withBot: el candado de la cuenta cubre las dos búsquedas
// (y la llamada al modelo entre medio), así otro pedido no se cuela entre la
// consulta cruda y la corregida.
const buscar: Service = async (ctx, args) => {
  const raw = argStr(args.q, 200);
  if (!raw) return { ok: false, error: T[ctx.lang].noQuery, code: "no_query" };
  const isSpelled = args.spelled === true || args.spelled === "true" || args.spelled === 1;
  const who = `libros: cuenta ${ctx.accountId}`;
  const out: Record<string, unknown> = {};
  let q = raw;
  let corrected: string | null = null;
  if (isSpelled) {
    const word = lettersToWord(raw, ctx.lang);
    if (!word) return { ok: false, error: T[ctx.lang].noQuery, code: "no_query" };
    q = word;
    out.spelled = word;
    // Un nombre deletreado sigue siendo ruidoso (letras que faltan, minúsculas):
    // el corrector va ANTES de la primera búsqueda.
    corrected = await correctQuery(word, ctx.lang);
    if (corrected) q = corrected;
    console.log(`${who}: deletreado "${raw.slice(0, 80)}" → "${word}"${corrected ? ` → "${corrected}"` : ""}`);
  }
  const list = await withBot(ctx.accountId, async (s) => {
    let r = await searchWith(s, q);
    console.log(`${who}: "${q.slice(0, 60)}" → ${r.results.length} resultados${r.nav.length ? ` (paginada: ${r.nav.join(",")})` : ""}`);
    if (!r.results.length && !corrected) {
      corrected = await correctQuery(q, ctx.lang);
      if (corrected) {
        r = await searchWith(s, corrected);
        console.log(`${who}: corregido a "${corrected.slice(0, 60)}" → ${r.results.length} resultados`);
      }
    }
    return r;
  });
  if (corrected) out.corrected = corrected;
  return { ...out, kind: "list", ...list };
};

// El texto y los botones que ES la respuesta: el mensaje con botones si lo
// hay (la ficha, o la lista paginada), y si no el más largo.
function replyOf(msgs: BotMessage[]): { msg: BotMessage; text: string; labels: string[] } | null {
  const msg = pickCard(msgs);
  if (!msg) return null;
  // El texto se junta entero: un bot puede partir la lista en dos mensajes.
  return { msg, text: msgs.map((m) => m.text).join("\n"), labels: msg.buttons.map((b) => b.text) };
}

// Lo que el bot contesta a un comando NO siempre es una ficha: si lo elegido
// era un autor (`Ángeles Mastretta [11]`), contesta con SU CATÁLOGO, otra
// lista y encima paginada. Leerla como ficha metía los diez títulos adentro
// de la descripción y perdía las demás páginas. Quién decide es
// `classifyReply`, y el `kind` que vuelve elige la pantalla en el aparato.
const ficha: Service = async (ctx, args) => {
  const code = argStr(args.code, 40);
  if (!CODE_RE.test(code)) return { ok: false, error: T[ctx.lang].badCode, code: "bad_code" };
  const msgs = await askBot(ctx.accountId, code, { timeoutMs: REPLY_TIMEOUT_MS });
  const reply = replyOf(msgs);
  if (!reply) throw new TelegramError("no_reply", "El bot no contestó");
  if (classifyReply(reply.text, reply.labels) === "list") {
    const list = listOf(msgs);
    console.log(`libros: cuenta ${ctx.accountId}: ${code} contestó una LISTA de ${list.results.length}${list.nav.length ? ` (${list.nav.join(",")})` : ""}`);
    return { kind: "list", ...list };
  }
  const parsed: Card = parseCard(reply.msg.text, reply.labels);
  return { kind: "card", ...parsed };
};

// Otra página de una lista que sigue en el chat. El bot normalmente EDITA el
// mismo mensaje al apretar la flecha (por eso hace falta `waitChange` y no
// `waitReply`, que sólo mira mensajes nuevos); si manda uno nuevo, también se
// toma. Los botones salen de releer el mensaje: el `data` de un botón es
// binario y no viaja al aparato.
const mas: Service = async (ctx, args) => {
  const t = T[ctx.lang];
  const msgId = Math.trunc(Number(args.msg));
  const raw = fold(argStr(args.dir, 10)) as NavDir;
  const dir: NavDir = DIRS.includes(raw) ? raw : "next";
  if (!Number.isFinite(msgId) || msgId <= 0) return { ok: false, error: t.badCode, code: "bad_code" };
  return withBot(ctx.accountId, async (s) => {
    const before = await s.read(msgId);
    if (!before) return { ok: false, error: t.noMore, code: "no_more" };
    const button = before.buttons.find((b) => navOfLabel(b.text) === dir);
    if (!button) return { ok: false, error: t.noMore, code: "no_more" };
    await s.press(msgId, button.data);
    const after = await s.waitChange(msgId, before, { timeoutMs: REPLY_TIMEOUT_MS });
    const list = listOf([after]);
    console.log(`libros: cuenta ${ctx.accountId}: ${dir} sobre ${msgId} → ${list.results.length} resultados en ${after.id}`);
    return { kind: "list", ...list };
  });
};

const bajar: Service = async (ctx, args) => {
  const code = argStr(args.code, 40);
  if (!CODE_RE.test(code)) return { ok: false, error: T[ctx.lang].badCode, code: "bad_code" };
  const format = (fold(argStr(args.format, 10)) || "epub").replace(/[^a-z0-9]/g, "") || "epub";
  // Sin sesión se contesta acá, en el acto: un trabajo que muere en el primer
  // paso deja al aparato sondeando `job.status` para leer lo mismo.
  const st = await getStatus(ctx.accountId);
  if (!(st.loggedIn && st.configured)) throw new TelegramError("no_telegram", "sin sesión");
  const t = T[ctx.lang];
  const accountId = ctx.accountId;
  const jobId = await startJob(accountId, t.askingCard, async (job): Promise<JobFile[]> => {
    try {
      return await withBot(accountId, async (s) => {
        await job.setProgress(1, 3, t.askingCard);
        const sentId = await s.send(code);
        const card = pickCard(await s.waitReply(sentId, { timeoutMs: REPLY_TIMEOUT_MS }));
        if (!card) throw new TelegramError("no_reply", "El bot no contestó");
        const parsed = parseCard(card.text, card.buttons.map((b) => b.text));
        const button = card.buttons.find((b) => formatOfLabel(b.text) === format);
        if (!button) throw new Error(t.noFormat(format));
        await job.setProgress(2, 3, t.waitingFile);
        await s.press(card.id, button.data);
        const replies = await s.waitReply(card.id, { timeoutMs: FILE_TIMEOUT_MS, wantDocument: true });
        const doc = replies.find((m) => m.document)?.document;
        if (!doc) throw new TelegramError("no_file", "El bot no mandó el archivo");
        const bytes = await s.download(doc, MAX_FILE_BYTES);
        await job.setProgress(3, 3, t.saving);
        const name = fileNameFor(doc.fileName, parsed.title || code.slice(1), format);
        const file = await job.saveFile(name, bytes, { maxBytes: MAX_FILE_BYTES });
        console.log(`libros: cuenta ${accountId}: bajado ${name} (${bytes.byteLength} bytes)`);
        return [file];
      });
    } catch (err) {
      // El trabajo guarda el mensaje tal cual: que salga en el idioma del pedido.
      throw new Error(localized(err, ctx.lang).error);
    }
  });
  return { jobId };
};

export const LIBROS_SERVICES: Record<string, Service> = {
  "libros.estado": guarded(estado),
  "libros.buscar": guarded(buscar),
  "libros.ficha": guarded(ficha),
  "libros.mas": guarded(mas),
  "libros.bajar": guarded(bajar),
};

// ---------------------------------------------------------------- la web: /api/board/telegram*

export const telegramBoard = new Hono<AppEnv>();

// Con la sesión de /board (o el token de siempre en el servidor de un solo
// usuario), nunca con el Bearer de un aparato vinculado: la sesión de
// Telegram es del dueño, y el aparato no la configura.
telegramBoard.use("*", async (c, next) => {
  if (viaOf(c) === "device") return c.json({ ok: false, error: "solo desde la web", code: "forbidden" }, 403);
  await next();
});

function fail(c: Context<AppEnv>, err: unknown) {
  const msg = err instanceof Error ? err.message : String(err);
  const code = err instanceof TelegramError ? err.code : "error";
  console.error(`telegram web: cuenta ${accountOf(c)}: ${code}: ${msg}`);
  return c.json({ ok: false, error: msg.slice(0, 200), code }, 400);
}

telegramBoard.get("/", async (c) => c.json({ ok: true, ...(await getStatus(accountOf(c))) }));

telegramBoard.post("/config", async (c) => {
  const b = await readBody(c);
  const bad = await saveConfig(accountOf(c), b);
  if (bad) return c.json({ ok: false, error: `Revisa el campo: ${bad}` }, 400);
  return c.json({ ok: true, ...(await getStatus(accountOf(c))) });
});

telegramBoard.post("/code", async (c) => {
  try {
    const r = await sendCode(accountOf(c));
    return c.json({ ok: true, ...r, ...(await getStatus(accountOf(c))) });
  } catch (err) {
    return fail(c, err);
  }
});

telegramBoard.post("/signin", async (c) => {
  const b = await readBody(c);
  try {
    const r = await signIn(accountOf(c), b);
    return c.json({ ok: true, ...r });
  } catch (err) {
    return fail(c, err);
  }
});

telegramBoard.post("/logout", async (c) => {
  await logOut(accountOf(c));
  return c.json({ ok: true, ...(await getStatus(accountOf(c))) });
});

telegramBoard.post("/test", async (c) => {
  const b = await readBody(c);
  const q = argStr(b.q, 200);
  const lang = normalizeLang(c.req.query("lang"));
  if (!q) return c.json({ ok: false, error: T[lang].noQuery, code: "no_query" }, 400);
  try {
    return c.json({ ok: true, results: await search(accountOf(c), q) });
  } catch (err) {
    const { error, code } = localized(err, lang);
    console.error(`telegram web: cuenta ${accountOf(c)}: prueba: ${code}: ${err instanceof Error ? err.message : String(err)}`);
    return c.json({ ok: false, error, code }, 400);
  }
});
