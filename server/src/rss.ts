// Noticias: feeds RSS/Atom que el usuario carga desde /board (store.feeds) y
// lectura de artículos limpiados en el servidor (sin LLM: se saca el texto de
// los párrafos del HTML). El aparato lo muestra en el visor paginado y cachea
// lo leído en la SD.
//
//   GET /api/rss?lang=xx                  -> { ok, feeds: [{ id, name, items: [{ id, title, when, link }] }] }
//   GET /api/rss/article?feed=id&item=id  -> { ok, title, text }   (o la descripción del feed si la página no da texto)
import { Hono } from "hono";
import { load, save, nextId } from "./store";

const TTL_MS = 30 * 60 * 1000;
const MAX_ITEMS = 15;
const MAX_TEXT = 30_000;

type Item = { id: number; title: string; when: string; link: string; desc: string };
type FeedCache = { at: number; items: Item[] };
const cache = new Map<number, FeedCache>();

function decodeEntities(s: string): string {
  return s
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
    .replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&nbsp;/g, " ").replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"').replace(/&#39;|&apos;/g, "'").replace(/&laquo;/g, "«").replace(/&raquo;/g, "»")
    .replace(/&ndash;/g, "–").replace(/&mdash;/g, "—").replace(/&hellip;/g, "…")
    .replace(/&([a-z]+);/gi, (m, name) => NAMED[name.toLowerCase()] ?? m);
}

const NAMED: Record<string, string> = {
  aacute: "á", eacute: "é", iacute: "í", oacute: "ó", uacute: "ú", ntilde: "ñ", uuml: "ü", agrave: "à", egrave: "è",
  igrave: "ì", ograve: "ò", ugrave: "ù", acirc: "â", ecirc: "ê", icirc: "î", ocirc: "ô", ucirc: "û", ccedil: "ç",
  atilde: "ã", otilde: "õ", auml: "ä", euml: "ë", iuml: "ï", ouml: "ö", szlig: "ß", aring: "å", aelig: "æ", oslash: "ø",
  iexcl: "¡", iquest: "¿", deg: "°", euro: "€", pound: "£", copy: "©", reg: "®", trade: "™", middot: "·", bull: "•",
  lsquo: "‘", rsquo: "’", ldquo: "“", rdquo: "”", times: "×", divide: "÷", frac12: "½", frac14: "¼", frac34: "¾",
  aacute_: "", Aacute: "Á", Eacute: "É", Iacute: "Í", Oacute: "Ó", Uacute: "Ú", Ntilde: "Ñ", Uuml: "Ü",
};

function stripTags(s: string): string {
  return decodeEntities(s.replace(/<[^>]+>/g, " ")).replace(/\s+/g, " ").trim();
}

function tag(block: string, name: string): string {
  const m = new RegExp(`<${name}(?:\\s[^>]*)?>([\\s\\S]*?)<\\/${name}>`, "i").exec(block);
  return m ? m[1].trim() : "";
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

function parseFeed(xml: string): Item[] {
  const items: Item[] = [];
  const blocks = xml.match(/<item(?:\s[^>]*)?>[\s\S]*?<\/item>/gi) ?? xml.match(/<entry(?:\s[^>]*)?>[\s\S]*?<\/entry>/gi) ?? [];
  let i = 0;
  for (const b of blocks) {
    if (items.length >= MAX_ITEMS) break;
    const title = stripTags(tag(b, "title"));
    let link = stripTags(tag(b, "link"));
    if (!link) {
      const href = /<link[^>]*href="([^"]+)"/i.exec(b);
      link = href ? href[1] : "";
    }
    const date = stripTags(tag(b, "pubDate") || tag(b, "published") || tag(b, "updated") || tag(b, "dc:date"));
    const desc = stripTags(tag(b, "description") || tag(b, "summary") || tag(b, "content:encoded") || tag(b, "content"));
    if (!title) continue;
    items.push({ id: ++i, title, when: whenLabel(date), link, desc: desc.slice(0, 4000) });
  }
  return items;
}

async function fetchFeed(id: number, url: string): Promise<Item[]> {
  const c = cache.get(id);
  if (c && Date.now() - c.at < TTL_MS) return c.items;
  try {
    const res = await fetch(url, { headers: { "User-Agent": "ws397-hub/1.0", Accept: "application/rss+xml, application/atom+xml, application/xml, text/xml" } });
    if (!res.ok) throw new Error(`feed ${res.status}`);
    const items = parseFeed(await res.text());
    cache.set(id, { at: Date.now(), items });
    return items;
  } catch (err) {
    console.error("rss:", url.slice(0, 50), err);
    return c?.items ?? [];
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
  const out = [];
  for (const f of feeds) {
    const items = await fetchFeed(f.id, f.url);
    out.push({ id: f.id, name: f.name, items: items.map(({ id, title, when, link }) => ({ id, title, when, link })) });
  }
  return c.json({ ok: true, feeds: out });
});

rss.get("/article", async (c) => {
  const feedId = Number(c.req.query("feed"));
  const itemId = Number(c.req.query("item"));
  const store = await load();
  const feed = (store.feeds ?? []).find((f) => f.id === feedId);
  if (!feed) return c.json({ ok: false, error: "unknown feed" }, 404);
  const items = await fetchFeed(feed.id, feed.url);
  const item = items.find((i) => i.id === itemId);
  if (!item) return c.json({ ok: false, error: "unknown item" }, 404);
  let text = "";
  let title = item.title;
  if (item.link) {
    try {
      const res = await fetch(item.link, { headers: { "User-Agent": "Mozilla/5.0 (compatible; ws397-hub/1.0)", Accept: "text/html" }, redirect: "follow" });
      if (res.ok) {
        const a = extractArticle(await res.text());
        text = a.text;
        if (a.title && a.title.length > 10) title = a.title;
      }
    } catch (err) {
      console.error("article:", item.link.slice(0, 60), err);
    }
  }
  if (text.length < 200) text = item.desc || text || "(sin texto)";
  return c.json({ ok: true, title, text, when: item.when, source: feed.name });
});
