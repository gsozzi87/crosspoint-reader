// Noticias: feeds RSS/Atom/RDF que el usuario carga desde /board (store.feeds) y
// lectura de artículos limpiados en el servidor (sin LLM: se saca el texto de
// los párrafos del HTML). El aparato lo muestra en el visor paginado y cachea
// lo leído en la SD.
//
//   GET /api/rss?lang=xx                  -> { ok, feeds: [{ id, name, error?, items: [{ id, title, when, link }] }] }
//   GET /api/rss/article?feed=id&item=id&lang=xx
//        -> { ok, title, text, when, source, reason, cache }
//        `text` SIEMPRE trae algo legible: la nota, la descripción del feed o,
//        si no se pudo, el motivo escrito en el idioma del aparato.
//        `reason` = "" | paywall | blocked | notfound | timeout | empty | down | stale
//        `cache` = false cuando `text` es una explicación y no la nota (el
//        aparato no debe guardarla en la SD, o el error queda pegado ahí).
//
// OJO con el juego de caracteres: `res.text()` decodifica SIEMPRE como UTF-8 y
// medio diario latinoamericano todavía sirve el RSS en iso-8859-1 (Reforma, por
// ejemplo, manda `<?xml encoding="iso-8859-1"?>` con un Content-Type sin
// charset). Los bytes inválidos se descartaban y el titular llegaba al aparato
// sin tildes: "Exhibe PISA fracaso educativo en Mxico", "Nominan a Quiones",
// "Baln de Oro". Ahora el cuerpo se baja como bytes y lo decodifica
// `textCappedSmart` mirando el Content-Type, la declaración del documento y,
// como desempate, si los bytes son UTF-8 legal (ver net.ts).
//
// OJO con el CDATA: casi todos los diarios escriben <title><![CDATA[Titular]]></title>
// y stripTags() sacaba "<...>" ANTES de desenvolverlo, así que el titular entero
// se iba como si fuera una etiqueta, quedaba sin título y el ítem se descartaba.
// Por eso "agrego el feed y no trae noticias": BBC, Clarín, Le Monde, Substack y
// cualquiera con CDATA daban cero. El orden correcto es CDATA → etiquetas → entidades.
import { Hono } from "hono";
import { load } from "./store";
import { normalizeLang, type Lang } from "./lang";
import { safeFetchAt, textCappedSmart, BROWSER_UA, FEED_ACCEPT } from "./net";

const TTL_MS = 30 * 60 * 1000;
const ERROR_TTL_MS = 5 * 60 * 1000;  // un feed que falla no se reintenta en cada pedido del aparato
const MAX_DOWNLOAD = 4_000_000;      // un feed o una nota no pasan de esto; el resto se corta
const MAX_ITEMS = 15;
const MAX_TEXT = 30_000;
const MAX_DESC = 8_000;              // el <content:encoded> sirve de artículo cuando la página no se puede leer

export type Item = { id: number; title: string; when: string; link: string; desc: string };
type FeedCache = { at: number; items: Item[]; error?: string };
const cache = new Map<number, FeedCache>();

// El CDATA se desenvuelve primero, siempre.
function unCdata(s: string): string {
  return s.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1");
}

function decodeEntities(s: string): string {
  return s
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
    .replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&nbsp;/g, " ").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"').replace(/&#39;|&apos;/g, "'").replace(/&laquo;/g, "«").replace(/&raquo;/g, "»")
    .replace(/&ndash;/g, "–").replace(/&mdash;/g, "—").replace(/&hellip;/g, "…")
    .replace(/&([a-z]+);/gi, (m, name) => NAMED[name.toLowerCase()] ?? m)
    // &amp; al final: si no, "&amp;lt;" se convertía en "<".
    .replace(/&amp;/g, "&");
}

const NAMED: Record<string, string> = {
  aacute: "á", eacute: "é", iacute: "í", oacute: "ó", uacute: "ú", ntilde: "ñ", uuml: "ü", agrave: "à", egrave: "è",
  igrave: "ì", ograve: "ò", ugrave: "ù", acirc: "â", ecirc: "ê", icirc: "î", ocirc: "ô", ucirc: "û", ccedil: "ç",
  atilde: "ã", otilde: "õ", auml: "ä", euml: "ë", iuml: "ï", ouml: "ö", szlig: "ß", aring: "å", aelig: "æ", oslash: "ø",
  iexcl: "¡", iquest: "¿", deg: "°", euro: "€", pound: "£", copy: "©", reg: "®", trade: "™", middot: "·", bull: "•",
  lsquo: "‘", rsquo: "’", ldquo: "“", rdquo: "”", times: "×", divide: "÷", frac12: "½", frac14: "¼", frac34: "¾",
  Aacute: "Á", Eacute: "É", Iacute: "Í", Oacute: "Ó", Uacute: "Ú", Ntilde: "Ñ", Uuml: "Ü",
};

