// El paquete de noticias: el servidor mastica, el aparato sólo lee.
//
// Hasta ahora Noticias era 100 % bajo demanda: el aparato entraba, pedía los
// titulares, y recién al abrir uno se bajaba y se limpiaba el artículo. Eso
// significa esperar con WiFi arriba por cada nota, y que sin señal no haya nada.
//
// Acá se da vuelta: el servidor recorre los feeds SOLO, cada hora, se mete en
// cada noticia, la deja legible y arma un PAQUETE. El aparato, cuando se
// conecta por cualquier motivo, se baja lo que le falte de un tirón y después
// lee sin red.
//
// Por qué manifiesto + un archivo por nota, y no un JSON grande:
//   - `ServerClient` no tiene streaming y copia el cuerpo DOS veces, así que un
//     JSON de 100 KB son 200 KB de heap interno en un aparato que tiene 230 KB
//     libres.
//   - El manifiesto son ~4 KB y dice qué cambió (por el sha), así que la
//     segunda sincronización del día no baja nada.
import { Hono } from "hono";

import { accountOf, type AppEnv } from "./tenant";
import { DEFAULT_ACCOUNT, sha256Hex } from "./db";
import { mutateDoc, readDoc } from "./fsjson";
import { chatText } from "./llm";
import { normalizeLang, type Lang } from "./lang";
import { download, DownloadError, extractArticle, readFeed } from "./rss";
import { load as loadStore } from "./store";
import { addUsage, overQuota } from "./usage";

// Cuántas notas lleva el paquete y cuántas de ésas pasan por el modelo. El
// resto van con el texto limpiado a mano, que es gratis y casi siempre alcanza.
const PACK_ITEMS = Number(process.env.NEWS_PACK_ITEMS ?? 18);
const DIGEST_PER_RUN = Number(process.env.NEWS_DIGEST_PER_RUN ?? 10);
const REFRESH_MS = Number(process.env.NEWS_REFRESH_MS ?? 60 * 60 * 1000);
const ARTICLE_BUDGET_MS = 12000;
// Un cuerpo de más de esto no entra cómodo en el aparato ni aporta nada: son
// unos 12 minutos de lectura.
const MAX_BODY = 6000;

export type PackItem = {
  id: string;      // "<feedId>-<itemId>", estable entre pasadas
  feed: string;    // el nombre del medio, ya como lo muestra la pantalla
  title: string;
  when: string;
  sha: string;     // del cuerpo: si no cambió, el aparato no lo vuelve a bajar
  bytes: number;
  chewed: boolean; // pasó por el modelo
  link: string;
};

type Body = { id: string; title: string; feed: string; when: string; text: string };
type Pack = { at: string; items: PackItem[]; bodies: Record<string, Body> };

// Conserva las últimas notas buenas de los feeds que no contestaron en esta
// pasada. Una caída temporal de un diario no puede convertir el manifiesto en
// uno vacío y ordenar al aparato que borre toda su caché.
export function carryUnavailable(
  previous: PackItem[],
  bodies: Record<string, Body>,
  unavailable: ReadonlySet<number>,
): PackItem[] {
  return previous.filter((item) => {
    const dash = item.id.indexOf("-");
    if (dash <= 0 || !bodies[item.id]) return false;
    return unavailable.has(Number(item.id.slice(0, dash)));
  });
}

function shape(raw: unknown): Pack {
  const p = (raw && typeof raw === "object" ? raw : {}) as Partial<Pack>;
  return {
    at: typeof p.at === "string" ? p.at : "",
    items: Array.isArray(p.items) ? (p.items as PackItem[]) : [],
    bodies: p.bodies && typeof p.bodies === "object" ? (p.bodies as Record<string, Body>) : {},
  };
}

export async function loadPack(accountId: number): Promise<Pack> {
  return shape(await readDoc<unknown>(accountId, "news", null));
}

const PROMPT: Record<Lang, string> = {
  es: "Reescribe esta noticia para que se lea cómoda en una pantalla chica: qué pasó, dónde, a quién afecta y por qué importa. Español neutro, frases cortas, sin opinión y sin inventar nada que no esté en el texto. Entre cinco y diez frases, en párrafos, texto plano sin títulos ni viñetas.",
  en: "Rewrite this news story so it reads well on a small screen: what happened, where, who it affects and why it matters. Short sentences, no opinion, invent nothing that is not in the text. Five to ten sentences, plain text, no headings or bullets.",
  fr: "Réécris cette actualité pour un petit écran : ce qui s'est passé, où, qui est concerné et pourquoi c'est important. Phrases courtes, sans opinion, sans rien inventer. Cinq à dix phrases, texte brut.",
  de: "Schreibe diese Nachricht für einen kleinen Bildschirm um: was passiert ist, wo, wen es betrifft und warum es wichtig ist. Kurze Sätze, keine Meinung, nichts erfinden. Fünf bis zehn Sätze, reiner Text.",
  pt: "Reescreve esta notícia para um ecrã pequeno: o que aconteceu, onde, quem afeta e porque importa. Frases curtas, sem opinião, sem inventar nada. Cinco a dez frases, texto simples.",
  ru: "Перепиши эту новость для маленького экрана: что произошло, где, кого касается и почему это важно. Короткие фразы, без мнений, ничего не выдумывай. Пять-десять фраз, простой текст.",
};

