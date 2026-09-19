// Cómo se lee lo que dice el bot de libros (docs/ws397/LIBROS_CONTRATO.md).
// Funciones puras, sin Telegram ni red: se prueban de escritorio con los
// textos de las dos capturas del dueño (`./test/libros/run.sh`).
//
//   - La lista: cada línea `Título /comando` es un resultado; las demás
//     ("Ahí van algunos libros que coinciden con tu búsqueda:") se ignoran.
//     Las entidades de Telegram no hacen falta: el comando está en el texto.
//   - La ficha: `Título - Autor` en la primera línea (se parte por el ÚLTIMO
//     " - ", porque un título puede llevar guiones), "Publicado: 1967 | 345
//     páginas", después el género (la línea siguiente sin números) y el resto
//     es la descripción. Los formatos salen de los botones en línea que
//     parecen un formato de archivo; el resto ("Información", "Reportar
//     error") no viaja al aparato.

export type ListResult = { title: string; code: string };

export type Card = {
  title: string;
  author: string;
  year: number | null;
  pages: number | null;
  genre: string;
  desc: string;
  formats: string[];
};

const LINE_RE = /^(.+?)\s+(\/[A-Za-z0-9_]+)\s*$/;
const MAX_DESC = 2048;

// Lo que cuenta como formato de libro en el texto de un botón, en minúsculas y
// sin acentos. Se compara la etiqueta entera y también palabra por palabra,
// así "📕 EPUB" y "Epub" dan los dos "epub".
const FORMATS = ["epub", "pdf", "mobi", "azw3", "azw", "fb2", "kepub", "txt", "djvu", "docx", "doc", "rtf", "cbz", "cbr"];

export function fold(s: string): string {
  return s.normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase();
}

function lines(text: string): string[] {
  return text.replace(/\r/g, "").split("\n").map((l) => l.trim());
}

/** Los resultados de una lista `Título /comando`, en el orden del bot y sin repetir el comando. */
export function parseList(text: string): ListResult[] {
  const out: ListResult[] = [];
  const seen = new Set<string>();
  for (const line of lines(text)) {
    const m = LINE_RE.exec(line);
    if (!m) continue;
    const code = m[2];
    if (seen.has(code)) continue;
    seen.add(code);
    // Un bot puede numerar o poner viñeta: "1. Título /abc", "• Título /abc".
    const title = m[1].replace(/^(?:[-•*·]|\d+[.)])\s+/, "").trim();
    if (!title) continue;
    out.push({ title, code });
  }
  return out;
}

/** El formato que nombra la etiqueta de un botón ("Epub" → "epub"), o "" si no es un formato. */
export function formatOfLabel(label: string): string {
  const f = fold(label).replace(/[^a-z0-9]+/g, " ").trim();
  if (!f) return "";
  if (FORMATS.includes(f)) return f;
  for (const word of f.split(" ")) if (FORMATS.includes(word)) return word;
  return "";
}

/** La ficha de un libro y los formatos que ofrecen sus botones. */
export function parseCard(text: string, buttonLabels: string[]): Card {
  const ls = lines(text).filter((l) => l.length > 0);
  const first = ls[0] ?? "";
  const dash = first.lastIndexOf(" - ");
  const title = (dash > 0 ? first.slice(0, dash) : first).trim();
  const author = (dash > 0 ? first.slice(dash + 3) : "").trim();

  let year: number | null = null;
  let pages: number | null = null;
  let genre = "";
  let descFrom = 1;

  // "Publicado: 1967 | 345 páginas", o parecido en otro idioma: el año va en
  // esa línea; las páginas pueden ir ahí o en cualquier otra.
  const pubIdx = ls.findIndex((l, i) => i > 0 && /^(publicad|publish|publi|veröffentlicht|erschien|опубликован|издан|a[ñn]o|year|ann[ée]e|jahr|год)/i.test(fold(l)) && /\d{4}/.test(l));
  if (pubIdx > 0) {
    const y = /(\d{4})/.exec(ls[pubIdx]);
    if (y) year = Number(y[1]);
    descFrom = pubIdx + 1;
    const next = ls[pubIdx + 1];
    if (next && !/\d/.test(next) && next.length <= 60) {
      genre = next;
      descFrom = pubIdx + 2;
    }
  }
  for (let i = 1; i < ls.length; i++) {
    const p = /(\d+)\s*p[aá]g/i.exec(ls[i]);
    if (p) {
      pages = Number(p[1]);
      break;
    }
  }
  const desc = ls.slice(descFrom).join("\n").slice(0, MAX_DESC);

  const formats: string[] = [];
  for (const label of buttonLabels) {
    const f = formatOfLabel(label);
    if (f && !formats.includes(f)) formats.push(f);
  }
  // Epub primero: es el que el lector abre.
  formats.sort((a, b) => (a === "epub" ? -1 : b === "epub" ? 1 : 0));

  return { title, author, year, pages, genre, desc, formats };
}

/** "Cien años de soledad" → "cien-anos-de-soledad" (letras y números ASCII, tope 40). */
export function slugify(s: string): string {
  const slug = fold(s).replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40).replace(/-+$/, "");
  return slug || "libro";
}

/**
 * El nombre con el que se guarda el archivo: el del bot si es sano (letras,
 * números, punto, guion y guion bajo, hasta 48), y si no, el slug del título
 * con la extensión del bot o del formato pedido. `saveFile` rechaza cualquier
 * otro nombre, así que acá no se deja pasar ninguno.
 */
