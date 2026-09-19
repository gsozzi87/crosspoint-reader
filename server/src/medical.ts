// Fuente médica curada desde PubMed para la pantalla Noticias.
//
// El servidor consulta E-utilities, trae un conjunto amplio de candidatos y los
// ordena por señal clínica antes de entregar sólo los mejores al paquete. El
// ESP32 recibe exactamente el mismo formato que un RSS normal.
//
// La prioridad es deliberada: revistas generales de alto impacto, guías,
// ensayos fase III/RCT y evidencia secundaria; después revistas de especialidad
// mayores. Editoriales, cartas, comentarios y casos quedan afuera.
//
// NCBI_EMAIL y NCBI_API_KEY son opcionales, pero permiten identificar la app.
import type { Item } from "./rss";
import { safeFetchAt, textCappedSmart } from "./net";

export const MEDICAL_FEED_ID = 2_000_000_000;
export const MEDICAL_FEED_NAME = "Medicina · PubMed";
export const MEDICAL_SUMMARY_VERSION = 2;

const EUTILS = "https://eutils.ncbi.nlm.nih.gov/entrez/eutils";
const MAX_XML = 4_000_000;
const MAX_JSON = 500_000;
const DEFAULT_ITEMS = 10;
const MAX_ITEMS = 12;
const RECENT_DAYS = 21;
const CANDIDATES = 80;

const QUERY = [
  "(",
  '"Randomized Controlled Trial"[Publication Type]',
  'OR "Clinical Trial, Phase III"[Publication Type]',
  'OR "Meta-Analysis"[Publication Type]',
  'OR "Systematic Review"[Publication Type]',
  'OR "Practice Guideline"[Publication Type]',
  'OR "Guideline"[Publication Type]',
  'OR "N Engl J Med"[Journal]',
  'OR "Lancet"[Journal]',
  'OR "JAMA"[Journal]',
  'OR "BMJ"[Journal]',
  'OR "Ann Intern Med"[Journal]',
  'OR "Nat Med"[Journal]',
  ")",
  "NOT (",
  '"Editorial"[Publication Type]',
  'OR "Letter"[Publication Type]',
  'OR "Comment"[Publication Type]',
  'OR "Case Reports"[Publication Type]',
  ")",
].join(" ");

const GENERAL: Record<string, { label: string; score: number }> = {
  "n engl j med": { label: "NEJM", score: 140 },
  lancet: { label: "LANCET", score: 140 },
  jama: { label: "JAMA", score: 135 },
  bmj: { label: "BMJ", score: 125 },
  "ann intern med": { label: "ANNALS", score: 120 },
  "nat med": { label: "NAT MED", score: 120 },
};

const SPECIALTY: Record<string, { label: string; score: number }> = {
  "jama intern med": { label: "JAMA IM", score: 80 },
  "jama pediatr": { label: "JAMA PED", score: 80 },
  "jama surg": { label: "JAMA SURG", score: 80 },
  "lancet oncol": { label: "LANCET ONC", score: 85 },
  "lancet neurol": { label: "LANCET NEUROL", score: 85 },
  "lancet infect dis": { label: "LANCET ID", score: 85 },
  circulation: { label: "CIRC", score: 80 },
  "j am coll cardiol": { label: "JACC", score: 80 },
  "eur heart j": { label: "EHJ", score: 75 },
  blood: { label: "BLOOD", score: 75 },
  "j clin oncol": { label: "JCO", score: 75 },
  gastroenterology: { label: "GASTRO", score: 70 },
  gut: { label: "GUT", score: 70 },
  "kidney int": { label: "KIDNEY INT", score: 70 },
  neurology: { label: "NEUROLOGY", score: 70 },
  "diabetes care": { label: "DIABETES CARE", score: 70 },
  "am j respir crit care med": { label: "AJRCCM", score: 70 },
  "obstet gynecol": { label: "OBGYN", score: 65 },
  "br j dermatol": { label: "BJD", score: 65 },
  "j am acad dermatol": { label: "JAAD", score: 65 },
  "clin infect dis": { label: "CID", score: 65 },
};

type Candidate = {
  item: Item;
  score: number;
  journal: string;
  types: string[];
};

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