function stripTags(s: string): string {
  // Segunda pasada: hay feeds (Atom con type="html") que mandan el HTML
  // escapado, así que las etiquetas recién aparecen después de decodificar.
  const once = decodeEntities(unCdata(s).replace(/<[^>]+>/g, " "));
  const twice = decodeEntities(once.replace(/<\/?[a-z][a-z0-9]*(?:\s[^>]*)?>/gi, " "));
  return twice.replace(/\s+/g, " ").trim();
}

// Contenido crudo de una etiqueta, aceptando prefijo de espacio de nombres
// (<dc:date>, <content:encoded>, <atom:updated>).
function tagRaw(block: string, name: string): string {
  const open = name.includes(":") ? name : `(?:[a-z0-9]+:)?${name}`;
  const m = new RegExp(`<${open}(?:\\s[^>]*)?>([\\s\\S]*?)<\\/${open}>`, "i").exec(block);
  return m ? m[1].trim() : "";
}

function tag(block: string, name: string): string {
  return stripTags(tagRaw(block, name));
}

// Primer valor no vacío de una lista de etiquetas.
function firstTag(block: string, names: string[]): string {
  for (const n of names) {
    const v = tag(block, n);
    if (v) return v;
  }
  return "";
}

function attr(attrs: string, name: string): string {
  const m = new RegExp(`${name}\\s*=\\s*("([^"]*)"|'([^']*)'|([^\\s>]+))`, "i").exec(attrs);
  return decodeEntities(m ? (m[2] ?? m[3] ?? m[4] ?? "") : "").trim();
}

// El link del ítem: RSS lo pone como texto (<link>url</link>), Atom como
// atributo de una etiqueta vacía (<link rel="alternate" href="..."/>) y algunos
// solo dejan el <guid> o el origLink de FeedBurner.
function itemLink(block: string): string {
  const plain = tag(block, "link");
  if (/^https?:\/\//i.test(plain)) return plain;
  const links: { rel: string; type: string; href: string }[] = [];
  const re = /<(?:[a-z0-9]+:)?link\b([^>]*)>/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(block))) {
    const href = attr(m[1], "href");
    if (href) links.push({ rel: attr(m[1], "rel") || "alternate", type: attr(m[1], "type"), href });
  }
  const good =
    links.find((l) => l.rel === "alternate" && /html/i.test(l.type)) ??
    links.find((l) => l.rel === "alternate") ??
    links.find((l) => l.rel !== "self" && l.rel !== "hub" && l.rel !== "enclosure");
  if (good) return good.href;
  for (const n of ["feedburner:origLink", "guid", "id"]) {
    const v = tag(block, n);
    if (/^https?:\/\//i.test(v)) return v;
  }
  return "";
}

function whenLabel(date: string): string {
  const t = Date.parse(date);
  if (Number.isNaN(t)) return "";
  const d = new Date(t);
  const now = new Date();
  const sameDay = d.toDateString() === now.toDateString();
  const hh = String(d.getHours()).padStart(2, "0"), mm = String(d.getMinutes()).padStart(2, "0");
  return sameDay ? `${hh}:${mm}` : `${String(d.getDate()).padStart(2, "0")}/${String(d.getMonth() + 1).padStart(2, "0")}`;
}

// RSS 2.0 (<item>), Atom (<entry>) y RDF/RSS 1.0 (<item rdf:about=...>), con o
// sin prefijo de espacio de nombres.
const RE_ITEM = /<(?:[a-z0-9]+:)?item\b[^>]*>[\s\S]*?<\/(?:[a-z0-9]+:)?item>/gi;
const RE_ENTRY = /<(?:[a-z0-9]+:)?entry\b[^>]*>[\s\S]*?<\/(?:[a-z0-9]+:)?entry>/gi;

