// Prepara un texto para que Piper lo LEA como lo diría una persona.
//
// El usuario reportó (1.5.41) que "384 000 km" se leía como "trescientos ochenta
// y cuatro cero cero cero ka eme". Son dos problemas distintos:
//   1. el separador de miles parte el número en pedazos, y
//   2. las abreviaturas de unidades se deletrean.
// Piper no normaliza nada de eso: le llega el texto tal cual. Así que se
// normaliza acá antes de sintetizar.
import type { Lang } from "./lang";

// ── Números a palabras (es / en) ──────────────────────────────────────────────
const ES_UNI = ["cero","uno","dos","tres","cuatro","cinco","seis","siete","ocho","nueve","diez",
  "once","doce","trece","catorce","quince","dieciséis","diecisiete","dieciocho","diecinueve","veinte",
  "veintiuno","veintidós","veintitrés","veinticuatro","veinticinco","veintiséis","veintisiete","veintiocho","veintinueve"];
const ES_DEC = ["","","","treinta","cuarenta","cincuenta","sesenta","setenta","ochenta","noventa"];
const ES_CEN = ["","ciento","doscientos","trescientos","cuatrocientos","quinientos","seiscientos","setecientos","ochocientos","novecientos"];

function esBelow1000(n: number): string {
  if (n === 100) return "cien";
  const c = Math.floor(n / 100);
  const r = n % 100;
  const head = c ? ES_CEN[c] : "";
  let tail = "";
  if (r < 30) tail = r ? ES_UNI[r] : "";
  else {
    const d = Math.floor(r / 10);
    const u = r % 10;
    tail = u ? `${ES_DEC[d]} y ${ES_UNI[u]}` : ES_DEC[d];
  }
  return [head, tail].filter(Boolean).join(" ");
}

function esNumber(n: number): string {
  if (n === 0) return "cero";
  const parts: string[] = [];
  const scales: [number, string, string][] = [
    [1e9, "mil millones", "mil millones"],
    [1e6, "millón", "millones"],
    [1e3, "mil", "mil"],
  ];
  let rest = n;
  for (const [value, one, many] of scales) {
    const count = Math.floor(rest / value);
    if (!count) continue;
    rest %= value;
    if (value === 1e3) parts.push(count === 1 ? "mil" : `${esBelow1000(count)} mil`);
    else parts.push(count === 1 ? `un ${one}` : `${esBelow1000(count)} ${many}`);
  }
  if (rest) parts.push(esBelow1000(rest));
  return parts.join(" ");
}

const EN_UNI = ["zero","one","two","three","four","five","six","seven","eight","nine","ten",
  "eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"];
const EN_DEC = ["","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"];

function enBelow1000(n: number): string {
  const c = Math.floor(n / 100);
  const r = n % 100;
  const head = c ? `${EN_UNI[c]} hundred` : "";
  let tail = "";
  if (r < 20) tail = r ? EN_UNI[r] : "";
  else {
    const d = Math.floor(r / 10);
    const u = r % 10;
    tail = u ? `${EN_DEC[d]}-${EN_UNI[u]}` : EN_DEC[d];
  }
  return [head, tail].filter(Boolean).join(" ");
}

function enNumber(n: number): string {
  if (n === 0) return "zero";
  const parts: string[] = [];
  let rest = n;
  for (const [value, name] of [[1e9, "billion"], [1e6, "million"], [1e3, "thousand"]] as [number, string][]) {
    const count = Math.floor(rest / value);
    if (!count) continue;
    rest %= value;
    parts.push(`${enBelow1000(count)} ${name}`);
  }
  if (rest) parts.push(enBelow1000(rest));
  return parts.join(" ");
}

