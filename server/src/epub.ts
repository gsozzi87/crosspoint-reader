import { randomUUID } from "node:crypto";

type EpubSource = { title: string; url: string; source?: string };

const enc = new TextEncoder();

function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function u16(n: number): Uint8Array {
  return new Uint8Array([n & 255, (n >>> 8) & 255]);
}

function u32(n: number): Uint8Array {
  return new Uint8Array([n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255]);
}

function join(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

// EPUB es un ZIP. Guardar sin compresión produce archivos un poco mayores,
// pero evita una dependencia nativa y sigue el estándar (mimetype debe ser el
// primer miembro y no puede comprimirse).
function zip(files: { name: string; data: Uint8Array }[]): Uint8Array {
  const local: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  for (const file of files) {
    const name = enc.encode(file.name);
    const crc = crc32(file.data);
    const head = join([
      u32(0x04034b50), u16(20), u16(0x0800), u16(0), u16(0), u16(0), u32(crc),
      u32(file.data.length), u32(file.data.length), u16(name.length), u16(0), name,
    ]);
    local.push(head, file.data);
    central.push(join([
      u32(0x02014b50), u16(20), u16(20), u16(0x0800), u16(0), u16(0), u16(0), u32(crc),
      u32(file.data.length), u32(file.data.length), u16(name.length), u16(0), u16(0),
      u16(0), u16(0), u32(0), u32(offset), name,
    ]));
    offset += head.length + file.data.length;
  }
  const directory = join(central);
  return join([
    ...local,
    directory,
    u32(0x06054b50), u16(0), u16(0), u16(files.length), u16(files.length),
    u32(directory.length), u32(offset), u16(0),
  ]);
}

function xml(s: string): string {
  return s.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;").replaceAll("'", "&apos;");
}

function paragraphs(text: string): string {
  return text.split(/\n{2,}/).map((raw) => raw.trim()).filter(Boolean).map((raw) => {
    const heading = raw.match(/^(?:#{1,3}\s*|CAP[IÍ]TULO\s+\d+[:.]?\s*)(.+)$/i);
    if (heading && !raw.includes("\n")) return `<h2>${xml(heading[1].replace(/\*\*/g, ""))}</h2>`;
    return `<p>${xml(raw.replace(/^[-*]\s+/gm, "• ").replace(/\*\*/g, "")).replaceAll("\n", "<br/>")}</p>`;
  }).join("\n");
}

export function researchEpub(title: string, text: string, sources: EpubSource[], lang: string): Uint8Array {
  const id = `urn:uuid:${randomUUID()}`;
  const made = new Date().toISOString().slice(0, 10);
  const sourceItems = sources.length
    ? sources.map((s) => `<li><a href="${xml(s.url)}">${xml(s.title || s.source || s.url)}</a></li>`).join("\n")
    : "<li>No se obtuvieron enlaces adicionales.</li>";
  const css = "body{font-family:serif;line-height:1.45;margin:5%;}h1,h2{font-family:sans-serif;}a{color:#111;}li{margin:.5em 0;}";
  const chapter = `<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" lang="${xml(lang)}"><head><title>${xml(title)}</title><link rel="stylesheet" href="style.css" type="text/css"/></head><body><h1>${xml(title)}</h1><p><small>Investigación generada el ${made}.</small></p>${paragraphs(text)}<h2>Fuentes</h2><ol>${sourceItems}</ol></body></html>`;
  const nav = `<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Índice</title></head><body><nav epub:type="toc"><h1>Índice</h1><ol><li><a href="research.xhtml">${xml(title)}</a></li></ol></nav></body></html>`;
  const opf = `<?xml version="1.0" encoding="utf-8"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="book-id">${id}</dc:identifier><dc:title>${xml(title)}</dc:title><dc:language>${xml(lang)}</dc:language><dc:creator>CrossPoint Reader</dc:creator><meta property="dcterms:modified">${new Date().toISOString().replace(/\.\d{3}Z$/, "Z")}</meta></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="research" href="research.xhtml" media-type="application/xhtml+xml"/><item id="css" href="style.css" media-type="text/css"/></manifest><spine><itemref idref="research"/></spine></package>`;
  const container = `<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>`;
  return zip([
    { name: "mimetype", data: enc.encode("application/epub+zip") },
    { name: "META-INF/container.xml", data: enc.encode(container) },
    { name: "OEBPS/content.opf", data: enc.encode(opf) },
    { name: "OEBPS/nav.xhtml", data: enc.encode(nav) },
    { name: "OEBPS/research.xhtml", data: enc.encode(chapter) },
    { name: "OEBPS/style.css", data: enc.encode(css) },
  ]);
}
