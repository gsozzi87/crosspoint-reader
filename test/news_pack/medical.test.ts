import { expect, test } from "bun:test";

import {
  MEDICAL_FEED_ID,
  MEDICAL_FEED_NAME,
  MEDICAL_SUMMARY_VERSION,
  parseMedicalXml,
} from "../../server/src/medical";

const article = (p: {
  pmid: number;
  journal: string;
  title: string;
  type: string;
  day: number;
  doi?: string;
  abstract?: string;
}) => `
  <PubmedArticle>
    <MedlineCitation>
      <PMID Version="1">${p.pmid}</PMID>
      <Article>
        <Journal>
          <JournalIssue><PubDate><Year>2026</Year><Month>Sep</Month><Day>${p.day}</Day></PubDate></JournalIssue>
          <ISOAbbreviation>${p.journal}</ISOAbbreviation>
        </Journal>
        <ArticleTitle>${p.title}</ArticleTitle>
        <Abstract>
          <AbstractText Label="METHODS">${p.abstract ?? "Randomized study with clinically relevant methods and outcomes described in sufficient detail for medical review."}</AbstractText>
          <AbstractText Label="RESULTS">The primary endpoint and safety results were reported with enough text to keep this record clinically useful.</AbstractText>
        </Abstract>
        <PublicationTypeList><PublicationType>${p.type}</PublicationType></PublicationTypeList>
        <ArticleDate DateType="Electronic"><Year>2026</Year><Month>09</Month><Day>${p.day}</Day></ArticleDate>
      </Article>
    </MedlineCitation>
    <PubmedData><ArticleIdList>${p.doi ? `<ArticleId IdType="doi">${p.doi}</ArticleId>` : ""}</ArticleIdList></PubmedData>
  </PubmedArticle>`;

test("la fuente médica usa un id reservado y versión de resumen", () => {
  expect(MEDICAL_FEED_ID).toBe(2_000_000_000);
  expect(MEDICAL_FEED_NAME).toContain("PubMed");
  expect(MEDICAL_SUMMARY_VERSION).toBeGreaterThan(1);
});

test("prioriza señal clínica sobre recencia bruta", () => {
  const xml = `<PubmedArticleSet>
    ${article({ pmid: 1, journal: "Unknown Journal", title: "Small recent trial", type: "Randomized Controlled Trial", day: 19 })}
    ${article({ pmid: 2, journal: "N Engl J Med", title: "Important trial", type: "Randomized Controlled Trial", day: 15, doi: "10.1000/nejm.test" })}
    ${article({ pmid: 3, journal: "Another Journal", title: "Practice recommendation", type: "Practice Guideline", day: 18 })}
  </PubmedArticleSet>`;
  const items = parseMedicalXml(xml, 3);
  expect(items[0].id).toBe(2);
  expect(items[0].title).toContain("[NEJM · RCT]");
  expect(items[1].id).toBe(3);
  expect(items[1].title).toContain("[GUÍA]");
});

test("conserva PMID, DOI, abstract y enlace de PubMed", () => {
  const xml = `<PubmedArticleSet>${article({
    pmid: 12345678,
    journal: "JAMA",
    title: "A clinically useful randomized trial.",
    type: "Randomized Controlled Trial",
    day: 19,
    doi: "10.1001/jama.2026.123",
    abstract: "First sentence with &amp; entities and the study design, population, intervention and comparator clearly described.",
  })}</PubmedArticleSet>`;
  const [item] = parseMedicalXml(xml, 8);
  expect(item.title).toContain("[JAMA · RCT]");
  expect(item.desc).toContain("PMID 12345678");
  expect(item.desc).toContain("DOI 10.1001/jama.2026.123");
  expect(item.desc).toContain("& entities");
  expect(item.sourceLink).toBe("https://pubmed.ncbi.nlm.nih.gov/12345678/");
  expect(item.whenAt).toBe(Date.UTC(2026, 8, 19, 12, 0, 0));
});

test("respeta el límite sin inventar ítems", () => {
  const xml = `<PubmedArticleSet>${article({ pmid: 1, journal: "JAMA", title: "A", type: "Meta-Analysis", day: 19 })}${article({ pmid: 2, journal: "BMJ", title: "B", type: "Systematic Review", day: 18 })}</PubmedArticleSet>`;
  expect(parseMedicalXml(xml, 1)).toHaveLength(1);
  expect(parseMedicalXml("<PubmedArticleSet/>", 8)).toEqual([]);
});
