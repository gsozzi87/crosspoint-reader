// Fuente médica curada desde PubMed. No es un RSS externo: el servidor consulta
// E-utilities y devuelve ítems con la misma forma que rss.ts, para que Noticias
// los mezcle en el paquete normal y el aparato no tenga que conocer PubMed.
//
// La búsqueda prioriza evidencia clínica útil y reciente (RCT, revisiones,
// metaanálisis, guías y revistas grandes) y excluye editoriales/cartas/casos.
// Se usa Entrez date: así entra lo NUEVO que PubMed incorporó, aunque la fecha
// formal de publicación sea anterior.
//
// NCBI pide identificar la aplicación; NCBI_EMAIL y NCBI_API_KEY son opcionales.
import type { Item } from "./rss";
import { safeFetchAt, textCappedSmart } from "./net";

export const MEDICAL_FEED_ID = 2_000_000_000;
export const MEDICAL_FEED_NAME = "Medicina · PubMed";

const EUTILS = "https://eutils.ncbi.nlm.nih.gov/entrez/eutils";
const MAX_XML = 2_000_000;
const MAX_JSON = 500_000;
const DEFAULT_ITEMS = 8;
const MAX_ITEMS = 12;
const RECENT_DAYS = 14;

const QUERY = [
  "(",
  '"Randomized Controlled Trial"[Publication Type]',
  'OR "Meta-Analysis"[Publication Type]',
  'OR "Systematic Review"[Publication Type]',
  'OR "Practice Guideline"[Publication Type]',
  'OR "Guideline"[Publication Type]',
  'OR "N Engl J Med"[Journal]',
  'OR "Lancet"[Journal]',
  'OR "JAMA"[Journal]',
  'OR "BMJ"[Journal]',
  'OR "Nat Med"[Journal]',
  ")",
  "NOT (",
  '"Editorial"[Publication Type]',
  'OR "Letter"[Publication Type]',
  'OR "Comment"[Publication Type]',
  'OR "Case Reports"[Publication Type]',
  ")",
].join(" ");

function clampItems(): number {
  const n = Number(process.env.NEWS_MEDICAL_ITEMS ?? DEFAULT_ITEMS);
  return Number.isFinite(n) ? Math.max(1, Math.min(MAX_ITEMS, Math.trunc(n))) : DEFAULT_ITEMS;
}

function addIdentity(url: URL): void {
  url.searchParams.set("tool", "ws397-paper");
  const email = process.env.NCBI_EMAIL?.trim();
  const key = process.env.NCBI_API_KEY?.trim();
  if (email) url.searchParams.set("email", email);
  if (key) url.searchParams.set("api_key", key);
}

async function getText(url: URL, max: number): Promise<string> {
  addIdentity(url);
  const { res } = await safeFetchAt(
    url.toString(),
    { headers: { Accept: "application/json, application/xml;q=0.9, text/xml;q=0.8" } },
    { timeoutMs: 15_000, maxHops: 3 },
  );
  if (!res.ok) {
    res.body?.cancel().catch(() => {});
    throw new Error(`PubMed respondió ${res.status}`);
  }
  return textCappedSmart(res, max);
}

function entity(s: string): string {
  return s
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
    .replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&nbsp;/g, " ")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&amp;/g, "&");
}

function cleanXml(s: string): string {
  return entity(s.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1").replace(/<[^>]+>/g, " "))
    .replace(/\s+/g, " ")
    .trim();
}

function first(block: string, tag: string): string {
  const m = new RegExp(`<${tag}\\b[^>]*>([\\s\\S]*?)<\\/${tag}>`, "i").exec(block);
  return m ? cleanXml(m[1]) : "";
}

function all(block: string, tag: string): string[] {
  const out: string[] = [];
  const re = new RegExp(`<${tag}\\b[^>]*>([\\s\\S]*?)<\\/${tag}>`, "gi");
  let m: RegExpExecArray | null;
  while ((m = re.exec(block))) {
    const value = cleanXml(m[1]);
    if (value) out.push(value);
  }
  return out;
}

const MONTHS: Record<string, number> = {
  jan: 1, feb: 2, mar: 3, apr: 4, may: 5, jun: 6,
  jul: 7, aug: 8, sep: 9, oct: 10, nov: 11, dec: 12,
};

function monthNumber(raw: string): number {
  const n = Number(raw);
  if (Number.isFinite(n) && n >= 1 && n <= 12) return n;
  return MONTHS[raw.slice(0, 3).toLowerCase()] ?? 1;
}

function dateFromBlock(block: string): number {
  // ArticleDate suele ser la fecha electrónica exacta. Si falta, PubDate.
  const articleDate = /<ArticleDate\b[^>]*>([\s\S]*?)<\/ArticleDate>/i.exec(block)?.[1] ?? "";
  const pubDate = /<PubDate\b[^>]*>([\s\S]*?)<\/PubDate>/i.exec(block)?.[1] ?? "";
  const src = articleDate || pubDate;
  if (!src) return 0;
  const year = Number(first(src, "Year"));
  if (!Number.isFinite(year) || year < 1900) return 0;
  const month = monthNumber(first(src, "Month") || "1");
  const dayRaw = Number(first(src, "Day") || "1");
  const day = Number.isFinite(dayRaw) && dayRaw >= 1 && dayRaw <= 31 ? dayRaw : 1;
  return Date.UTC(year, month - 1, day, 12, 0, 0);
}

function abstractOf(block: string): string {
  const out: string[] = [];
  const re = /<AbstractText\b([^>]*)>([\s\S]*?)<\/AbstractText>/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(block))) {
    const text = cleanXml(m[2]);
    if (!text) continue;
    const label = /\bLabel="([^"]+)"/i.exec(m[1])?.[1];
    out.push(label ? `${label}: ${text}` : text);
  }
  return out.join(" ").replace(/\s+/g, " ").trim();
}

