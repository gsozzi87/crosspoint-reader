// La sesión de Telegram de cada cuenta, para la app Libros
// (docs/ws397/LIBROS_CONTRATO.md).
//
// Un bot no puede hablarle a otro bot, así que el servidor le escribe al bot
// de libros COMO LA CUENTA DE TELEGRAM DEL DUEÑO (MTProto, @mtcute/bun). La
// sesión se abre una vez desde /board (api id + api hash de my.telegram.org,
// teléfono, código y la contraseña de dos pasos si la hay) y queda en el
// volumen: `/data/telegram/<cuenta>.session` (`TELEGRAM_DIR` lo cambia). La
// configuración y el estado del login viven en el documento `telegram` de
// fsjson, por cuenta. `apiHash` es secreto y no sale nunca por la API.
//
// Cómo se le habla al bot: se manda el texto y se SONDEA `getHistory` cada
// 700 ms hasta ver un mensaje entrante con id mayor que el enviado, o hasta el
// plazo. Nada de `onNewMessage`: el sondeo no depende del bucle de updates y
// se prueba fácil. Apretar un botón en línea es `getCallbackAnswer` con el
// `data` del botón.
//
// Un solo pedido por cuenta a la vez: dos búsquedas encimadas confundirían
// las respuestas. El segundo no espera, falla con `telegram_busy` (el aparato
// corta a los 25 s y una descarga puede durar dos minutos).
//
// Nunca se loguea el api hash ni el código: sólo el número de cuenta y el
// texto del error de Telegram.
import { mkdir, rm, stat } from "node:fs/promises";
import { TelegramClient, SentCode, tl, type Document, type Message } from "@mtcute/bun";
import { mutateDoc, readDoc } from "./fsjson";

const DIR = process.env.TELEGRAM_DIR ?? "/data/telegram";
const CONNECT_TIMEOUT_MS = 15_000;
const POLL_MS = 700;
const GRACE_MS = 700;           // después de la primera respuesta, una vuelta más por si el bot manda dos mensajes
const CALLBACK_TIMEOUT_MS = 10_000;

export type TelegramErrorCode =
  | "no_telegram"     // sin sesión: hay que conectar Telegram en la web
  | "telegram_busy"   // otro pedido de la misma cuenta en curso
  | "connect"         // no se pudo hablar con Telegram (red)
  | "bot_unknown"     // el @usuario del bot no existe
  | "no_reply"        // el bot no contestó en el plazo
  | "no_file"         // el bot no mandó el archivo en el plazo
  | "too_big"         // el archivo pasa el tope
  | "session_lost"    // Telegram cerró la sesión (401): hay que entrar de nuevo
  | "login"           // un paso del login falló (código, contraseña, api id)
  | "error";

export class TelegramError extends Error {
  constructor(readonly code: TelegramErrorCode, message: string) {
    super(message);
  }
}

// ---------------------------------------------------------------- config (documento `telegram`)

export type TelegramUser = { id: number; name: string; phone: string };

export type TelegramConfig = {
  apiId: number;
  apiHash: string;
  phone: string;
  bot: string;
  loggedIn: boolean;
  phoneCodeHash?: string;
  awaitingPassword?: boolean;
  user?: TelegramUser;
};


function shape(raw: unknown): TelegramConfig {
  const o = raw && typeof raw === "object" ? (raw as Partial<TelegramConfig>) : {};
  const out: TelegramConfig = {
    apiId: Number.isInteger(o.apiId) && (o.apiId as number) > 0 ? (o.apiId as number) : 0,
    apiHash: typeof o.apiHash === "string" ? o.apiHash : "",
    phone: typeof o.phone === "string" ? o.phone : "",
    bot: typeof o.bot === "string" ? o.bot : "",
    loggedIn: o.loggedIn === true,
  };
  if (typeof o.phoneCodeHash === "string" && o.phoneCodeHash) out.phoneCodeHash = o.phoneCodeHash;
  if (o.awaitingPassword === true) out.awaitingPassword = true;
  const u = o.user;
  if (u && typeof u === "object" && Number.isFinite((u as TelegramUser).id)) {
    out.user = { id: Number((u as TelegramUser).id), name: String((u as TelegramUser).name ?? ""), phone: String((u as TelegramUser).phone ?? "") };
  }
  return out;
}

