// Noticias: feeds RSS/Atom/RDF que el usuario carga desde /board (store.feeds) y
// lectura de artículos limpiados en el servidor (sin LLM: se saca el texto de
// los párrafos del HTML). El aparato lo muestra en el visor paginado y cachea
// lo leído en la SD.
//
//   GET /api/rss?lang=xx                  -> { ok, feeds: [{ id, name, error?, items: [{ id, title, when, link }] }] }
//   GET /api/rss/article?feed=id&item=id  -> { ok, title, text }   (o la descripción del feed si la página no da texto)
//
// OJO con el CDATA: casi todos los diarios escriben <title><![CDATA[Titular]]></title>
// y stripTags() sacaba "<...>" ANTES de desenvolverlo, así que el titular entero
// se iba como si fuera una etiqueta, quedaba sin título y el ítem se descartaba.
// Por eso "agrego el feed y no trae noticias": BBC, Clarín, Le Monde, Substack y
// cualquiera con CDATA daban cero. El orden correcto es CDATA → etiquetas → entidades.
import { Hono } from "hono";
import { load } from "./store";
import { safeFetchAt, textCapped, BROWSER_UA, FEED_ACCEPT } from "./net";

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

// Baja una URL siguiendo redirecciones y con pinta de navegador. Un 403 o un 406
// casi siempre es el filtro anti-bots o un Accept que no le gustó al servidor:
// se reintenta una vez presentándose como lector de feeds y aceptando cualquier cosa.
async function download(url: string, accept: string): Promise<Downloaded> {
  let got = await safeFetchAt(url, { headers: headers(BROWSER_UA, accept) }, { timeoutMs: 12_000, maxHops: 5 });
  if (got.res.status === 403 || got.res.status === 406 || got.res.status === 401) {
    got.res.body?.cancel().catch(() => {});
    got = await safeFetchAt(
      url,
      { headers: headers("ws397-hub/1.0 (lector de feeds)", "*/*") },
      { timeoutMs: 12_000, maxHops: 5 },
    );
  }
  if (!got.res.ok) {
    got.res.body?.cancel().catch(() => {});
    throw new Error(`el sitio respondió ${got.res.status}`);
  }
  return { body: await textCapped(got.res, MAX_DOWNLOAD), url: got.url, type: got.res.headers.get("content-type") ?? "" };
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

rss.get("/article", async (c) => {
  const feedId = Number(c.req.query("feed"));
  const itemId = Number(c.req.query("item"));
  const store = await load();
  const feed = (store.feeds ?? []).find((f) => f.id === feedId);
  if (!feed) return c.json({ ok: false, error: "unknown feed" }, 404);
  const { items } = await fetchFeed(feed.id, feed.url);
  const item = items.find((i) => i.id === itemId);
  if (!item) return c.json({ ok: false, error: "unknown item" }, 404);
  let text = "";
  let title = item.title;
  if (item.link) {
    try {
      // download(): redirecciones revalidadas una por una (un link público
      // puede rebotar a la red interna de Railway) y con pinta de navegador.
      const page = await download(item.link, "text/html,application/xhtml+xml,*/*;q=0.8");
      const a = extractArticle(page.body);
      text = a.text;
      if (a.title && a.title.length > 10) title = a.title;
    } catch (err) {
      console.error("article:", item.link.slice(0, 60), err);
    }
  }
  if (text.length < 200) text = item.desc || text || "(sin texto)";
  return c.json({ ok: true, title, text, when: item.when, source: feed.name });
});
