import { expect, test } from "bun:test";

import { MEDICAL_FEED_ID, MEDICAL_FEED_NAME, parseMedicalXml } from "../../server/src/medical";

const XML = `<?xml version="1.0"?>
<PubmedArticleSet>
  <PubmedArticle>
    <MedlineCitation>
      <PMID Version="1">12345678</PMID>
      <Article>
        <Journal>
          <JournalIssue><PubDate><Year>2026</Year><Month>Sep</Month><Day>18</Day></PubDate></JournalIssue>
          <Title>New England Journal of Medicine</Title>
          <ISOAbbreviation>N Engl J Med</ISOAbbreviation>
        </Journal>
        <ArticleTitle>A clinically useful randomized trial.</ArticleTitle>
        <Abstract>
          <AbstractText Label="BACKGROUND">First sentence with &amp; entities.</AbstractText>
          <AbstractText Label="RESULTS">Second sentence with results and enough text to represent an abstract body for the reader.</AbstractText>
        </Abstract>
        <PublicationTypeList>
          <PublicationType>Randomized Controlled Trial</PublicationType>
        </PublicationTypeList>
        <ArticleDate DateType="Electronic"><Year>2026</Year><Month>09</Month><Day>19</Day></ArticleDate>
      </Article>
    </MedlineCitation>
  </PubmedArticle>
</PubmedArticleSet>`;

test("la fuente médica usa un id reservado estable", () => {
  expect(MEDICAL_FEED_ID).toBe(2_000_000_000);
  expect(MEDICAL_FEED_NAME).toContain("PubMed");
});

test("convierte PubMed XML al formato de Noticias", () => {
  const [item] = parseMedicalXml(XML, 8);
  expect(item.id).toBe(12345678);
  expect(item.title).toContain("[RCT]");
  expect(item.title).toContain("randomized trial");
  expect(item.desc).toContain("BACKGROUND:");
  expect(item.desc).toContain("& entities");
  expect(item.whenAt).toBe(Date.UTC(2026, 8, 19, 12, 0, 0));
});

test("respeta el límite sin inventar ítems", () => {
  expect(parseMedicalXml(XML + XML, 1)).toHaveLength(1);
  expect(parseMedicalXml("<PubmedArticleSet/>", 8)).toEqual([]);
});