// Una nota masticada. Si el modelo falla, se devuelve el texto limpiado a mano:
// el paquete NUNCA queda sin la nota por culpa del modelo.
async function chew(accountId: number, text: string, lang: Lang): Promise<{ text: string; chewed: boolean }> {
  const raw = text.slice(0, MAX_BODY);
  if (raw.length < 400) return { text: raw, chewed: false };
  try {
    const out = await chatText({ system: PROMPT[lang], user: raw, maxTokens: 900 });
    await addUsage(accountId, { llm: 1 });
    const clean = out.trim();
    if (clean.length > 200) return { text: clean, chewed: true };
  } catch (err) {
    console.error("news chew:", String(err).slice(0, 120));
  }
  return { text: raw, chewed: false };
}

// Uno de cada feed y después la segunda vuelta, en vez de los primeros N de la
// lista pegada: un diario que publica cada diez minutos si no se come el
// paquete entero y los otros no aparecen nunca. Pura a propósito, para poder
// probarla sin red (./test/news_pack/run.sh).
export function interleave<T>(feeds: { feed: string; id: number; items: T[] }[], max: number):
    { feed: string; feedId: number; item: T }[] {
  const out: { feed: string; feedId: number; item: T }[] = [];
  const masLargo = feeds.reduce((n, f) => Math.max(n, f.items.length), 0);
  for (let round = 0; round < masLargo && out.length < max; round++) {
    for (const f of feeds) {
      const it = f.items[round];
      if (it === undefined) continue;
      out.push({ feed: f.feed, feedId: f.id, item: it });
      if (out.length >= max) break;
    }
  }
  return out;
}

// Arma el paquete de una cuenta. Devuelve cuántas notas quedaron.
export async function rebuild(accountId: number, lang: Lang = "es"): Promise<number> {
  const store = await loadStore(accountId);
  const feeds = store.feeds ?? [];
  if (feeds.length === 0) {
    await mutateDoc(accountId, "news", shape, (pack) => {
      pack.at = new Date().toISOString();
      pack.items = [];
      pack.bodies = {};
    });
    return 0;
  }

  // El masticado gasta modelo SIN que nadie lo pida (corre solo cada hora), así
  // que respeta el mismo tope mensual que las rutas metered: pasado el tope el
  // paquete se arma igual, pero con el texto limpiado a mano, que es gratis.
  const sinCupo = await overQuota(accountId);

  const previo = await loadPack(accountId);
  const conocido = new Map(previo.items.map((i) => [i.id, i]));

  // Los titulares de todos los feeds, intercalados: uno de cada uno y después
  // la segunda vuelta, así un diario que publica mucho no se come el paquete.
  const porFeed: { feed: string; id: number; items: Awaited<ReturnType<typeof readFeed>>["items"] }[] = [];
  const unavailable = new Set<number>();
  for (const f of feeds) {
    try {
      const r = await readFeed(f.url);
      if (r.items.length) porFeed.push({ feed: f.name || r.title, id: f.id, items: r.items });
      else unavailable.add(f.id);
    } catch (err) {
      unavailable.add(f.id);
      console.error("news feed:", f.url.slice(0, 60), String(err).slice(0, 100));
    }
  }
  const orden = interleave(porFeed, PACK_ITEMS);

  const items: PackItem[] = [];
  const bodies: Record<string, Body> = {};
  let chewedCount = 0;
  for (const { feed, feedId, item } of orden) {
    const id = `${feedId}-${item.id}`;
    // Lo que ya estaba y no cambió de título no se vuelve a bajar ni a masticar:
    // eso es lo que hace que la pasada de cada hora sea barata.
    const antes = conocido.get(id);
    if (antes && antes.title === item.title && previo.bodies[id]) {
      items.push(antes);
      bodies[id] = previo.bodies[id];
      continue;
    }

    let text = item.desc ?? "";
    if (item.link) {
      try {
        const page = await download(item.link, "text/html,application/xhtml+xml,*/*;q=0.8", ARTICLE_BUDGET_MS);
        const a = extractArticle(page.body);
        if (a.text.length > text.length) text = a.text;
      } catch (err) {
        const why = err instanceof DownloadError ? err.reason : "down";
        console.error("news article:", item.link.slice(0, 60), why);
      }
    }
    if (text.length < 200) continue;  // sin cuerpo no entra: un titular suelto no es una nota

    const puedeMasticar = !sinCupo && chewedCount < DIGEST_PER_RUN;
    const { text: final, chewed } = puedeMasticar ? await chew(accountId, text, lang) : { text: text.slice(0, MAX_BODY), chewed: false };
    if (chewed) chewedCount++;
    const body: Body = { id, title: item.title, feed, when: item.when, text: final };
    bodies[id] = body;
    items.push({
      id,
      feed,
      title: item.title,
      when: item.when,
      sha: (await sha256Hex(final)).slice(0, 16),
      bytes: final.length,
      chewed,
      link: item.link ?? "",
    });
  }

  // Si ningún feed produjo una nota utilizable, se consideran indisponibles
  // todos los configurados. Esto también cubre páginas que contestaron pero
  // sólo trajeron titulares o cuerpos demasiado cortos.
  if (items.length === 0) {
    for (const feed of feeds) unavailable.add(feed.id);
  }
  const seen = new Set(items.map((item) => item.id));
  for (const old of carryUnavailable(previo.items, previo.bodies, unavailable)) {
    if (seen.has(old.id)) continue;
    items.push(old);
    bodies[old.id] = previo.bodies[old.id];
    seen.add(old.id);
  }
  if (items.length > PACK_ITEMS) items.length = PACK_ITEMS;
  const selectedIds = new Set(items.map((item) => item.id));
  for (const id of Object.keys(bodies)) {
    if (!selectedIds.has(id)) delete bodies[id];
  }

  await mutateDoc(accountId, "news", shape, (pack) => {
    pack.at = new Date().toISOString();
    pack.items = items;
    pack.bodies = bodies;
  });
  console.log(`news: cuenta ${accountId}, ${items.length} notas (${chewedCount} masticadas${sinCupo ? ", sin cupo" : ""})`);
  return items.length;
}

