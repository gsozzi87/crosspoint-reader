// Genera un EPUB de muestra con buildEpub() para que run.sh lo revise con
// unzip y un parser XML. Sin servidor ni modelo: la función es pura.
import { writeFileSync } from "node:fs";
import { buildEpub, slugify } from "../../server/src/epub";

const out = process.argv[2];
if (!out) throw new Error("uso: bun make.ts <salida.epub>");

const title = "La peste negra: cómo se vivía & qué cambió <después>";
const bytes = buildEpub({
  title,
  author: "Librito",
  subtitle: "Librito",
  lang: "es",
  date: "2026-09-18",
  identifier: "00000000-0000-4000-8000-000000000001",
  chapters: [
    { title: "Antes de la peste", paragraphs: ["Europa en 1340 era un continente lleno & apretado.", "Segundo párrafo con <etiquetas> y \"comillas\"."] },
    { title: "La llegada", paragraphs: ["Los barcos genoveses llegaron a Mesina en 1347.", "", "   ", "Un tercer párrafo tras dos vacíos."] },
    { title: "Qué cambió después", paragraphs: ["Los salarios subieron y los señores lo notaron."] },
  ],
  closing: { title: "Para seguir leyendo", paragraphs: ["Barbara Tuchman, Un espejo lejano.", "John Kelly, La gran mortandad."] },
});
writeFileSync(out, bytes);
console.log(`${out}: ${bytes.byteLength} bytes · slug=${slugify(title)}`);
if (slugify(title) !== "la-peste-negra-como-se-vivia-que-cambio") throw new Error(`slug inesperado: ${slugify(title)}`);
if (slugify("Война и мир") !== "librito") throw new Error("un título sin ASCII tiene que dar 'librito'");
if (slugify("Straße") !== "strasse") throw new Error("ß → ss");