export function fileNameFor(botName: string | null | undefined, title: string, format: string): string {
  const clean = (botName ?? "").trim();
  if (/^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$/.test(clean)) return clean;
  const dot = clean.lastIndexOf(".");
  let ext = dot > 0 ? fold(clean.slice(dot + 1)).replace(/[^a-z0-9]/g, "") : "";
  if (!ext || ext.length > 5) ext = fold(format).replace(/[^a-z0-9]/g, "") || "epub";
  return `${slugify(title)}.${ext}`;
}

// ---------------------------------------------------------------- deletreo

// Lo que dice el usuario cuando deletrea un nombre ("a, ene, ge, e, ele, e,
// ese, espacio, eme, a, ese…" o "a-n-g-e-l-e-s"), vuelto palabra SIN el
// modelo. Se parte por comas, espacios y guiones; cada trozo es el nombre de
// una letra (en español o en inglés), una letra suelta, un número, "espacio"
// o "guion", o —si no es nada de eso y tiene más de una letra— una palabra
// entera que el usuario dijo en el medio ("cien, espacio, a, eñe, o, ese").
// Antes de partir se juntan los nombres de dos palabras ("doble uve", "i
// griega", "double you") y se tiran los que no son letras ("mayúscula").
// Lo que sale va en minúsculas: quien pone acentos y mayúsculas es el
// corrector, si hay modelo; sin él, el bot igual busca sin mirar mayúsculas.

const LETTERS_ES: Record<string, string> = {
  a: "a", be: "b", ce: "c", de: "d", e: "e", efe: "f", ge: "g", hache: "h", i: "i", jota: "j", ka: "k",
  ele: "l", eme: "m", ene: "n", "eñe": "ñ", o: "o", pe: "p", cu: "q", erre: "r", ere: "r", ese: "s", te: "t",
  u: "u", uve: "v", ve: "v", equis: "x", ye: "y", zeta: "z", ceta: "z",
  espacio: " ", guion: "-", "guión": "-",
};

const LETTERS_EN: Record<string, string> = {
  ay: "a", bee: "b", see: "c", cee: "c", dee: "d", e: "e", ee: "e", ef: "f", eff: "f", gee: "g", aitch: "h",
  aych: "h", i: "i", eye: "i", jay: "j", kay: "k", el: "l", ell: "l", em: "m", en: "n", oh: "o", o: "o", pee: "p",
  cue: "q", queue: "q", ar: "r", es: "s", ess: "s", tee: "t", you: "u", u: "u", vee: "v", ex: "x", why: "y",
  zee: "z", zed: "z", a: "a",
  space: " ", dash: "-", hyphen: "-",
};

// Nombres de más de una palabra → una sola ficha, ANTES de partir. Los de
// "doble" van con sus dos órdenes ("uve doble" también se dice).
const PHRASES: [RegExp, string][] = [
  [/\b(?:doble\s+(?:uve|ve|u)|(?:uve|ve|u)\s+doble|double\s+(?:you|u|yoo))\b/giu, " w "],
  [/\b(?:i\s+griega|y\s+griega)\b/giu, " y "],
  [/\bi\s+latina\b/giu, " i "],
  [/\b(?:be|b)\s+(?:larga|grande|alta)\b/giu, " b "],
  [/\b(?:ve|v|uve)\s+(?:corta|chica|baja|pequeña)\b/giu, " v "],
  [/\bcon\s+(?:acento|tilde)\b/giu, " "],
];

// Fichas que no son una letra ni una palabra del nombre: se tiran.
const IGNORE = new Set(["mayúscula", "mayúsculas", "minúscula", "minúsculas", "acento", "tilde", "letra",
  "capital", "uppercase", "lowercase", "coma", "punto"]);

/**
 * El nombre deletreado, letra por letra, vuelto palabra: "a, ene, ge, e, ele,
 * e, ese, espacio, eme, a, ese, te, erre, e, te, te, a" → "angeles mastretta".
 * `lang` decide qué tabla manda cuando un nombre vale en las dos ("de" es d en
 * español; "el" es l en inglés). Vacío si no hay nada que armar.
 */
export function lettersToWord(text: string, lang = "es"): string {
  let t = ` ${text.toLowerCase()} `;
  for (const [re, rep] of PHRASES) t = t.replace(re, rep);
  const table = lang === "en" ? { ...LETTERS_ES, ...LETTERS_EN } : { ...LETTERS_EN, ...LETTERS_ES };
  const words: string[] = [];
  let cur = "";
  const flush = () => {
    if (cur) words.push(cur);
    cur = "";
  };
  for (const raw of t.split(/[\s,;:¡!¿?"«»()/\-–—]+/u)) {
    const tok = raw.replace(/^[.'’]+|[.'’]+$/g, "");
    if (!tok) continue;
    if (IGNORE.has(tok)) continue;
    // Primero tal cual (así "eñe" no se confunde con "ene"), después sin acentos.
    const mapped = table[tok] ?? table[fold(tok)];
    if (mapped === " ") {
      flush();
    } else if (mapped !== undefined) {
      cur += mapped;
    } else if ([...tok].length === 1 || /^\d+$/.test(tok)) {
      cur += tok;
    } else {
      // Una palabra entera dicha en el medio: va suelta, entre espacios.
      flush();
      words.push(tok);
    }
  }
  flush();
  return words.join(" ").replace(/\s+/g, " ").trim();
}
