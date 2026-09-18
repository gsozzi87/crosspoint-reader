// Un EPUB armado a mano, sin dependencias nativas: es un ZIP con un puñado de
// archivos de texto. `fflate` (JavaScript puro) hace el ZIP; el resto son
// plantillas.
//
// Lo que un lector exige y por qué:
//   - `mimetype` es la PRIMERA entrada y va SIN comprimir (stored): la
//     especificación lo pide así para que el tipo se pueda leer en los
//     primeros bytes del archivo, y más de un lector lo rechaza si no.
//   - `META-INF/container.xml` apunta al `.opf`, que lista cada archivo
//     (manifest) y el orden de lectura (spine); `toc.ncx` es el índice de
//     EPUB 2, que es lo que entiende el lector de CrossPoint.
//   - Cada capítulo es un XHTML bien formado: el parser del lector es XML, no
//     HTML, así que un `&` suelto o un `<p>` sin cerrar lo rompe. Todo el
//     texto pasa por `esc()`.
//
// Función pura a propósito: se prueba de escritorio (test/epub/run.sh) con
// `unzip -t` y un parser XML, sin servidor ni modelo.
import { zipSync, strToU8 } from "fflate";
import { randomUUID } from "node:crypto";

export type EpubChapter = { title: string; paragraphs: string[] };

export type EpubInput = {
  title: string;
  author: string;
  lang: string;
  chapters: EpubChapter[];
  /** Sección final ("Para seguir leyendo"): un capítulo más, fuera de la numeración. */
  closing?: EpubChapter;
  /** Segunda línea de la portada; por omisión, el autor. */
  subtitle?: string;
  /** Fecha para la portada y los metadatos (YYYY-MM-DD); por omisión, hoy. */
  date?: string;
  /** urn:uuid fijo, para pruebas reproducibles; por omisión, uno nuevo. */
  identifier?: string;
};

