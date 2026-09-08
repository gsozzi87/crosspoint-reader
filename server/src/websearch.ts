// Búsqueda en internet para los proveedores que NO la traen incorporada.
//
// Anthropic la resuelve solo (herramienta del lado de su servidor, ver llm.ts);
// las APIs compatibles con OpenAI (Groq, DeepSeek, OpenAI) no tienen nada
// parecido, así que la búsqueda la hace este servidor y los resultados le van al
// modelo dentro del prompt (RAG simple: título, dirección y extracto).
//
// Buscadores, de mejor a peor y en ese orden de preferencia:
//   tavily  — con clave (1000 búsquedas gratis por mes), devuelve extractos largos
//   brave   — con clave (2000 por mes gratis)
//   free    — sin ninguna clave: Google Noticias (RSS, titulares del día con
//             fecha y medio) + la respuesta corta de DuckDuckGo. Es lo que hay
//             por defecto y alcanza para "quién ganó", "qué pasó con", "cuándo es".
//
// Regla: nunca hace falta una clave nueva. Sin clave sigue funcionando; si el
// buscador falla, se devuelve el motivo y el modelo contesta como antes.
import { config } from "./config";
import { safeFetch, textCapped, redactSecrets, BROWSER_UA, FEED_ACCEPT } from "./net";
import { parseFeed } from "./rss";
import type { Lang } from "./lang";

// `source`: de dónde salió, para mostrarlo. Google Noticias y DuckDuckGo
// devuelven direcciones propias que redirigen al medio, así que el dominio de la
// URL no sirve como fuente y el nombre real viene aparte.
export type SearchResult = { title: string; url: string; snippet: string; when?: string; source?: string };
export type SearchOutcome = { results: SearchResult[]; provider: string; error?: string };

const MAX_QUERY = 300;
const CACHE_TTL_MS = 10 * 60 * 1000;
const cache = new Map<string, { at: number; out: SearchOutcome }>();

// Región de Google Noticias por idioma del aparato.
const NEWS_REGION: Record<Lang, { hl: string; gl: string }> = {
  es: { hl: "es-419", gl: "AR" },
  en: { hl: "en-US", gl: "US" },
  fr: { hl: "fr", gl: "FR" },
  de: { hl: "de", gl: "DE" },
  pt: { hl: "pt-BR", gl: "BR" },
  ru: { hl: "ru", gl: "RU" },
};

// Palabras que delatan una pregunta de actualidad: son las únicas que disparan
// la búsqueda cuando la decide el servidor (con Anthropic decide el modelo).
// Buscar en todas las preguntas cuesta plata y segundos, y el aparato ya tarda.
const FRESH: Record<Lang, string> = {
  es: "hoy|ahora|actual|actualmente|ultimo|ultima|ultimos|ultimas|reciente|novedad|noticia|esta semana|este mes|este ano|ayer|anoche|manana|quien gano|quien es el presidente|quien ganara|sigue vivo|murio|falle|cuanto sale|cuanto cuesta|cuanto esta|precio|cotizacion|dolar|resultado|partido|jugo|estreno|version|cartelera|clima|pronostico|elecciones|mundial|campeon",
  en: "today|now|current|currently|latest|recent|news|this week|this month|this year|yesterday|tonight|tomorrow|who won|who is the president|still alive|died|price|cost|how much is|exchange rate|score|match|released|release date|version|weather|forecast|election|world cup|champion",
  fr: "aujourd hui|maintenant|actuel|actuelle|dernier|derniere|recent|nouvelle|cette semaine|cette annee|hier|demain|qui a gagne|qui est le president|est mort|prix|combien coute|cours|resultat|match|sortie|version|meteo|election|coupe du monde|champion",
  de: "heute|jetzt|aktuell|neueste|letzte|kurzlich|nachrichten|diese woche|dieses jahr|gestern|morgen|wer hat gewonnen|wer ist der prasident|gestorben|preis|wie viel kostet|kurs|ergebnis|spiel|erschienen|version|wetter|wahl|weltmeisterschaft|meister",
  pt: "hoje|agora|atual|atualmente|ultimo|ultima|recente|noticia|esta semana|este ano|ontem|amanha|quem ganhou|quem e o presidente|morreu|preco|quanto custa|cotacao|resultado|jogo|estreia|versao|clima|previsao|eleicoes|copa do mundo|campeao",
  ru: "сегодня|сейчас|текущий|последний|последние|недавно|новости|на этой неделе|в этом году|вчера|завтра|кто выиграл|кто победил|кто президент|умер|цена|сколько стоит|курс|результат|матч|вышел|версия|погода|выборы|чемпионат|чемпион",
};

function fold(s: string): string {
  return s.normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase();
}

