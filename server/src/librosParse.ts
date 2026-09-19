// Cómo se lee lo que dice el bot de libros (docs/ws397/LIBROS_CONTRATO.md).
// Funciones puras, sin Telegram ni red: se prueban de escritorio con los
// textos de las dos capturas del dueño (`./test/libros/run.sh`).
//
//   - La lista: cada línea `Título /comando` es un resultado; las demás
//     ("Ahí van algunos libros que coinciden con tu búsqueda:") se ignoran.
//     Las entidades de Telegram no hacen falta: el comando está en el texto.
//     Una entrada puede ser un AUTOR y no un libro: el bot la manda con la
//     cuenta de libros entre corchetes (`Ángeles Mastretta [11] /a7kE`), que
//     sale como `count` y NO se queda pegada al título.
//   - Lo que contesta el bot a un comando puede ser una ficha O OTRA LISTA
//     (el catálogo de ese autor). `classifyReply` lo decide antes de parsear:
//     leer una lista como ficha deja el catálogo entero metido adentro de la
//     descripción, que es justo lo que pasaba.
//   - La lista larga viene PAGINADA, con botones de flecha (⏮ ◀ ▶ ⏭). Cuáles
//     son de navegación lo dice `navOfLabel`, y no se puede confundir con los
//     botones de formato (`formatOfLabel`), que son los que dan el archivo.
//   - La ficha: `Título - Autor` en la primera línea (se parte por el ÚLTIMO
//     " - ", porque un título puede llevar guiones), "Publicado: 1967 | 345
//     páginas", después el género (la línea siguiente sin números) y el resto
//     es la descripción. Los formatos salen de los botones en línea que
//     parecen un formato de archivo; el resto ("Información", "Reportar
//     error") no viaja al aparato.

export type ListResult = { title: string; code: string; count?: number };

/** Hacia dónde lleva un botón de navegación de una lista paginada. */
export type NavDir = "first" | "prev" | "next" | "last";

/** Qué es lo que contestó el bot a un comando: una ficha o otra lista. */
export type ReplyKind = "card" | "list";

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
// "Ángeles Mastretta [11]" → el título y cuántos libros tiene ese autor.
// SÓLO corchetes: un título entre paréntesis termina en el año muchas veces
// ("Cien años de soledad (1967)") y eso no es una cuenta de libros.
const COUNT_RE = /^(.*\S)\s*\[(\d{1,4})\]$/;
// La línea del año de una ficha ("Publicado: 1967 | 345 páginas"), en los seis
// idiomas del producto. Se usa para parsear la ficha Y para reconocerla.
const PUB_RE = /^(publicad|publish|publi|veröffentlicht|erschien|опубликован|издан|a[ñn]o|year|ann[ée]e|jahr|год)/i;
const PAGES_RE = /(\d+)\s*p[aá]g/i;
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
    let title = m[1].replace(/^(?:[-•*·]|\d+[.)])\s+/, "").trim();
    // Un AUTOR viene con cuántos libros tiene: "Ángeles Mastretta [11]". El
    // número sale aparte; si sacarlo dejara el título vacío, se deja tal cual.
    let count: number | undefined;
    const c = COUNT_RE.exec(title);
    if (c && c[1].trim() && Number(c[2]) > 0) {
      title = c[1].trim();
      count = Number(c[2]);
    }
    if (!title) continue;
    out.push(count === undefined ? { title, code } : { title, code, count });
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
  const pubIdx = ls.findIndex((l, i) => i > 0 && PUB_RE.test(fold(l)) && /\d{4}/.test(l));
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
    const p = PAGES_RE.exec(ls[i]);
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

// ------------------------------------------------- lista o ficha, y páginas

/**
 * ¿Ese texto tiene forma de FICHA? Alcanza con la línea del año ("Publicado:
 * 1967") o con las páginas ("345 páginas"): una lista de resultados no las
 * lleva. Se usa sólo para desempatar; el que manda es el botón de formato.
 */
export function looksLikeCard(text: string): boolean {
  const ls = lines(text).filter((l) => l.length > 0);
  for (let i = 1; i < ls.length; i++) {
    if (PUB_RE.test(fold(ls[i])) && /\d{4}/.test(ls[i])) return true;
    if (PAGES_RE.test(ls[i])) return true;
  }
  return false;
}