function articleId(block: string, kind: string): string {
  const re = new RegExp(`<ArticleId\\b[^>]*IdType=["']${kind}["'][^>]*>([\\s\\S]*?)<\\/ArticleId>`, "i");
  const m = re.exec(block);
  return m ? cleanXml(m[1]) : "";
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
    const label = /\bLabel=["']([^"']+)["']/i.exec(m[1])?.[1];
    out.push(label ? `${label}: ${text}` : text);
  }
  return out.join(" ").replace(/\s+/g, " ").trim();
}

function journalInfo(journal: string): { label: string; score: number } | null {
  const key = journal.trim().toLowerCase();
  return GENERAL[key] ?? SPECIALTY[key] ?? null;
}

function evidence(types: string[]): { label: string; score: number } {
  const lower = types.map((t) => t.toLowerCase());
  if (lower.some((t) => t.includes("practice guideline") || t === "guideline")) return { label: "GUÍA", score: 105 };
  if (lower.some((t) => t.includes("clinical trial, phase iii"))) return { label: "FASE III", score: 70 };
  if (lower.some((t) => t.includes("randomized controlled trial"))) return { label: "RCT", score: 55 };
  if (lower.some((t) => t.includes("meta-analysis"))) return { label: "META", score: 50 };
  if (lower.some((t) => t.includes("systematic review"))) return { label: "REV SIST", score: 35 };
  return { label: "PAPER", score: 0 };
}

function labelFor(journal: string, types: string[]): string {
  const j = journalInfo(journal);
  const e = evidence(types);
  if (j && e.label !== "PAPER") return `${j.label} · ${e.label}`;
  return j?.label ?? e.label;
}

function candidateScore(journal: string, types: string[], abstract: string, whenAt: number): number {
  const j = journalInfo(journal);
  const e = evidence(types);
  let score = (j?.score ?? 0) + e.score;
  if (abstract.length >= 700) score += 8;
  else if (abstract.length < 250) score -= 15;
  if (whenAt > 0) {
    const ageDays = Math.max(0, (Date.now() - whenAt) / 86_400_000);
    score += Math.max(0, 12 - Math.floor(ageDays / 2));
  }
  return score;
}

function parseCandidate(block: string): Candidate | null {
  const pmid = Number(first(block, "PMID"));
  if (!Number.isFinite(pmid) || pmid <= 0) return null;

  const title = first(block, "ArticleTitle");
  if (!title) return null;

  const journalBlock = /<Journal\b[^>]*>([\s\S]*?)<\/Journal>/i.exec(block)?.[1] ?? "";
  const journal = first(journalBlock, "ISOAbbreviation") || first(journalBlock, "Title") || "PubMed";
  const types = all(block, "PublicationType");
  const abstract = abstractOf(block);
  const whenAt = dateFromBlock(block);
  const doi = articleId(block, "doi");
  const label = labelFor(journal, types);

  const citation = [
    `PMID ${pmid}`,
    journal,
    doi ? `DOI ${doi}` : "",
  ].filter(Boolean).join(" · ");

  const desc = abstract
    ? `${citation}. ${abstract}`.slice(0, 6000)
    : `${citation}. Publicación reciente indexada en PubMed.`;

  const item: Item = {
    id: pmid,
    title: `[${label}] ${title}`.slice(0, 500),
    when: "",
    whenAt,
    link: abstract.length >= 200 ? "" : `https://pubmed.ncbi.nlm.nih.gov/${pmid}/`,
    desc,
  };
  return { item, score: candidateScore(journal, types, abstract, whenAt), journal, types };
}

export function parseMedicalXml(xml: string, limit = clampItems()): Item[] {
  const blocks = xml.match(/<PubmedArticle\b[\s\S]*?<\/PubmedArticle>/gi) ?? [];
  const candidates: Candidate[] = [];
  for (const block of blocks) {
    const candidate = parseCandidate(block);
    if (candidate) candidates.push(candidate);
  }
  candidates.sort((a, b) => b.score - a.score || b.item.whenAt - a.item.whenAt || b.item.id - a.item.id);
  return candidates.slice(0, limit).map((c) => c.item);
}

export async function readMedicalFeed(): Promise<{ items: Item[]; title: string }> {
  const limit = clampItems();
  const search = new URL(`${EUTILS}/esearch.fcgi`);
  search.searchParams.set("db", "pubmed");
  search.searchParams.set("retmode", "json");
  search.searchParams.set("retmax", String(CANDIDATES));
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