// ── Unidades ──────────────────────────────────────────────────────────────────
// Solo las que se dicen en voz alta y se deletrearían mal. La clave se compara
// tal cual (sensible a mayúsculas: "m" es metro pero "M" no).
const UNITS: Record<Lang, Record<string, [string, string]>> = {
  es: { km: ["kilómetro","kilómetros"], m: ["metro","metros"], cm: ["centímetro","centímetros"],
        mm: ["milímetro","milímetros"], kg: ["kilo","kilos"], g: ["gramo","gramos"],
        l: ["litro","litros"], ml: ["mililitro","mililitros"], h: ["hora","horas"],
        min: ["minuto","minutos"], s: ["segundo","segundos"], "°C": ["grado","grados"],
        "%": ["por ciento","por ciento"], "km/h": ["kilómetro por hora","kilómetros por hora"] },
  en: { km: ["kilometre","kilometres"], m: ["metre","metres"], cm: ["centimetre","centimetres"],
        mm: ["millimetre","millimetres"], kg: ["kilo","kilos"], g: ["gram","grams"],
        l: ["litre","litres"], ml: ["millilitre","millilitres"], h: ["hour","hours"],
        min: ["minute","minutes"], s: ["second","seconds"], "°C": ["degree","degrees"],
        "%": ["percent","percent"], "km/h": ["kilometre per hour","kilometres per hour"] },
  fr: { km: ["kilomètre","kilomètres"], "%": ["pour cent","pour cent"], "°C": ["degré","degrés"] },
  de: { km: ["Kilometer","Kilometer"], "%": ["Prozent","Prozent"], "°C": ["Grad","Grad"] },
  pt: { km: ["quilômetro","quilômetros"], "%": ["por cento","por cento"], "°C": ["grau","graus"] },
  ru: { km: ["километр","километров"], "%": ["процент","процентов"], "°C": ["градус","градусов"] },
};

function words(n: number, lang: Lang): string | null {
  if (!Number.isFinite(n) || n < 0 || n > 999_999_999_999 || !Number.isInteger(n)) return null;
  if (lang === "es") return esNumber(n);
  if (lang === "en") return enNumber(n);
  return null;  // los demás idiomas: solo se junta el número, Piper lo lee
}

/**
 * Deja el texto listo para leer en voz alta: junta los separadores de miles,
 * pasa los números enteros a palabras (español e inglés) y estira las
 * abreviaturas de unidades. No toca el texto que se muestra en pantalla.
 */
export function speakable(text: string, lang: Lang): string {
  let t = text;

  // 1. Separador de miles: "384 000" y "384.000" y "384,000" -> "384000".
  //    Ojo con no romper los decimales ni las horas (14:30) ni las fechas.
  t = t.replace(/(\d)[  \u00a0\u202f](?=\d{3}\b)/g, "$1");
  t = t.replace(/(\d)[.,](?=\d{3}\b)(?!\d)/g, "$1");

  // 2. Número + unidad -> palabras. Se hace junto para poder concordar el
  //    plural ("un kilómetro" vs "dos kilómetros").
  const table = UNITS[lang] ?? UNITS.es;
  const unitKeys = Object.keys(table).sort((a, b) => b.length - a.length).map((u) =>
    u.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&"));
  const unitRe = new RegExp(`(\\d+)\\s?(${unitKeys.join("|")})(?![\\p{L}\\d])`, "gu");
  t = t.replace(unitRe, (whole, digits: string, unit: string) => {
    const n = Number(digits);
    const pair = table[unit];
    if (!pair) return whole;
    let spoken = words(n, lang) ?? digits;
    // Apócope: en español es "un kilómetro", no "uno kilómetro".
    if (lang === "es") spoken = spoken.replace(/\buno$/, "un");
    return `${spoken} ${n === 1 ? pair[0] : pair[1]}`;
  });

  // 3. Números sueltos que quedaron. Se saltean los que parecen hora, fecha,
  //    versión o código (llevan ':' '/' '-' pegado), que se leen mejor tal cual.
  t = t.replace(/(?<![\d:/.\-])\d{4,}(?![\d:/.\-])/g, (m) => words(Number(m), lang) ?? m);

  return t;
}