export function parseFeed(xml: string): Item[] {
  const blocks = [...(xml.match(RE_ITEM) ?? []), ...(xml.match(RE_ENTRY) ?? [])];
  const items: Item[] = [];
  let i = 0;
  for (const b of blocks) {
    if (items.length >= MAX_ITEMS) break;
    // El contenido va primero: en Substack y en muchos blogs el artículo entero
    // viene en <content:encoded> y sirve para leerlo sin abrir la página.
    const desc = firstTag(b, ["content:encoded", "content", "description", "summary", "subtitle"]);
    let title = tag(b, "title");
    if (!title) title = desc.slice(0, 90);           // los feeds tipo microblog no traen título
    if (!title) continue;
    const date = firstTag(b, ["pubDate", "published", "updated", "dc:date", "date", "issued"]);
    items.push({ id: ++i, title, when: whenLabel(date), link: itemLink(b), desc: desc.slice(0, MAX_DESC) });
  }
  return items;
}

// ¿Es un feed aunque hoy no tenga noticias (pasa: arXiv los fines de semana)?
export function looksLikeFeed(body: string): boolean {
  return /<(?:[a-z0-9]+:)?(rss|feed|rdf)\b/i.test(body.slice(0, 4000));
}

// Título del canal: el primer <title> antes del primer ítem.
function feedTitle(xml: string): string {
  const head = xml.split(/<(?:[a-z0-9]+:)?(?:item|entry)\b/i)[0];
  return tag(head, "title").slice(0, 60);
}

// <link rel="alternate" type="application/rss+xml" href="..."> de una página
// web: si el usuario pegó la dirección del diario y no la del feed, es acá donde
// está el feed de verdad.
export function discoverFeedUrl(html: string, base: string): string {
  const candidates: { type: string; href: string }[] = [];
  const re = /<link\b([^>]*)>/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(html))) {
    const rel = attr(m[1], "rel").toLowerCase();
    const type = attr(m[1], "type").toLowerCase();
    const href = attr(m[1], "href");
    if (!href || !rel.includes("alternate")) continue;
    if (!/(rss|atom|rdf)\+xml|application\/xml|text\/xml/.test(type)) continue;
    candidates.push({ type, href });
  }
  const best = candidates.find((c) => c.type.includes("rss")) ?? candidates.find((c) => c.type.includes("atom")) ?? candidates[0];
  if (!best) return "";
  try {
    return new URL(best.href, base).toString();
  } catch {
    return "";
  }
}

function headers(ua: string, accept: string): Record<string, string> {
  return { "User-Agent": ua, Accept: accept, "Accept-Language": "es,en;q=0.8,*;q=0.5" };
}

type Downloaded = { body: string; url: string; type: string };

// Motivo concreto de por qué una nota no se pudo traer. Viaja al aparato para
// que muestre "el diario lo tiene detrás de una suscripción" en vez de un
// "No se pudo obtener respuesta" que no dice nada.
export type Reason = "paywall" | "blocked" | "notfound" | "timeout" | "empty" | "down";

export class DownloadError extends Error {
  constructor(message: string, readonly reason: Reason, readonly status = 0) {
    super(message);
  }
}

function httpReason(status: number): Reason {
  if (status === 401 || status === 402 || status === 403 || status === 429 || status === 451) return "blocked";
  if (status === 404 || status === 410) return "notfound";
  return "down";
}