export async function loadConfig(accountId: number): Promise<TelegramConfig> {
  return shape(await readDoc<unknown>(accountId, "telegram", null));
}

function patch(accountId: number, fn: (c: TelegramConfig) => void): Promise<TelegramConfig> {
  return mutateDoc(accountId, "telegram", shape, (c) => {
    fn(c);
    return { ...c };
  });
}

/** "@Bot_Name" / "https://t.me/Bot_Name" → "Bot_Name". */
export function normalizeBot(raw: string): string {
  const s = raw.trim().replace(/^https?:\/\/(t\.me|telegram\.me)\//i, "").replace(/^@/, "").replace(/\/.*$/, "");
  return /^[A-Za-z][A-Za-z0-9_]{3,31}$/.test(s) ? s : "";
}

function normalizePhone(raw: string): string {
  const s = raw.replace(/[\s()-]/g, "");
  return /^\+?\d{6,16}$/.test(s) ? (s.startsWith("+") ? s : `+${s}`) : "";
}

export type TelegramStatus = {
  configured: boolean;
  apiId: number;        // no es secreto (el hash sí): la web lo muestra para saber qué hay guardado
  loggedIn: boolean;
  phone: string;
  bot: string;
  hasHash: boolean;
  user: TelegramUser | null;
  awaitingCode: boolean;
  awaitingPassword: boolean;
};

export async function getStatus(accountId: number): Promise<TelegramStatus> {
  const c = await loadConfig(accountId);
  return {
    configured: c.apiId > 0 && !!c.apiHash && !!c.phone && !!c.bot,
    apiId: c.apiId,
    loggedIn: c.loggedIn,
    phone: c.phone,
    bot: c.bot,
    hasHash: !!c.apiHash,
    user: c.user ?? null,
    awaitingCode: !c.loggedIn && !!c.phoneCodeHash && !c.awaitingPassword,
    awaitingPassword: !c.loggedIn && c.awaitingPassword === true,
  };
}

/** Guarda lo que vino; hash vacío = no tocar. Devuelve qué campo estaba mal, o "". */
export async function saveConfig(accountId: number, b: { apiId?: unknown; apiHash?: unknown; phone?: unknown; bot?: unknown }): Promise<string> {
  const apiId = b.apiId === undefined || b.apiId === "" ? undefined : Number(b.apiId);
  if (apiId !== undefined && !(Number.isInteger(apiId) && apiId > 0)) return "api id";
  const apiHash = typeof b.apiHash === "string" ? b.apiHash.trim() : "";
  if (apiHash && !/^[0-9a-f]{32}$/i.test(apiHash)) return "api hash";
  const phone = typeof b.phone === "string" && b.phone.trim() ? normalizePhone(b.phone) : undefined;
  if (phone === "") return "teléfono";
  const bot = typeof b.bot === "string" && b.bot.trim() ? normalizeBot(b.bot) : undefined;
  if (bot === "") return "bot";
  let credsChanged = false;
  await patch(accountId, (c) => {
    if (apiId !== undefined && apiId !== c.apiId) { c.apiId = apiId; credsChanged = true; }
    if (apiHash && apiHash !== c.apiHash) { c.apiHash = apiHash; credsChanged = true; }
    if (phone !== undefined && phone !== c.phone) {
      c.phone = phone;
      // Otro teléfono es otra sesión: el código que hubiera en curso ya no vale.
      if (!c.loggedIn) { delete c.phoneCodeHash; delete c.awaitingPassword; }
    }
    if (bot !== undefined) c.bot = bot;
  });
  if (credsChanged) await dropClient(accountId);
  return "";
}

// ---------------------------------------------------------------- el cliente, uno por cuenta

type Entry = { client: TelegramClient; apiId: number; apiHash: string; connected: boolean; peer: tl.TypeInputPeer | null; peerName: string };

const clients = new Map<number, Entry>();

function sessionFile(accountId: number): string {
  return `${DIR}/${accountId | 0}.session`;
}

function withTimeout<T>(p: Promise<T>, ms: number, what: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const t = setTimeout(() => reject(new TelegramError("connect", what)), ms);
    p.then((v) => { clearTimeout(t); resolve(v); }, (e) => { clearTimeout(t); reject(e); });
  });
}

