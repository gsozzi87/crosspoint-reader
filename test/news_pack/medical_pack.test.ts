// El camino de un PAPER dentro del paquete, entero y SIN RED. Lo que se prueba
// acá es lo que el dueño pidió con todas las letras: "no las quiero para
// público en general, las quiero para un médico, tienen que tener el lenguaje
// técnico con el que fueron escritos, no cambiar palabras sino traducirlas".
//
// O sea: que al modelo se le pida una TRADUCCIÓN y no un resumen, que el título
// llegue traducido, que un abstract corto no se cuele en inglés, que el
// presupuesto de los diarios no deje un paper sin traducir, y que la traducción
// no se vuelva a pagar en cada pasada.
//
// OJO: este archivo va en SU PROPIO proceso (por eso `run.sh` llama a `bun test`
// una vez por archivo). Fija variables de entorno que `news.ts` lee al cargarse
// y mockea módulos enteros, así que mezclarlo con otro archivo en la misma
// corrida le da un presupuesto distinto y falla.
import { expect, mock, test } from "bun:test";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const dir = mkdtempSync(join(tmpdir(), "news-medical-"));
process.env.STORE_FILE = join(dir, "store.json");
process.env.NEWS_FILE = join(dir, "news.json");
process.env.HUB_SETTINGS_FILE = join(dir, "hub-settings.json");
// Cero presupuesto para los diarios: si un paper igual se traduce, es que tiene
// el suyo propio. Esa es la diferencia entre "llega en español" y "llega en
// inglés porque el diario se comió las diez llamadas".
process.env.NEWS_DIGEST_PER_RUN = "0";

const SRC = join(import.meta.dir, "../../server/src");
const T0 = Date.parse("2026-09-19T12:00:00Z");

const LARGO = "METHODS: Randomized, double-blind trial. ".repeat(12);
const CORTO = "RESULTS: HR 0.74 (95% CI 0.61-0.89; p=0.002) for the primary endpoint at 24 months. CONCLUSIONS: The intervention reduced the primary composite endpoint without an excess of serious adverse events.";

await mock.module(`${SRC}/rss.ts`, () => ({
  DEFAULT_TZ: "UTC",
  whenLabel: (at: number) => new Date(at).toISOString().slice(11, 16),
  DownloadError: class extends Error { reason = "down"; },
  readFeed: async () => ({
    title: "Diario",
    items: [{ id: 1, title: "Noticia común", when: "", whenAt: T0, link: "", desc: "palabra ".repeat(120) }],
  }),
  download: async () => ({ body: "" }),
  extractArticle: () => ({ text: "" }),
}));

// El feed médico, sin salir a PubMed. Los títulos llevan la etiqueta que arma
// `parseMedicalXml` ([NEJM · RCT]), que es una marca NUESTRA y no parte del
// título del paper.
await mock.module(`${SRC}/medical.ts`, () => ({
  MEDICAL_SUMMARY_VERSION: 4,
  MEDICAL_URL: "pubmed:",
  MEDICAL_FEED_NAME: "Medicina · PubMed",
  isMedicalFeed: (url: string) => (url ?? "").startsWith("pubmed:"),
  readMedicalFeed: async () => ({
    title: "Medicina · PubMed",
    items: [
      { id: 11, title: "[NEJM · RCT] Semaglutide in heart failure", when: "", whenAt: T0, link: "", desc: `PMID 11. ${LARGO}` },
      { id: 22, title: "[JAMA · META] Short abstract trial", when: "", whenAt: T0 - 600_000, link: "", desc: `PMID 22. ${CORTO}` },
    ],
  }),
}));

const pedidos: { system: string; user: string; maxTokens?: number }[] = [];
await mock.module(`${SRC}/llm.ts`, () => ({
  chatText: async (req: { system: string; user: string; maxTokens?: number }) => {
    pedidos.push(req);
    const src = /^TÍTULO:\s*(.+)$/m.exec(req.user)?.[1] ?? "";
    return `TÍTULO: ES ${src}\n\n${"traducción clínica con HR 0.74 (IC95% 0.61-0.89) ".repeat(12)}`;
  },
}));

