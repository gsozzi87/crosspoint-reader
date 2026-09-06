// Biblia en el aparato. Los textos (JSON de thiagobodruk/bible, uno por
// idioma: Reina-Valera, King James, Bible de l'Épée, Schlachter, Almeida,
// Sinodal) se bajan en el Dockerfile a /opt/bible; si faltan se traen a /data.
// El aparato pide capítulos de a uno (máx. 12 KB) y los cachea en la SD.
//
//   GET  /api/bible/books?lang=xx                 -> { ok, books: [{ i, name, chapters }] }
//   GET  /api/bible/chapter?lang=xx&book=i&chapter=n -> { ok, book, name, chapter, chapters, text }
//        text = versículos numerados, uno por línea ("16 Porque de tal manera...")
//   GET  /api/bible/day?lang=xx                   -> { ok, ref, text }   versículo del día
//   GET  /api/bible/find?lang=xx&q=texto          -> referencia ("Juan 3 16", "Salmo 23") o búsqueda
//        -> { ok, kind: "ref", book, chapter, verse } | { ok, kind: "search", results: [{book, name, chapter, verse, text}] }
//        Sin LLM: parser de referencias con los nombres del idioma y búsqueda de texto normalizado.
import { Hono } from "hono";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { normalizeLang, type Lang } from "./lang";
import { BOOK_NAMES } from "./bibleNames";

const SOURCES: Record<Lang, string> = {
  es: "es_rvr", en: "en_kjv", fr: "fr_apee", de: "de_schlachter", pt: "pt_aa", ru: "ru_synodal",
};
const DIR = process.env.BIBLE_DIR ?? "/opt/bible";
const CACHE_DIR = process.env.BIBLE_CACHE ?? "/data/bible";
const RAW = "https://raw.githubusercontent.com/thiagobodruk/bible/master/json";

type Book = { abbrev: string; name: string; chapters: string[][] };
const loaded = new Map<Lang, Book[]>();

async function bible(lang: Lang): Promise<Book[] | null> {
  const have = loaded.get(lang);
  if (have) return have;
  const file = `${SOURCES[lang]}.json`;
  let raw: string | null = null;
  for (const p of [`${DIR}/${file}`, `${CACHE_DIR}/${file}`]) {
    if (existsSync(p)) {
      raw = await readFile(p, "utf8");
      break;
    }
  }
  if (raw === null) {
    try {
      const res = await fetch(`${RAW}/${file}`);
      if (!res.ok) throw new Error(`bible ${res.status}`);
      raw = await res.text();
      await mkdir(CACHE_DIR, { recursive: true });
      await writeFile(`${CACHE_DIR}/${file}`, raw);
    } catch (err) {
      console.error("bible:", err);
      return null;
    }
  }
  const books = JSON.parse(raw.replace(/^﻿/, "")) as Book[];
  loaded.set(lang, books);
  return books;
}

function norm(s: string): string {
  return s.normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase().replace(/[^a-z0-9а-яё ]+/gi, " ").replace(/\s+/g, " ").trim();
}

// "primera de juan 3 16", "juan 3:16", "salmo 23", "1 corintios 13" -> índice de libro y números.
function parseRef(q: string, lang: Lang): { book: number; chapter: number; verse: number } | null {
  let s = norm(q);
  const ordinals: Record<string, string> = {
    primera: "1", primero: "1", segunda: "2", segundo: "2", tercera: "3", tercero: "3",
    first: "1", second: "2", third: "3", premiere: "1", premier: "1", deuxieme: "2", troisieme: "3",
    erste: "1", erster: "1", zweite: "2", dritte: "3", primeira: "1", primeiro: "1", terceira: "3",
    первая: "1", первое: "1", вторая: "2", второе: "2", третья: "3", третье: "3",
  };
  s = s.split(" ").map((w) => ordinals[w] ?? w).join(" ").replace(/\b(de|of|du|des|von|da|do)\b/g, " ").replace(/\s+/g, " ").trim();
  const m = /^(.*?)\s*(\d{1,3})(?:\s*[: ]\s*(\d{1,3}))?$/.exec(s);
  if (!m) return null;
  const nameWords = m[1].trim();
  if (!nameWords) return null;
  const names = BOOK_NAMES[lang].map(norm);
  let book = names.findIndex((n) => n === nameWords);
  if (book < 0) book = names.findIndex((n) => n.startsWith(nameWords));
  if (book < 0) {
    // "salmo 23" (singular), "1 reyes" without dot, etc.: word-by-word prefix match
    book = names.findIndex((n) => {
      const a = n.split(" "), b = nameWords.split(" ");
      return a.length === b.length && a.every((w, i) => w.startsWith(b[i].slice(0, 4)) || b[i].startsWith(w.slice(0, 4)));
    });
  }
  if (book < 0) return null;
  return { book, chapter: Number(m[2]), verse: m[3] ? Number(m[3]) : 0 };
}