async function dropClient(accountId: number): Promise<void> {
  const e = clients.get(accountId);
  if (!e) return;
  clients.delete(accountId);
  try {
    await withTimeout(e.client.destroy(), 5000, "destroy");
  } catch {
    // ya está: lo que importa es que no se use más
  }
}

/** El cliente de la cuenta, conectado. Con `needLogin`, exige sesión abierta (`no_telegram` si no). */
async function getClient(accountId: number, needLogin: boolean): Promise<{ client: TelegramClient; cfg: TelegramConfig; entry: Entry }> {
  const cfg = await loadConfig(accountId);
  if (!(cfg.apiId > 0 && cfg.apiHash)) throw new TelegramError(needLogin ? "no_telegram" : "login", "Faltan el api id y el api hash (my.telegram.org)");
  if (needLogin && !cfg.loggedIn) throw new TelegramError("no_telegram", "Conecta Telegram en la web (Ajustes → Avanzado → Telegram)");
  let entry = clients.get(accountId);
  if (entry && (entry.apiId !== cfg.apiId || entry.apiHash !== cfg.apiHash)) {
    await dropClient(accountId);
    entry = undefined;
  }
  if (!entry) {
    await mkdir(DIR, { recursive: true });
    const client = new TelegramClient({
      apiId: cfg.apiId,
      apiHash: cfg.apiHash,
      storage: sessionFile(accountId),
      // Se sondea el historial: el bucle de updates no hace falta y
      // mantendría una conexión trabajando de más.
      disableUpdates: true,
    });
    entry = { client, apiId: cfg.apiId, apiHash: cfg.apiHash, connected: false, peer: null, peerName: "" };
    clients.set(accountId, entry);
  }
  const { client } = entry;
  // `connect()` es idempotente en mtcute; la bandera sólo evita la llamada y
  // el plazo cuando ya se conectó una vez. Si la red se cae en el medio, el
  // cliente reconecta solo y las llamadas esperan; el plazo de cada llamada
  // es el que las corta.
  if (!entry.connected) {
    try {
      // `connect()` vuelve en el acto (abre el socket por detrás y reintenta
      // solo, para siempre), así que no dice si Telegram está al alcance. La
      // sonda sí: un pedido que no necesita sesión, con plazo. Sin red, error
      // claro a los 15 s y no un pedido colgado.
      await withTimeout(
        client.connect().then(() => client.call({ _: "help.getNearestDc" })),
        CONNECT_TIMEOUT_MS,
        "No se pudo conectar con Telegram (¿hay red?)",
      );
      entry.connected = true;
    } catch (err) {
      // Un cliente que no llegó se destruye: si se dejara, seguiría
      // reintentando cada 5 s y llenando el log hasta el próximo pedido.
      await dropClient(accountId);
      throw err instanceof TelegramError ? err : new TelegramError("connect", `No se pudo conectar con Telegram: ${describe(err)}`);
    }
  }
  return { client, cfg, entry };
}

/** El texto corto de un error de mtcute/Telegram, sin secretos. */
export function describe(err: unknown): string {
  if (tl.RpcError.is(err)) {
    if (err.is("FLOOD_WAIT_%d")) return `Telegram pide esperar ${err.seconds} s`;
    return err.text;
  }
  return (err instanceof Error ? err.message : String(err)).slice(0, 160);
}

// Un 401 es "Telegram cerró esta sesión": desde otro aparato, por inactividad
// o porque el dueño la revocó. Se anota y todo pasa a `no_telegram` hasta que
// vuelva a entrar.
async function handleRpcFailure(accountId: number, err: unknown): Promise<never> {
  if (err instanceof TelegramError && err.code === "connect") {
    // Un plazo vencido en el medio de un pedido es la red caída: el cliente
    // se tira, y el próximo pedido vuelve a sondear en vez de heredar una
    // conexión que reintenta sola en el fondo.
    await dropClient(accountId);
    throw err;
  }
  if (tl.RpcError.is(err) && err.code === 401) {
    console.error(`telegram: cuenta ${accountId}: la sesión ya no vale (${err.text}); hay que entrar de nuevo`);
    await patch(accountId, (c) => { c.loggedIn = false; delete c.phoneCodeHash; delete c.awaitingPassword; delete c.user; });
    await dropClient(accountId);
    throw new TelegramError("session_lost", "Telegram cerró la sesión: entra de nuevo desde la web");
  }
  if (err instanceof TelegramError) throw err;
  throw new TelegramError("error", describe(err));
}