const { rebuild, loadPack } = await import(`${SRC}/news.ts`);
const { mutate } = await import(`${SRC}/store.ts`);

test("al paper se le pide una traducción técnica, no un resumen para todos", async () => {
  await mutate(1, (s) => {
    s.feeds = [
      { id: 1, name: "Diario", url: "https://diario.test/rss" },
      { id: 2, name: "Medicina · PubMed", url: "pubmed:" },
    ];
  });
  // REV-085: con `true` se pide el comportamiento de NEWS_PREFETCH — bajar los
  // cuerpos y masticarlos en la pasada. Lo que se prueba acá es el PROMPT y la
  // traducción, que no cambiaron de contenido, sólo de disparador: en el modo
  // normal esto mismo corre cuando el usuario abre la nota (`ensureBody`).
  await rebuild(1, "es", true);

  expect(pedidos.length).toBe(2);                     // los dos papers; el diario no, que tiene cupo 0
  for (const p of pedidos) {
    expect(p.system).toContain("UN MÉDICO");
    expect(p.system).toContain("TRADUCCIÓN, no un resumen para público general");
    expect(p.system).toContain("no cambies las palabras, tradúcelas");
    expect(p.system).toContain("español neutro");
    expect(p.system).toContain("NO lo acortes");
    // Lo que NO se traduce tiene que estar nombrado, o el modelo "mejora" las
    // dosis y las siglas y el paper deja de servir para buscarlo después.
    expect(p.system).toContain("HR/RR/OR");
    expect(p.system).toContain("IC95%");
    // Y traducir entero necesita más lugar que resumir.
    expect(p.maxTokens ?? 0).toBeGreaterThan(1100);
  }
});

test("el título llega traducido y con la etiqueta intacta", async () => {
  const pack = await loadPack(1);
  const paper = pack.items.find((i) => i.id === "2-11")!;
  // La etiqueta es nuestra y no va al modelo; el título sí, y vuelve traducido.
  expect(paper.title).toBe("[NEJM · RCT] ES Semaglutide in heart failure");
  expect(pedidos[0].user.startsWith("TÍTULO: Semaglutide in heart failure")).toBe(true);
  expect(pedidos[0].user).not.toContain("[NEJM · RCT]");
  // Y el cuerpo guardado lleva el mismo título que el manifiesto.
  expect(pack.bodies["2-11"].title).toBe(paper.title);
});

test("un abstract corto igual se traduce: en inglés no sirve", async () => {
  const pack = await loadPack(1);
  const corto = pack.items.find((i) => i.id === "2-22")!;
  expect(corto.chewed).toBe(true);
  expect(pack.bodies["2-22"].text).toContain("traducción clínica");
});

test("el presupuesto de los diarios no deja un paper sin traducir", async () => {
  const pack = await loadPack(1);
  // El diario se quedó sin masticar (NEWS_DIGEST_PER_RUN=0) y los papers no.
  expect(pack.items.find((i) => i.id === "1-1")!.chewed).toBe(false);
  expect(pack.items.filter((i) => i.id.startsWith("2-")).every((i) => i.chewed)).toBe(true);
});

test("la traducción no se vuelve a pagar en cada pasada", async () => {
  // El título guardado es la TRADUCCIÓN, así que sin `srcTitle` la nota se vería
  // distinta de la del feed en cada vuelta y se traduciría de nuevo para siempre.
  const antes = pedidos.length;
  await rebuild(1, "es");
  expect(pedidos.length).toBe(antes);
  const pack = await loadPack(1);
  expect(pack.items.find((i) => i.id === "2-11")!.title).toBe("[NEJM · RCT] ES Semaglutide in heart failure");
});