// El temporizador del proceso. Sobrevive a un redeploy porque el paquete está
// en el volumen y la primera pasada al arrancar lo refresca si quedó viejo.
let timer: ReturnType<typeof setInterval> | null = null;
const running = new Map<number, Promise<number>>();

export function refreshPack(accountId: number, lang: Lang = "es"): Promise<number> {
  const active = running.get(accountId);
  if (active) return active;
  const job = rebuild(accountId, lang).finally(() => running.delete(accountId));
  running.set(accountId, job);
  return job;
}

export function startRefresher(): void {
  if (timer) return;
  const pasada = async () => {
    try {
      // Sin multiusuario hay una sola cuenta. Con multiusuario, las cuentas se
      // refrescan cuando su aparato pide el paquete (ver la ruta de abajo): un
      // barrido de todas cada hora gastaría modelo por gente que no lo usa.
      await refreshPack(DEFAULT_ACCOUNT);
    } catch (err) {
      console.error("news refresher:", String(err).slice(0, 200));
    }
  };
  // La primera al minuto de arrancar, para no pelear con el arranque.
  setTimeout(pasada, 60 * 1000);
  timer = setInterval(pasada, REFRESH_MS);
  console.log(`news: refresco cada ${Math.round(REFRESH_MS / 60000)} min`);
}

export const news = new Hono<AppEnv>();

// El manifiesto: todo lo que hay, sin cuerpos. Es lo que el aparato compara
// contra lo que ya tiene en la tarjeta.
news.get("/pack", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const pack = await loadPack(acc);
  const viejo = !pack.at || Date.now() - Date.parse(pack.at) > REFRESH_MS;
  // La preparación de 18 artículos puede tardar más que el timeout HTTP del
  // lector. Se dispara en segundo plano y se devuelve inmediatamente el último
  // paquete bueno; la siguiente sincronización recogerá el nuevo.
  if (viejo || pack.items.length === 0)
    void refreshPack(acc, lang).catch((err) => console.error("news pack:", String(err).slice(0, 200)));
  return c.json({
    ok: true,
    at: pack.at,
    items: pack.items.map(({ id, feed, title, when, sha, bytes, chewed }) => ({ id, feed, title, when, sha, bytes, chewed })),
  });
});

news.get("/status", async (c) => {
  const acc = accountOf(c);
  const pack = await loadPack(acc);
  return c.json({
    ok: true,
    building: running.has(acc),
    at: pack.at,
    items: pack.items.length,
    chewed: pack.items.filter((item) => item.chewed).length,
  });
});

news.post("/refresh", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const pack = await loadPack(acc);
  void refreshPack(acc, lang).catch((err) => console.error("news refresh:", String(err).slice(0, 200)));
  return c.json({ ok: true, building: true, items: pack.items.length }, 202);
});

// Una nota, entera. El aparato baja las que le falten, de a una.
news.get("/item", async (c) => {
  const id = (c.req.query("id") ?? "").toString();
  const pack = await loadPack(accountOf(c));
  const body = pack.bodies[id];
  if (!body) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, ...body });
});