// Baja una URL siguiendo redirecciones y con pinta de navegador. Un 403 o un 406
// casi siempre es el filtro anti-bots o un Accept que no le gustó al servidor:
// se reintenta una vez presentándose como lector de feeds y aceptando cualquier cosa.
//
// `budgetMs` es el tiempo TOTAL: el aparato corta a los 20 s (ServerClient) y
// antes acá se podían encadenar dos intentos de 12 s, así que una nota lenta
// terminaba en un error genérico en pantalla en vez de en una respuesta.
async function download(url: string, accept: string, budgetMs = 24_000): Promise<Downloaded> {
  const deadline = Date.now() + budgetMs;
  const left = () => Math.max(1_000, deadline - Date.now());
  let got: Awaited<ReturnType<typeof safeFetchAt>>;
  try {
    got = await safeFetchAt(url, { headers: headers(BROWSER_UA, accept) }, { timeoutMs: Math.min(left(), budgetMs * 0.6), maxHops: 5 });
  } catch (err) {
    const msg = String(err instanceof Error ? err.message : err);
    throw new DownloadError(msg, /timeout|abort|timed out/i.test(msg) ? "timeout" : "down");
  }
  if (got.res.status === 403 || got.res.status === 406 || got.res.status === 401) {
    got.res.body?.cancel().catch(() => {});
    const first = got.res.status;
    if (Date.now() >= deadline - 1_500) throw new DownloadError(`el sitio respondió ${first}`, httpReason(first), first);
    try {
      got = await safeFetchAt(
        url,
        { headers: headers("ws397-hub/1.0 (lector de feeds)", "*/*") },
        { timeoutMs: left(), maxHops: 5 },
      );
    } catch (err) {
      const msg = String(err instanceof Error ? err.message : err);
      throw new DownloadError(msg, /timeout|abort|timed out/i.test(msg) ? "timeout" : httpReason(first), first);
    }
  }
  if (!got.res.ok) {
    got.res.body?.cancel().catch(() => {});
    throw new DownloadError(`el sitio respondió ${got.res.status}`, httpReason(got.res.status), got.res.status);
  }
  return { body: await textCappedSmart(got.res, MAX_DOWNLOAD), url: got.url, type: got.res.headers.get("content-type") ?? "" };
}

// Baja y parsea un feed sin caché. Devuelve el motivo real cuando no hay
// noticias, en vez de una lista vacía que parece "el diario no publicó nada".
export async function readFeed(url: string): Promise<{ items: Item[]; title: string; url: string }> {
  const got = await download(url, FEED_ACCEPT);
  const items = parseFeed(got.body);
  if (items.length || looksLikeFeed(got.body)) return { items, title: feedTitle(got.body), url: got.url };
  const alt = discoverFeedUrl(got.body, got.url);
  if (!alt) throw new Error("esa dirección no es un feed: es una página web y no declara ninguno");
  const second = await download(alt, FEED_ACCEPT);
  const items2 = parseFeed(second.body);
  if (!items2.length && !looksLikeFeed(second.body)) throw new Error("el feed que declara la página no se pudo leer");
  return { items: items2, title: feedTitle(second.body), url: second.url };
}

// La URL definitiva del feed a partir de lo que pegó el usuario (la página del
// diario sirve: se busca el <link rel="alternate"> adentro).
export async function probeFeed(raw: string): Promise<{ url: string; title: string; count: number }> {
  const r = await readFeed(raw);
  return { url: r.url, title: r.title, count: r.items.length };
}

function errorText(err: unknown): string {
  return String(err instanceof Error ? err.message : err).slice(0, 160);
}

// Devuelve también el error: un feed caído daba lista vacía con ok:true y en el
// aparato parecía que el diario no publicó nada.
async function fetchFeed(id: number, url: string): Promise<{ items: Item[]; error?: string }> {
  const c = cache.get(id);
  if (c && Date.now() - c.at < (c.error ? ERROR_TTL_MS : TTL_MS)) return { items: c.items, error: c.error };
  try {
    const { items } = await readFeed(url);
    cache.set(id, { at: Date.now(), items });
    return { items };
  } catch (err) {
    const error = errorText(err);
    console.error("rss:", url.slice(0, 60), error);
    // Se guardan los últimos titulares buenos: mejor noticias viejas que nada.
    cache.set(id, { at: Date.now(), items: c?.items ?? [], error });
    return { items: c?.items ?? [], error };
  }
}

// Prueba un feed desde /board sin ensuciar la caché del aparato.
export async function checkFeed(url: string): Promise<{ count: number; error?: string }> {
  try {
    const { items } = await readFeed(url);
    return { count: items.length };
  } catch (err) {
    return { count: 0, error: errorText(err) };
  }
}

// Texto legible de una página: <article> si hay, si no el <body>; párrafos
// de más de 40 caracteres, sin scripts, estilos, menús ni pies.
export function extractArticle(html: string): { title: string; text: string } {
  const title = stripTags(tag(html, "title")).replace(/\s*[|\-–—].*$/, "").slice(0, 200);
  let body = html
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<noscript[\s\S]*?<\/noscript>/gi, " ")
    .replace(/<(nav|header|footer|aside|form|figure|iframe)[\s\S]*?<\/\1>/gi, " ")
    .replace(/<!--[\s\S]*?-->/g, " ");
  const article = tag(body, "article");
  if (article && stripTags(article).length > 400) body = article;
  const paragraphs: string[] = [];
  const re = /<(p|h1|h2|h3|li)(?:\s[^>]*)?>([\s\S]*?)<\/\1>/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(body)) && paragraphs.join("\n").length < MAX_TEXT) {
    const t = stripTags(m[2]);
    if (m[1] === "p" && t.length < 40) continue;
    if (t.length < 3) continue;
    paragraphs.push(t);
  }
  let text = paragraphs.join("\n\n");
  if (text.length < 300) text = stripTags(body).slice(0, MAX_TEXT);  // plain pages
  return { title, text: text.slice(0, MAX_TEXT) };
}