/**
 * Qué contestó el bot al comando de un resultado: la ficha de un libro, o
 * OTRA LISTA (el catálogo de un autor, que en la lista de búsqueda viene como
 * `Ángeles Mastretta [11] /a7kE`).
 *
 * El que manda es el BOTÓN: sólo una ficha ofrece el archivo, así que un
 * botón de formato (Epub, PDF…) la decide sin mirar el texto. Sin ninguno,
 * dos o más líneas `Título /comando` son una lista, salvo que el texto además
 * tenga forma de ficha (año o páginas), que es cuando la descripción de un
 * libro nombra otros comandos.
 *
 * Una lista de UNA sola entrada (el autor con un solo libro) se lee como
 * ficha: distinguirla de una ficha cuya última línea termina en `/algo` no se
 * puede sin adivinar, y equivocarse para ese lado deja un resultado que al
 * abrirlo vuelve a la misma pantalla.
 */
export function classifyReply(text: string, buttonLabels: string[]): ReplyKind {
  for (const label of buttonLabels) if (formatOfLabel(label)) return "card";
  if (parseList(text).length >= 2 && !looksLikeCard(text)) return "list";
  return "card";
}

// Las flechas con las que un bot pagina una lista. Los dobles van ANTES que
// los simples: "«" y "<<" son el primero, no el anterior.
const NAV_SYMBOLS: [string, NavDir][] = [
  ["\u23ee", "first"], ["\u23ea", "first"], ["\u00ab", "first"], ["<<", "first"], ["|<", "first"],
  ["\u23ed", "last"], ["\u23e9", "last"], ["\u00bb", "last"], [">>", "last"], [">|", "last"],
  ["\u25c0", "prev"], ["\u2b05", "prev"], ["\u2190", "prev"], ["\u2039", "prev"], ["<", "prev"],
  ["\u25b6", "next"], ["\u27a1", "next"], ["\u2192", "next"], ["\u203a", "next"], [">", "next"],
];

// La etiqueta ENTERA, sin acentos ni símbolos: se compara completa a
// propósito. Palabra por palabra, "Más información" sería "siguiente".
const NAV_PHRASES: Record<string, NavDir> = {
  primera: "first", "primera pagina": "first", primero: "first", first: "first", "first page": "first",
  inicio: "first", principio: "first",
  anterior: "prev", anteriores: "prev", "pagina anterior": "prev", atras: "prev", prev: "prev",
  previous: "prev", "previous page": "prev", back: "prev", "ver anteriores": "prev",
  siguiente: "next", siguientes: "next", "pagina siguiente": "next", next: "next", "next page": "next",
  mas: "next", "ver mas": "next", "mas resultados": "next", more: "next", "show more": "next", adelante: "next",
  ultima: "last", "ultima pagina": "last", ultimo: "last", last: "last", "last page": "last", final: "last",
};

/**
 * Hacia dónde lleva el botón de una lista paginada ("▶" o "Siguiente" →
 * "next"), o "" si no es de navegación. Un botón de FORMATO nunca lo es: eso
 * se comprueba primero, así "PDF ▶" no se lee como una flecha.
 */
export function navOfLabel(label: string): NavDir | "" {
  const raw = (label ?? "").trim();
  if (!raw || formatOfLabel(raw)) return "";
  const words = fold(raw).replace(/[^a-z0-9]+/g, " ").trim();
  if (words) {
    const dir = NAV_PHRASES[words];
    return dir ?? "";
  }
  for (const [sym, dir] of NAV_SYMBOLS) if (raw.includes(sym)) return dir;
  return "";
}

/** "2/5", "Página 2 de 5", "2 de 5" → `{at:2, of:5}`; cualquier otra cosa, null. */
export function pageOfLabel(label: string): { at: number; of: number } | null {
  const t = fold(label ?? "").replace(/[^a-z0-9/]+/g, " ").trim();
  const m = /^(?:pagina|page|pag|seite|strona|pagina)?\s*(\d{1,4})\s*(?:\/|de|of|von|sur)\s*(\d{1,4})$/.exec(t);
  if (!m) return null;
  const at = Number(m[1]);
  const of = Number(m[2]);
  if (!(at > 0 && of > 0 && at <= of)) return null;
  return { at, of };
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