function tagFor(types: string[], journal: string): string {
  const lower = types.map((t) => t.toLowerCase());
  if (lower.some((t) => t.includes("practice guideline") || t === "guideline")) return "GUÍA";
  if (lower.some((t) => t.includes("meta-analysis"))) return "META";
  if (lower.some((t) => t.includes("systematic review"))) return "REV SIST";
  if (lower.some((t) => t.includes("randomized controlled trial"))) return "RCT";
  if (/^(n engl j med|lancet|jama|bmj|nat med)$/i.test(journal.trim())) return "TOP";
  return "PAPER";
}

export function parseMedicalXml(xml: string, limit = clampItems()): Item[] {
  const blocks = xml.match(/<PubmedArticle\b[\s\S]*?<\/PubmedArticle>/gi) ?? [];
  const items: Item[] = [];
  for (const block of blocks) {
    if (items.length >= limit) break;
    const pmid = Number(first(block, "PMID"));
    if (!Number.isFinite(pmid) || pmid <= 0) continue;

    const title = first(block, "ArticleTitle");
    if (!title) continue;

    const journalBlock = /<Journal\b[^>]*>([\s\S]*?)<\/Journal>/i.exec(block)?.[1] ?? "";
    const journal = first(journalBlock, "ISOAbbreviation") || first(journalBlock, "Title") || "PubMed";
    const types = all(block, "PublicationType");
    const tag = tagFor(types, journal);
    const abstract = abstractOf(block);
    const whenAt = dateFromBlock(block);

    // Con abstract suficiente no hace falta abrir pubmed.ncbi.nlm.nih.gov de
    // nuevo: el paquete ya tiene el cuerpo. Sin abstract sí se deja el enlace
    // para que news.ts intente rescatar texto de la página.
    const desc = abstract
      ? `PMID ${pmid}. ${journal}. ${abstract}`.slice(0, 6000)
      : `PMID ${pmid}. ${journal}. Publicación reciente indexada en PubMed.`;

    items.push({
      id: pmid,
      title: `[${tag}] ${title}`.slice(0, 500),
      when: "",
      whenAt,
      link: abstract.length >= 200 ? "" : `https://pubmed.ncbi.nlm.nih.gov/${pmid}/`,
      desc,
    });
  }
  return items;
}

export async function readMedicalFeed(): Promise<{ items: Item[]; title: string }> {
  const limit = clampItems();
  // Se piden más PMIDs de los que se muestran porque algunos registros no
  // traen título/abstract utilizable.
  const search = new URL(`${EUTILS}/esearch.fcgi`);
  search.searchParams.set("db", "pubmed");
  search.searchParams.set("retmode", "json");
  search.searchParams.set("retmax", String(Math.min(30, limit * 3)));
  search.searchParams.set("sort", "pub_date");
  search.searchParams.set("datetype", "edat");
  search.searchParams.set("reldate", String(RECENT_DAYS));
  search.searchParams.set("term", QUERY);

  const raw = await getText(search, MAX_JSON);
  const parsed = JSON.parse(raw) as { esearchresult?: { idlist?: unknown } };
  const ids = Array.isArray(parsed.esearchresult?.idlist)
    ? parsed.esearchresult!.idlist.filter((id): id is string => typeof id === "string" && /^\d+$/.test(id))
    : [];
  if (ids.length === 0) return { items: [], title: MEDICAL_FEED_NAME };

  const fetch = new URL(`${EUTILS}/efetch.fcgi`);
  fetch.searchParams.set("db", "pubmed");
  fetch.searchParams.set("retmode", "xml");
  fetch.searchParams.set("id", ids.join(","));
  const xml = await getText(fetch, MAX_XML);

  return { items: parseMedicalXml(xml, limit), title: MEDICAL_FEED_NAME };
}