function userOf(u: { id: number; displayName: string; phoneNumber: string | null }): TelegramUser {
  return { id: u.id, name: u.displayName, phone: u.phoneNumber ? `+${u.phoneNumber.replace(/^\+/, "")}` : "" };
}

// ---------------------------------------------------------------- login (desde la web)

/** Manda el código al teléfono. Si Telegram dice que ya estaba dentro, queda `loggedIn`. */
export async function sendCode(accountId: number): Promise<{ loggedIn: boolean }> {
  const { client, cfg } = await getClient(accountId, false);
  if (!cfg.phone) throw new TelegramError("login", "Falta el teléfono");
  try {
    const r = await withTimeout(client.sendCode({ phone: cfg.phone }), 30_000, "Telegram no contestó al pedir el código");
    if (r instanceof SentCode) {
      const hash = r.phoneCodeHash;
      await patch(accountId, (c) => { c.phoneCodeHash = hash; delete c.awaitingPassword; c.loggedIn = false; });
      console.log(`telegram: cuenta ${accountId}: código enviado (${r.type})`);
      return { loggedIn: false };
    }
    const user = userOf(r);
    await patch(accountId, (c) => { c.loggedIn = true; c.user = user; delete c.phoneCodeHash; delete c.awaitingPassword; });
    console.log(`telegram: cuenta ${accountId}: ya estaba dentro como ${user.name}`);
    return { loggedIn: true };
  } catch (err) {
    if (err instanceof TelegramError) throw err;
    throw new TelegramError("login", loginMessage(err));
  }
}

function loginMessage(err: unknown): string {
  if (tl.RpcError.is(err)) {
    switch (err.text) {
      case "PHONE_CODE_INVALID": return "El código no es correcto";
      case "PHONE_CODE_EXPIRED": return "El código venció: pide otro";
      case "PHONE_CODE_EMPTY": return "Falta el código";
      case "PASSWORD_HASH_INVALID": return "La contraseña no es correcta";
      case "PHONE_NUMBER_INVALID": return "El teléfono no es válido (formato internacional, +52…)";
      case "PHONE_NUMBER_BANNED": return "Telegram tiene bloqueado ese teléfono";
      case "API_ID_INVALID": return "El api id o el api hash no son válidos (my.telegram.org)";
      case "PHONE_PASSWORD_FLOOD": return "Demasiados intentos: espera un rato";
      case "SESSION_PASSWORD_NEEDED": return "Hace falta la contraseña de dos pasos";
    }
  }
  return describe(err);
}

/** Entra con el código (y la contraseña de dos pasos si Telegram la pide). */
export async function signIn(accountId: number, b: { code?: unknown; password?: unknown }): Promise<{ loggedIn: boolean; awaitingPassword: boolean }> {
  const code = typeof b.code === "string" ? b.code.replace(/\D/g, "") : "";
  const password = typeof b.password === "string" ? b.password : "";
  const { client, cfg } = await getClient(accountId, false);
  if (cfg.loggedIn) return { loggedIn: true, awaitingPassword: false };

  const done = async (u: { id: number; displayName: string; phoneNumber: string | null }) => {
    const user = userOf(u);
    await patch(accountId, (c) => { c.loggedIn = true; c.user = user; delete c.phoneCodeHash; delete c.awaitingPassword; });
    console.log(`telegram: cuenta ${accountId}: sesión abierta como ${user.name}`);
    return { loggedIn: true, awaitingPassword: false };
  };
  const withPassword = async () => {
    if (!password) return { loggedIn: false, awaitingPassword: true };
    try {
      return await done(await withTimeout(client.checkPassword(password), 30_000, "Telegram no contestó a la contraseña"));
    } catch (err) {
      if (err instanceof TelegramError) throw err;
      throw new TelegramError("login", loginMessage(err));
    }
  };

  if (cfg.awaitingPassword) return withPassword();
  if (!cfg.phoneCodeHash) throw new TelegramError("login", "Primero pide el código");
  if (!code) throw new TelegramError("login", "Falta el código");
  try {
    return await done(await withTimeout(client.signIn({ phone: cfg.phone, phoneCodeHash: cfg.phoneCodeHash, phoneCode: code }), 30_000, "Telegram no contestó al código"));
  } catch (err) {
    if (tl.RpcError.is(err, "SESSION_PASSWORD_NEEDED")) {
      await patch(accountId, (c) => { c.awaitingPassword = true; });
      console.log(`telegram: cuenta ${accountId}: pide la contraseña de dos pasos`);
      return withPassword();
    }
    if (err instanceof TelegramError) throw err;
    throw new TelegramError("login", loginMessage(err));
  }
}