// ¿La pregunta pide datos que el modelo no puede saber de memoria? Además de las
// palabras, cualquier año desde el actual en adelante (el entrenamiento siempre
// quedó atrás).
export function needsFreshInfo(text: string, lang: Lang): boolean {
  const t = fold(text);
  const year = new Date().getFullYear();
  if (new RegExp(`\\b(${year}|${year + 1})\\b`).test(t)) return true;
  return new RegExp(`(^|\\W)(${FRESH[lang]})(\\W|$)`).test(t) || new RegExp(`(^|\\W)(${FRESH.en})(\\W|$)`).test(t);
}

async function tavily(query: string, key: string, limit: number): Promise<SearchResult[]> {
  const res = await safeFetch(
    "https://api.tavily.com/search",
    {
      method: "POST",
      // La clave viaja por header y por cuerpo: la API vieja la pide adentro y
      // la nueva como Bearer.
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${key}` },
      body: JSON.stringify({ api_key: key, query, max_results: limit, search_depth: "basic", include_answer: false }),
    },
    { timeoutMs: 12_000 },
  );
  const body = redactSecrets(await textCapped(res, 400_000), key);
  if (!res.ok) throw new Error(`Tavily ${res.status}: ${body.slice(0, 120)}`);
  const data = JSON.parse(body) as { results?: { title?: string; url?: string; content?: string }[] };
  return (data.results ?? []).slice(0, limit).map((r) => ({
    title: (r.title ?? "").slice(0, 160),
    url: r.url ?? "",
    snippet: (r.content ?? "").slice(0, 700),
  }));
}

async function brave(query: string, key: string, limit: number, lang: Lang): Promise<SearchResult[]> {
  const url = `https://api.search.brave.com/res/v1/web/search?q=${encodeURIComponent(query)}&count=${limit}&search_lang=${lang}`;
  const res = await safeFetch(
    url,
    { headers: { Accept: "application/json", "X-Subscription-Token": key } },
    { timeoutMs: 12_000 },
  );
  const body = redactSecrets(await textCapped(res, 400_000), key);
  if (!res.ok) throw new Error(`Brave ${res.status}: ${body.slice(0, 120)}`);
  const data = JSON.parse(body) as { web?: { results?: { title?: string; url?: string; description?: string; age?: string }[] } };
  return (data.web?.results ?? []).slice(0, limit).map((r) => ({
    title: (r.title ?? "").slice(0, 160),
    url: r.url ?? "",
    snippet: (r.description ?? "").replace(/<[^>]+>/g, "").slice(0, 500),
    when: r.age,
  }));
}

// Google Noticias por RSS: sin clave, sin registro, y trae lo del día con medio
// y fecha. Se parsea con el mismo lector de feeds de Noticias.
async function googleNews(query: string, lang: Lang, limit: number): Promise<SearchResult[]> {
  const { hl, gl } = NEWS_REGION[lang];
  const url = `https://news.google.com/rss/search?q=${encodeURIComponent(query)}&hl=${hl}&gl=${gl}&ceid=${gl}:${hl}`;
  const res = await safeFetch(url, { headers: { "User-Agent": BROWSER_UA, Accept: FEED_ACCEPT } }, { timeoutMs: 12_000 });
  if (!res.ok) throw new Error(`Google Noticias ${res.status}`);
  const items = parseFeed(await textCapped(res, 1_500_000));
  return items.slice(0, limit).map((i) => {
    // Google arma el título como "Titular - Medio".
    const cut = i.title.lastIndexOf(" - ");
    const medio = cut > 20 ? i.title.slice(cut + 3) : "";
    return {
      title: (cut > 20 ? i.title.slice(0, cut) : i.title).slice(0, 160),
      url: i.link,
      snippet: [medio, i.when].filter(Boolean).join(", "),
      when: i.when,
      source: medio,
    };
  });
}

// Respuesta corta de DuckDuckGo (la ficha de Wikipedia y afines). Sin clave.
async function duckDuckGo(query: string): Promise<SearchResult[]> {
  const url = `https://api.duckduckgo.com/?q=${encodeURIComponent(query)}&format=json&no_html=1&skip_disambig=1&t=ws397`;
  const res = await safeFetch(url, { headers: { "User-Agent": BROWSER_UA, Accept: "application/json" } }, { timeoutMs: 10_000 });
  if (!res.ok) throw new Error(`DuckDuckGo ${res.status}`);
  const data = JSON.parse(await textCapped(res, 400_000)) as {
    Heading?: string; Abstract?: string; AbstractURL?: string; AbstractSource?: string;
    Answer?: string; AnswerType?: string;
    RelatedTopics?: { Text?: string; FirstURL?: string }[];
  };
  const out: SearchResult[] = [];
  const source = data.AbstractSource || "DuckDuckGo";
  if (data.Answer) out.push({ title: data.Heading || query, url: data.AbstractURL ?? "", snippet: String(data.Answer).slice(0, 500), source });
  if (data.Abstract) {
    out.push({
      title: data.Heading || data.AbstractSource || query,
      url: data.AbstractURL ?? "",
      snippet: data.Abstract.slice(0, 900),
      source,
    });
  }
  for (const t of data.RelatedTopics ?? []) {
    if (out.length >= 4) break;
    // Los temas relacionados apuntan a duckduckgo.com (son fichas, no notas):
    // el texto sirve de contexto pero no se muestran como fuente.
    if (t.Text && t.FirstURL) out.push({ title: t.Text.slice(0, 80), url: t.FirstURL, snippet: t.Text.slice(0, 300), source: "" });
  }
  return out;
}

// Sin clave: la ficha de DuckDuckGo primero (contesta el dato) y después los
// titulares de Google Noticias (dan la fecha y el medio). Si uno de los dos
// falla, alcanza con el otro.
async function freeSearch(query: string, lang: Lang, limit: number): Promise<SearchOutcome> {
  const [ddg, news] = await Promise.allSettled([duckDuckGo(query), googleNews(query, lang, limit)]);
  const results = [
    ...(ddg.status === "fulfilled" ? ddg.value.slice(0, 2) : []),
    ...(news.status === "fulfilled" ? news.value : []),
  ].slice(0, limit + 2);
  if (results.length) return { results, provider: "gratis (DuckDuckGo + Google Noticias)" };
  const why = [ddg, news]
    .map((r) => (r.status === "rejected" ? String(r.reason instanceof Error ? r.reason.message : r.reason) : ""))
    .filter(Boolean)
    .join("; ");
  return { results: [], provider: "gratis", error: why || "sin resultados" };
}

export async function searchWeb(query: string, lang: Lang, limit = 5): Promise<SearchOutcome> {
  const cfg = (await config()).search;
  const q = query.trim().slice(0, MAX_QUERY);
  if (!cfg.enabled) return { results: [], provider: "apagada", error: "la búsqueda en internet está apagada (web → IA)" };
  if (!q) return { results: [], provider: "apagada", error: "sin consulta" };
  const cacheKey = `${cfg.provider}|${lang}|${q}`;
  const hit = cache.get(cacheKey);
  if (hit && Date.now() - hit.at < CACHE_TTL_MS) return hit.out;
  let out: SearchOutcome;
  try {
    if (cfg.provider === "tavily" && cfg.key) out = { results: await tavily(q, cfg.key, limit), provider: "tavily" };
    else if (cfg.provider === "brave" && cfg.key) out = { results: await brave(q, cfg.key, limit, lang), provider: "brave" };
    else out = await freeSearch(q, lang, limit);
  } catch (err) {
    out = { results: [], provider: cfg.provider, error: redactSecrets(String(err instanceof Error ? err.message : err), cfg.key).slice(0, 160) };
  }
  cache.set(cacheKey, { at: Date.now(), out });
  if (cache.size > 100) cache.delete(cache.keys().next().value as string);
  console.log(`search: "${q.slice(0, 60)}" -> ${out.results.length} resultados por ${out.provider}${out.error ? ` (${out.error})` : ""}`);
  return out;
}

export function hostOf(url: string): string {
  try {
    return new URL(url).hostname.replace(/^www\./, "");
  } catch {
    return "";
  }
}

// Los resultados como texto para meterlos en el prompt.
export function formatResults(query: string, results: SearchResult[]): string {
  const lines = results.map((r, i) => {
    const from = r.source || hostOf(r.url);
    return `${i + 1}. ${r.title}${from ? ` (${from}${r.when ? `, ${r.when}` : ""})` : ""}\n   ${r.snippet}`.trim();
  });
  return `<busqueda_web consulta="${query.replace(/"/g, "'")}" fecha="${new Date().toISOString().slice(0, 10)}">\n${lines.join("\n")}\n</busqueda_web>`;
}

// Una sola línea con los dominios, que es lo único que entra en la pantalla.
export function sourcesLine(results: { url: string; source?: string }[], lang: Lang): string {
  const named = results
    .map((r) => (r.source ?? "") || hostOf(r.url))
    // duckduckgo.com y news.google.com son el intermediario, no la fuente.
    .filter((h) => h && h !== "duckduckgo.com" && h !== "news.google.com");
  const hosts = Array.from(new Set(named)).slice(0, 3);
  if (!hosts.length) return "";
  const label: Record<Lang, string> = { es: "Fuentes", en: "Sources", fr: "Sources", de: "Quellen", pt: "Fontes", ru: "Источники" };
  return `${label[lang]}: ${hosts.join(", ")}`;
}