export const rss = new Hono();

rss.get("/", async (c) => {
  const store = await load();
  const feeds = store.feeds ?? [];
  // En serie, cinco feeds lentos eran cinco esperas sumadas y el aparato se
  // quedaba mirando "Cargando".
  const results = await Promise.allSettled(feeds.map((f) => fetchFeed(f.id, f.url)));
  const out = feeds.map((f, i) => {
    const r = results[i];
    const got = r.status === "fulfilled" ? r.value : { items: [] as Item[], error: String(r.reason).slice(0, 120) };
    return {
      id: f.id,
      name: f.name,
      error: got.error,
      items: got.items.map(({ id, title, when, link }) => ({ id, title, when, link })),
    };
  });
  return c.json({ ok: true, feeds: out });
});

// Marcas de muro de pago. Si la página bajó bien pero casi no dejó texto y
// aparece alguna de estas, el motivo no es "el servidor falló": el diario
// simplemente no lo da sin suscripción, y eso es lo que hay que decir.
const PAYWALL = new RegExp(
  [
    "paywall", "meterpaywall", "piano-id", "premium-content", "subscriber-only",
    "suscr[ií]b", "solo para suscriptores", "contenido exclusivo", "art[ií]culo para suscriptores",
    "reg[ií]strate para seguir", "inicia sesi[oó]n para (?:seguir|leer)",
    "subscribe (?:now|to (?:continue|read))", "subscription required",
    "assinantes", "abonn[ée]s", "nur f[üu]r abonnenten",
  ].join("|"),
  "i",
);

// Lo que se le muestra al lector cuando la nota no se pudo traer. Sale por
// pantalla tal cual, así que va en el idioma del aparato y en español neutro.
const WHY: Record<Reason, Record<Lang, string>> = {
  paywall: {
    es: "El diario pide una suscripción para leer esta nota. Solo llegó el resumen del feed.",
    en: "The site requires a subscription to read this article. Only the feed summary came through.",
    fr: "Le site demande un abonnement pour lire cet article. Seul le résumé du flux est arrivé.",
    de: "Die Seite verlangt ein Abo für diesen Artikel. Es kam nur die Zusammenfassung des Feeds an.",
    pt: "O site pede assinatura para ler esta notícia. Só chegou o resumo do feed.",
    ru: "Сайт требует подписку для чтения этой статьи. Пришло только краткое описание из ленты.",
  },
  blocked: {
    es: "El sitio bloqueó la descarga (contesta que no a los programas que no son un navegador).",
    en: "The site blocked the download (it refuses anything that is not a browser).",
    fr: "Le site a bloqué le téléchargement (il refuse tout ce qui n'est pas un navigateur).",
    de: "Die Seite hat den Abruf blockiert (sie lehnt alles ab, was kein Browser ist).",
    pt: "O site bloqueou o download (recusa tudo o que não seja um navegador).",
    ru: "Сайт заблокировал загрузку (он отказывает всему, что не является браузером).",
  },
  notfound: {
    es: "La nota ya no está en el sitio del diario.",
    en: "The article is no longer on the site.",
    fr: "L'article n'est plus sur le site.",
    de: "Der Artikel ist nicht mehr auf der Seite.",
    pt: "A notícia já não está no site.",
    ru: "Статьи больше нет на сайте.",
  },
  timeout: {
    es: "El sitio del diario tardó demasiado en contestar. Prueba de nuevo en un rato.",
    en: "The site took too long to answer. Try again in a while.",
    fr: "Le site a mis trop de temps à répondre. Réessaie plus tard.",
    de: "Die Seite hat zu lange gebraucht. Versuche es später noch einmal.",
    pt: "O site demorou demais para responder. Tenta de novo mais tarde.",
    ru: "Сайт слишком долго отвечал. Попробуй позже.",
  },
  empty: {
    es: "La página no tiene texto que se pueda leer (es un video, una galería o todo scripts).",
    en: "The page has no readable text (it is a video, a gallery, or all scripts).",
    fr: "La page n'a pas de texte lisible (vidéo, galerie ou tout en scripts).",
    de: "Die Seite hat keinen lesbaren Text (Video, Galerie oder nur Skripte).",
    pt: "A página não tem texto legível (é um vídeo, uma galeria ou só scripts).",
    ru: "На странице нет читаемого текста (видео, галерея или только скрипты).",
  },
  down: {
    es: "El sitio del diario no respondió bien.",
    en: "The site did not answer properly.",
    fr: "Le site n'a pas répondu correctement.",
    de: "Die Seite hat nicht richtig geantwortet.",
    pt: "O site não respondeu corretamente.",
    ru: "Сайт ответил некорректно.",
  },
};