export function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// Nombre de archivo a partir del título: ASCII, guiones, hasta `max` letras.
// Un título en ruso o solo con signos daría vacío: entonces "librito".
export function slugify(title: string, max = 40): string {
  const ascii = title
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/ß/g, "ss")
    .replace(/[^A-Za-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .toLowerCase();
  let out = ascii.slice(0, max);
  // Si el corte cayó a mitad de una palabra, se corta en la palabra anterior.
  if (ascii.length > max && ascii[max] !== "-" && out.includes("-")) out = out.slice(0, out.lastIndexOf("-"));
  return out.replace(/-+$/g, "") || "librito";
}

function today(): string {
  return new Date().toISOString().slice(0, 10);
}

const XHTML_HEAD = (lang: string, title: string) =>
  `<?xml version="1.0" encoding="utf-8"?>\n` +
  `<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">\n` +
  `<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="${esc(lang)}">\n<head>\n<meta http-equiv="Content-Type" content="application/xhtml+xml; charset=utf-8" />\n` +
  `<title>${esc(title)}</title>\n<link rel="stylesheet" type="text/css" href="style.css" />\n</head>\n<body>\n`;

const XHTML_FOOT = `</body>\n</html>\n`;

function paragraphs(list: string[]): string {
  return list
    .map((p) => p.trim())
    .filter(Boolean)
    .map((p) => `<p>${esc(p)}</p>\n`)
    .join("");
}

function chapterXhtml(lang: string, ch: EpubChapter): string {
  return `${XHTML_HEAD(lang, ch.title)}<h2>${esc(ch.title)}</h2>\n${paragraphs(ch.paragraphs)}${XHTML_FOOT}`;
}

function titleXhtml(i: EpubInput, date: string): string {
  return (
    `${XHTML_HEAD(i.lang, i.title)}<div class="cover">\n<h1>${esc(i.title)}</h1>\n` +
    `<p class="sub">${esc(i.subtitle ?? i.author)}</p>\n<p class="date">${esc(date)}</p>\n</div>\n${XHTML_FOOT}`
  );
}

const STYLE = [
  "body { font-family: serif; line-height: 1.4; margin: 1em; }",
  "h1 { font-size: 1.8em; margin: 2em 0 0.5em; text-align: center; }",
  "h2 { font-size: 1.3em; margin: 1.5em 0 0.8em; }",
  "p { margin: 0 0 0.8em; text-align: justify; }",
  ".cover { text-align: center; }",
  ".cover .sub { font-size: 1.1em; margin-top: 1em; }",
  ".cover .date { color: #555; }",
  "",
].join("\n");

export function buildEpub(i: EpubInput): Uint8Array {
  const date = i.date ?? today();
  const id = i.identifier ?? randomUUID();
  const lang = i.lang || "es";

  // Las secciones en orden de lectura: portada, capítulos, cierre.
  type Item = { file: string; itemId: string; title: string; xhtml: string };
  const items: Item[] = [{ file: "title.xhtml", itemId: "title", title: i.title, xhtml: titleXhtml(i, date) }];
  i.chapters.forEach((ch, n) => {
    items.push({ file: `ch${n + 1}.xhtml`, itemId: `ch${n + 1}`, title: ch.title, xhtml: chapterXhtml(lang, ch) });
  });
  if (i.closing) items.push({ file: "closing.xhtml", itemId: "closing", title: i.closing.title, xhtml: chapterXhtml(lang, i.closing) });

  const manifest = items
    .map((it) => `    <item id="${it.itemId}" href="${it.file}" media-type="application/xhtml+xml"/>`)
    .concat([`    <item id="css" href="style.css" media-type="text/css"/>`, `    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>`])
    .join("\n");
  const spine = items.map((it) => `    <itemref idref="${it.itemId}"/>`).join("\n");

  const opf =
    `<?xml version="1.0" encoding="utf-8"?>\n` +
    `<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="2.0">\n` +
    `  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">\n` +
    `    <dc:title>${esc(i.title)}</dc:title>\n` +
    `    <dc:creator opf:role="aut">${esc(i.author)}</dc:creator>\n` +
    `    <dc:language>${esc(lang)}</dc:language>\n` +
    `    <dc:identifier id="bookid">urn:uuid:${esc(id)}</dc:identifier>\n` +
    `    <dc:date>${esc(date)}</dc:date>\n` +
    `  </metadata>\n  <manifest>\n${manifest}\n  </manifest>\n  <spine toc="ncx">\n${spine}\n  </spine>\n</package>\n`;

  const navPoints = items
    .map(
      (it, n) =>
        `    <navPoint id="nav${n + 1}" playOrder="${n + 1}">\n      <navLabel><text>${esc(it.title)}</text></navLabel>\n      <content src="${it.file}"/>\n    </navPoint>`,
    )
    .join("\n");
  const ncx =
    `<?xml version="1.0" encoding="utf-8"?>\n` +
    `<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">\n` +
    `<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">\n` +
    `  <head>\n    <meta name="dtb:uid" content="urn:uuid:${esc(id)}"/>\n    <meta name="dtb:depth" content="1"/>\n` +
    `    <meta name="dtb:totalPageCount" content="0"/>\n    <meta name="dtb:maxPageNumber" content="0"/>\n  </head>\n` +
    `  <docTitle><text>${esc(i.title)}</text></docTitle>\n  <navMap>\n${navPoints}\n  </navMap>\n</ncx>\n`;

  const container =
    `<?xml version="1.0" encoding="utf-8"?>\n` +
    `<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n` +
    `  <rootfiles>\n    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>\n  </rootfiles>\n</container>\n`;

  // El orden de las claves es el orden de las entradas del ZIP: `mimetype`
  // primero y con nivel 0 (stored), que es lo que exige la especificación.
  const files: Record<string, Uint8Array | [Uint8Array, { level: 0 }]> = {
    mimetype: [strToU8("application/epub+zip"), { level: 0 }],
    "META-INF/container.xml": strToU8(container),
    "OEBPS/content.opf": strToU8(opf),
    "OEBPS/toc.ncx": strToU8(ncx),
    "OEBPS/style.css": strToU8(STYLE),
  };
  for (const it of items) files[`OEBPS/${it.file}`] = strToU8(it.xhtml);
  return zipSync(files, { level: 6 });
}