/** Cierra la sesión en Telegram (si se puede) y borra el archivo. Nunca falla. */
export async function logOut(accountId: number): Promise<void> {
  const entry = clients.get(accountId);
  const cfg = await loadConfig(accountId);
  if (cfg.loggedIn) {
    try {
      const { client } = entry ?? (await getClient(accountId, false));
      await withTimeout(client.logOut(), 15_000, "logout");
    } catch (err) {
      console.error(`telegram: cuenta ${accountId}: no se pudo avisar el logout a Telegram (${describe(err)}); se borra igual`);
    }
  }
  await dropClient(accountId);
  const file = sessionFile(accountId);
  for (const suffix of ["", "-wal", "-shm", "-journal"]) await rm(file + suffix, { force: true }).catch(() => {});
  await patch(accountId, (c) => { c.loggedIn = false; delete c.phoneCodeHash; delete c.awaitingPassword; delete c.user; });
  console.log(`telegram: cuenta ${accountId}: sesión cerrada`);
}

/** Hay archivo de sesión (aunque el documento diga otra cosa). Para diagnóstico. */
export async function hasSessionFile(accountId: number): Promise<boolean> {
  return stat(sessionFile(accountId)).then(() => true, () => false);
}

// ---------------------------------------------------------------- hablar con el bot

export type BotButton = { text: string; data: Uint8Array };
export type BotDocument = { fileName: string | null; fileSize: number; mimeType: string; raw: Document };
export type BotMessage = { id: number; text: string; buttons: BotButton[]; document: BotDocument | null };

export type BotSession = {
  /** Manda un texto al bot y devuelve el id del mensaje enviado. */
  send(text: string): Promise<number>;
  /** Espera mensajes ENTRANTES con id mayor que `afterId`; con `wantDocument`, hasta que alguno traiga un archivo. */
  waitReply(afterId: number, opts: { timeoutMs: number; wantDocument?: boolean }): Promise<BotMessage[]>;
  /** Aprieta un botón en línea de un mensaje del bot. */
  press(messageId: number, data: Uint8Array): Promise<void>;
  /** Baja el archivo; `too_big` si pasa el tope. */
  download(doc: BotDocument, maxBytes: number): Promise<Uint8Array>;
};

const busy = new Set<number>();