const STALE: Record<Lang, string> = {
  es: "Los titulares cambiaron desde que abriste la lista. Mantén Atrás para actualizarla y vuelve a entrar.",
  en: "The headlines changed since you opened the list. Hold Back to refresh it and try again.",
  fr: "Les titres ont changé depuis l'ouverture de la liste. Maintiens Retour pour l'actualiser.",
  de: "Die Schlagzeilen haben sich geändert. Halte Zurück gedrückt, um die Liste zu aktualisieren.",
  pt: "As manchetes mudaram desde que abriste a lista. Mantém Voltar para atualizar e entra de novo.",
  ru: "Заголовки изменились с момента открытия списка. Удерживай «Назад», чтобы обновить его.",
};

// Presupuesto total del pedido: el aparato corta a los 20 s (ServerClient), así
// que acá no se puede tardar más. Antes eran 12 s de feed + 12 + 12 de la nota
// y el aparato daba "No se pudo obtener respuesta" sin que nada estuviera roto.
const ARTICLE_BUDGET_MS = 14_000;

rss.get("/article", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const feedId = Number(c.req.query("feed"));
  const itemId = Number(c.req.query("item"));
  const store = await load();
  const feed = (store.feeds ?? []).find((f) => f.id === feedId);
  // Todo lo que sale por acá vuelve con 200 y un texto legible: el aparato solo
  // sabe mostrar "No se pudo obtener respuesta" cuando el status no es 2xx, y un
  // motivo escrito en pantalla vale mucho más que ese cartel. `cache: false`
  // avisa que ese texto NO hay que guardarlo en la SD como si fuera la nota.
  const explain = (title: string, why: string, reason: Reason | "stale") =>
    c.json({ ok: true, title, text: why, when: "", source: feed?.name ?? "", reason, cache: false });
  if (!feed) return explain("", STALE[lang], "stale");
  const { items, error } = await fetchFeed(feed.id, feed.url);
  const item = items.find((i) => i.id === itemId);
  if (!item) return explain(feed.name, error ? `${STALE[lang]}\n\n${error}` : STALE[lang], "stale");

  let text = "";
  let title = item.title;
  let reason: Reason | null = null;
  let detail = "";
  if (item.link) {
    try {
      // download(): redirecciones revalidadas una por una (un link público
      // puede rebotar a la red interna de Railway) y con pinta de navegador.
      const page = await download(item.link, "text/html,application/xhtml+xml,*/*;q=0.8", ARTICLE_BUDGET_MS);
      const a = extractArticle(page.body);
      text = a.text;
      if (a.title && a.title.length > 10) title = a.title;
      if (text.length < 400) reason = PAYWALL.test(page.body) ? "paywall" : "empty";
    } catch (err) {
      reason = err instanceof DownloadError ? err.reason : "down";
      detail = String(err instanceof Error ? err.message : err).slice(0, 120);
      console.error("article:", item.link.slice(0, 60), reason, detail);
    }
  } else {
    reason = "empty";
  }

  // El resumen del feed es contenido de verdad (en Substack y en muchos blogs
  // es la nota entera), así que si lo hay se muestra y se cachea igual.
  if (text.length < 400 && item.desc.length > text.length) text = item.desc;
  if (text.length >= 200) {
    return c.json({ ok: true, title, text, when: item.when, source: feed.name, reason: reason ?? "", cache: true });
  }
  const why = WHY[reason ?? "empty"][lang];
  return explain(title, item.link ? `${why}\n\n${item.link}` : why, reason ?? "empty");
});