export const bibleApi = new Hono();

bibleApi.get("/books", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const books = await bible(lang);
  if (!books) return c.json({ ok: false, error: "bible unavailable" }, 503);
  return c.json({ ok: true, books: books.map((b, i) => ({ i, name: BOOK_NAMES[lang][i] ?? b.name, chapters: b.chapters.length })) });
});

bibleApi.get("/chapter", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const books = await bible(lang);
  if (!books) return c.json({ ok: false, error: "bible unavailable" }, 503);
  const book = Number(c.req.query("book"));
  const chapter = Number(c.req.query("chapter"));
  const b = books[book];
  if (!b || !(chapter >= 1 && chapter <= b.chapters.length)) return c.json({ ok: false, error: "bad reference" }, 400);
  const text = b.chapters[chapter - 1].map((v, i) => `${i + 1} ${v}`).join("\n");
  return c.json({ ok: true, book, name: BOOK_NAMES[lang][book], chapter, chapters: b.chapters.length, text });
});

// Un versículo por día de una lista corta de referencias queridas.
const DAILY: [number, number, number][] = [
  [42, 3, 16], [18, 23, 1], [22, 41, 10], [19, 3, 5], [44, 8, 28], [49, 4, 13], [39, 11, 28], [18, 46, 1],
  [57, 11, 1], [23, 29, 11], [42, 14, 27], [45, 13, 4], [18, 119, 105], [5, 1, 9], [39, 6, 33], [18, 27, 1],
  [58, 1, 5], [47, 5, 22], [44, 12, 2], [48, 2, 8], [18, 34, 8], [39, 5, 3], [42, 8, 12], [60, 5, 7],
  [18, 91, 1], [22, 40, 31], [39, 28, 20], [61, 4, 8], [42, 15, 5], [18, 37, 4], [46, 12, 9],
];

bibleApi.get("/day", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const books = await bible(lang);
  if (!books) return c.json({ ok: false, error: "bible unavailable" }, 503);
  const day = Math.floor(Date.now() / 86_400_000);
  const [b, ch, v] = DAILY[day % DAILY.length];
  const text = books[b]?.chapters[ch - 1]?.[v - 1] ?? "";
  return c.json({ ok: true, book: b, chapter: ch, verse: v, ref: `${BOOK_NAMES[lang][b]} ${ch}:${v}`, text });
});

bibleApi.get("/find", async (c) => {
  const lang = normalizeLang(c.req.query("lang"));
  const q = (c.req.query("q") ?? "").trim().slice(0, 120);
  if (!q) return c.json({ ok: false, error: "q required" }, 400);
  const books = await bible(lang);
  if (!books) return c.json({ ok: false, error: "bible unavailable" }, 503);
  const ref = parseRef(q, lang);
  if (ref && books[ref.book] && ref.chapter <= books[ref.book].chapters.length) {
    return c.json({ ok: true, kind: "ref", book: ref.book, name: BOOK_NAMES[lang][ref.book], chapter: ref.chapter, verse: ref.verse });
  }
  // Búsqueda de texto: todas las palabras (normalizadas) en el versículo.
  const words = norm(q).split(" ").filter((w) => w.length > 2);
  if (!words.length) return c.json({ ok: true, kind: "search", results: [] });
  const results: { book: number; name: string; chapter: number; verse: number; text: string }[] = [];
  outer: for (let b = 0; b < books.length; b++) {
    const chapters = books[b].chapters;
    for (let ch = 0; ch < chapters.length; ch++) {
      for (let v = 0; v < chapters[ch].length; v++) {
        const t = norm(chapters[ch][v]);
        if (words.every((w) => t.includes(w))) {
          results.push({ book: b, name: BOOK_NAMES[lang][b], chapter: ch + 1, verse: v + 1, text: chapters[ch][v] });
          if (results.length >= 12) break outer;
        }
      }
    }
  }
  return c.json({ ok: true, kind: "search", results });
});