function toBotMessage(m: Message): BotMessage {
  const buttons: BotButton[] = [];
  const markup = m.markup;
  if (markup && markup.type === "inline") {
    for (const row of markup.buttons) {
      for (const b of row) {
        if (b.type._ === "inlineButtonTypeCallback") buttons.push({ text: b.text, data: b.type.data });
      }
    }
  }
  const media = m.media;
  const document = media && media.type === "document"
    ? { fileName: media.fileName, fileSize: media.fileSize ?? 0, mimeType: media.mimeType, raw: media }
    : null;
  return { id: m.id, text: m.text, buttons, document };
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

/**
 * Toma el candado de la cuenta, abre la sesión y corre `fn` con lo que hace
 * falta para hablarle al bot. `no_telegram` sin sesión, `telegram_busy` si
 * hay otro pedido en curso, `bot_unknown` si el @usuario no existe.
 */
export async function withBot<T>(accountId: number, fn: (s: BotSession) => Promise<T>): Promise<T> {
  if (busy.has(accountId)) throw new TelegramError("telegram_busy", "Telegram está ocupado con otro pedido; espera un momento");
  busy.add(accountId);
  try {
    const { client, cfg, entry } = await getClient(accountId, true);
    if (!cfg.bot) throw new TelegramError("no_telegram", "Falta el nombre del bot (Ajustes → Avanzado → Telegram)");
    if (!entry.peer || entry.peerName !== cfg.bot) {
      try {
        entry.peer = await withTimeout(client.resolvePeer(cfg.bot), 20_000, "Telegram no contestó al buscar el bot");
        entry.peerName = cfg.bot;
      } catch (err) {
        if (tl.RpcError.is(err) && (err.text === "USERNAME_NOT_OCCUPIED" || err.text === "USERNAME_INVALID")) {
          throw new TelegramError("bot_unknown", `No existe @${cfg.bot} en Telegram`);
        }
        return handleRpcFailure(accountId, err);
      }
    }
    const peer = entry.peer;

    const session: BotSession = {
      send: async (text) => {
        try {
          const sent = await withTimeout(client.sendText(peer, text), 20_000, "Telegram no contestó al mandar el mensaje");
          return sent.id;
        } catch (err) {
          return handleRpcFailure(accountId, err);
        }
      },
      waitReply: async (afterId, opts) => {
        const deadline = Date.now() + opts.timeoutMs;
        let graceUntil = 0;
        let found: BotMessage[] = [];
        for (;;) {
          let history: Message[];
          try {
            history = await withTimeout(client.getHistory(peer, { limit: 6 }), 20_000, "Telegram no contestó al leer el chat");
          } catch (err) {
            return handleRpcFailure(accountId, err);
          }
          const incoming = history.filter((m) => !m.isOutgoing && m.id > afterId).map(toBotMessage).sort((a, b) => a.id - b.id);
          const ready = opts.wantDocument ? incoming.some((m) => m.document) : incoming.some((m) => m.text || m.buttons.length);
          if (ready) {
            found = incoming;
            // Una vuelta más: el bot puede mandar la lista en dos mensajes.
            if (!graceUntil) graceUntil = Date.now() + GRACE_MS;
            else if (Date.now() >= graceUntil) return found;
          }
          if (Date.now() >= deadline) {
            if (found.length) return found;
            throw new TelegramError(opts.wantDocument ? "no_file" : "no_reply", opts.wantDocument ? "El bot no mandó el archivo" : "El bot no contestó");
          }
          await sleep(POLL_MS);
        }
      },
      press: async (messageId, data) => {
        try {
          await client.getCallbackAnswer({ chatId: peer, message: messageId, data, timeout: CALLBACK_TIMEOUT_MS });
        } catch (err) {
          if (tl.RpcError.is(err) && err.code === 401) return handleRpcFailure(accountId, err);
          // Muchos bots no contestan la consulta del botón y mandan el archivo
          // igual: el que decide es el historial, no esta respuesta.
          console.log(`telegram: cuenta ${accountId}: el botón no contestó (${describe(err)}); se espera el archivo igual`);
        }
      },
      download: async (doc, maxBytes) => {
        if (doc.fileSize > maxBytes) throw new TelegramError("too_big", `El archivo pesa ${(doc.fileSize / 1048576).toFixed(1)} MB y el tope es ${Math.round(maxBytes / 1048576)} MB`);
        try {
          const bytes = await withTimeout(client.downloadAsBuffer(doc.raw, { fileSize: doc.fileSize || undefined }), 180_000, "La descarga desde Telegram no terminó a tiempo");
          if (bytes.byteLength > maxBytes) throw new TelegramError("too_big", "El archivo pasa el tope");
          return bytes;
        } catch (err) {
          return handleRpcFailure(accountId, err);
        }
      },
    };
    return await fn(session);
  } finally {
    busy.delete(accountId);
  }
}

/** Un texto al bot y lo que contesta (todos los mensajes entrantes que siguieron). */
export function askBot(accountId: number, text: string, opts: { timeoutMs: number; wantDocument?: boolean }): Promise<BotMessage[]> {
  return withBot(accountId, async (s) => {
    const id = await s.send(text);
    return s.waitReply(id, opts);
  });
}
