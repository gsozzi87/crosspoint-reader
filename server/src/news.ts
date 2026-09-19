// El paquete de noticias: el servidor mastica, el aparato sólo lee.
//
// Hasta ahora Noticias era 100 % bajo demanda: el aparato entraba, pedía los
// titulares, y recién al abrir uno se bajaba y se limpiaba el artículo. Eso
// significa esperar con WiFi arriba por cada nota, y que sin señal no haya nada.
//
// Acá se da vuelta: el servidor recorre los feeds SOLO, cada hora, se mete en
// cada noticia, la deja legible y arma un PAQUETE. El aparato, cuando se
// conecta por cualquier motivo, se baja lo que le falte de un tirón y después
// lee sin red.
//
// Por qué manifiesto + un archivo por nota, y no un JSON grande:
//   - `ServerClient` no tiene streaming y copia el cuerpo DOS veces, así que un
//     JSON de 100 KB son 200 KB de heap interno en un aparato que tiene 230 KB
//     libres.
//   - El manifiesto dice qué cambió (por el sha), así que la segunda
//     sincronización del día no baja nada. Con el tope de 40 notas son ~9 KB
//     (unos 220 bytes por nota: id, medio, título, hora, sha, bytes, chewed),
//     o sea ~18 KB de heap mientras `ServerClient` lo copia: entra cómodo.
//
// LA VENTANA RODANTE. El paquete no es "las 18 últimas de todos juntos": es
// hasta PER_FEED notas POR MEDIO, y en cada pasada entra lo nuevo y se suelta
// lo más viejo (`rollingWindow`, por fecha y no por el orden del arreglo).
//
// LO QUE CUESTA UNA PASADA, con los topes de abajo y tres diarios cargados:
//   - Bajadas del diario: sólo las notas NUEVAS, como mucho NEW_PER_FEED (12)
//     por medio y NEW_PER_RUN (30) en toda la pasada, a ARTICLE_BUDGET_MS
//     (12 s) de tope cada una. O sea 30 bajadas en el peor caso —seis minutos
//     de pasada, y la pasada corre sola en segundo plano, con `/api/news/pack`
//     contestando mientras tanto el paquete anterior—; lo normal son uno o dos
//     segundos por nota. Lo que ya estaba y no cambió de título NO se vuelve a
//     bajar, y la nota de la que ya no se pudo sacar texto tampoco (`failed`):
//     ésa es la parte barata, y es la que hace que a la segunda pasada la
//     pasada entera cueste casi nada.
//   - Llamadas al modelo: como mucho DIGEST_PER_RUN (10). Lo masticado se
//     conserva entre pasadas (la nota reusada mantiene su texto y su `chewed`),
//     así que el paquete se va masticando entero solo en dos o tres horas sin
//     gastar más por hora que antes. Por eso este número NO subió con el tope.
import { Hono } from "hono";

import { accountOf, type AppEnv } from "./tenant";
import { DEFAULT_ACCOUNT, sha256Hex } from "./db";
import { mutateDoc, readDoc } from "./fsjson";
import { chatText } from "./llm";
import { normalizeLang, type Lang } from "./lang";
import { DEFAULT_TZ, download, DownloadError, extractArticle, readFeed, whenLabel } from "./rss";
import { load as loadStore } from "./store";
import { addUsage, overQuota } from "./usage";
import { isMedicalFeed, MEDICAL_SUMMARY_VERSION, readMedicalFeed } from "./medical";

// Cuántas notas lleva el paquete y cuántas de ésas pasan por el modelo. El
// resto van con el texto limpiado a mano, que es gratis y casi siempre alcanza.
//
// PER_FEED es el número que se ve: con 12, cada diario aporta hasta doce
// titulares, que es lo que se ve en la página del diario. PACK_ITEMS es sólo el
// techo de todo junto (tres diarios de doce entran holgados).
const PACK_ITEMS = Number(process.env.NEWS_PACK_ITEMS ?? 40);
const PER_FEED = Number(process.env.NEWS_PER_FEED ?? 12);
// Cuántos titulares se MIRAN por medio. El triple de PER_FEED a propósito: una
// nota de la que no se pudo sacar cuerpo no gasta un lugar del cupo, así que
// hace falta de dónde reponer (ver el bucle de abajo). Mirar más adentro del
// feed NO cuesta más: lo que cuesta es bajar, y eso lo topea NEW_PER_FEED.
const CANDIDATES_PER_FEED = PER_FEED * 3;
// La nota de la que no se pudo sacar cuerpo se ANOTA. Si no, cada pasada se
// vuelve a bajar la misma nota rota, se gasta el tope de bajadas en ella y el
// diario se queda para siempre en tres o cuatro titulares. Se reintenta a las
// seis horas (un diario puede haber estado caído un rato) y se olvida a los dos
// días, que es cuando la nota ya no está ni en el feed.
const FAIL_RETRY_MS = 6 * 60 * 60 * 1000;
const FAIL_FORGET_MS = 48 * 60 * 60 * 1000;
const FAIL_MAX = 500;
// El costo de la pasada, y el único freno que importa: cada nota que no estaba
// antes se baja del diario y eso son hasta ARTICLE_BUDGET_MS. Lo ya conocido es
// gratis, así que el paquete crece hasta el cupo en dos o tres pasadas.
const NEW_PER_FEED = Number(process.env.NEWS_NEW_PER_FEED ?? 12);
const NEW_PER_RUN = Number(process.env.NEWS_NEW_PER_RUN ?? 30);
const DIGEST_PER_RUN = Number(process.env.NEWS_DIGEST_PER_RUN ?? 10);
// Presupuesto de modelo de los PAPERS, aparte del de los diarios: una noticia
// sin masticar se lee igual, un paper sin traducir llega en inglés. Está topeado
// por arriba de todos modos, porque PubMed aporta como mucho NEWS_MEDICAL_ITEMS
// (10, tope 12) por pasada y lo ya traducido no se vuelve a traducir.
const MEDICAL_DIGEST_PER_RUN = Number(process.env.NEWS_MEDICAL_DIGEST ?? 12);
const REFRESH_MS = Number(process.env.NEWS_REFRESH_MS ?? 60 * 60 * 1000);
const ARTICLE_BUDGET_MS = 12000;
// Un cuerpo de más de esto no entra cómodo en el aparato ni aporta nada: son
// unos 12 minutos de lectura.
const MAX_BODY = 6000;

export type PackItem = {
  id: string;      // "<feedId>-<itemId>", estable entre pasadas
  feed: string;    // el nombre del medio, ya como lo muestra la pantalla
  title: string;
  when: string;
  sha: string;     // del cuerpo: si no cambió, el aparato no lo vuelve a bajar
  bytes: number;
  chewed: boolean; // pasó por el modelo
  link: string;
  // Cuándo se publicó, para ordenar la ventana rodante. NO viaja al aparato
  // (la ruta /pack elige campo por campo): el aparato ya tiene `when`, que es
  // la etiqueta hecha. Opcional porque un paquete guardado antes de 1.5.115 no
  // lo tiene; se completa solo en la primera pasada que reusa la nota.
  whenAt?: number;
  // Versión del formato de resumen médico. No viaja al aparato; sólo invalida
  // el cuerpo guardado cuando cambia el prompt clínico.
  medicalVersion?: number;
  // El título ORIGINAL de PubMed (en inglés), cuando `title` es la traducción.
  // No viaja al aparato: existe para comparar contra lo que trae el feed y
  // saber si la nota cambió. Sin esto, traducir el título haría que cada pasada
  // lo viera distinto del original y volviera a traducir todo cada hora.
  srcTitle?: string;
};

type Body = { id: string; title: string; feed: string; when: string; text: string };
// `failed`: id de la nota -> cuándo se intentó bajarla y no dio cuerpo.
type Pack = { at: string; items: PackItem[]; bodies: Record<string, Body>; failed: Record<string, number> };

// Conserva las últimas notas buenas de los feeds que no contestaron en esta
// pasada. Una caída temporal de un diario no puede convertir el manifiesto en
// uno vacío y ordenar al aparato que borre toda su caché.
export function carryUnavailable(
  previous: PackItem[],
  bodies: Record<string, Body>,
  unavailable: ReadonlySet<number>,
): PackItem[] {
  return previous.filter((item) => {
    const dash = item.id.indexOf("-");
    if (dash <= 0 || !bodies[item.id]) return false;
    return unavailable.has(Number(item.id.slice(0, dash)));
  });
}

function shape(raw: unknown): Pack {
  const p = (raw && typeof raw === "object" ? raw : {}) as Partial<Pack>;
  return {
    at: typeof p.at === "string" ? p.at : "",
    items: Array.isArray(p.items) ? (p.items as PackItem[]) : [],
    bodies: p.bodies && typeof p.bodies === "object" ? (p.bodies as Record<string, Body>) : {},
    failed: p.failed && typeof p.failed === "object" ? (p.failed as Record<string, number>) : {},
  };
}

export async function loadPack(accountId: number): Promise<Pack> {
  return shape(await readDoc<unknown>(accountId, "news", null));
}

const PROMPT: Record<Lang, string> = {
  es: "Reescribe esta noticia para que se lea cómoda en una pantalla chica: qué pasó, dónde, a quién afecta y por qué importa. Español neutro, frases cortas, sin opinión y sin inventar nada que no esté en el texto. Entre cinco y diez frases, en párrafos, texto plano sin títulos ni viñetas.",
  en: "Rewrite this news story so it reads well on a small screen: what happened, where, who it affects and why it matters. Short sentences, no opinion, invent nothing that is not in the text. Five to ten sentences, plain text, no headings or bullets.",
  fr: "Réécris cette actualité pour un petit écran : ce qui s'est passé, où, qui est concerné et pourquoi c'est important. Phrases courtes, sans opinion, sans rien inventer. Cinq à dix phrases, texte brut.",
  de: "Schreibe diese Nachricht für einen kleinen Bildschirm um: was passiert ist, wo, wen es betrifft und warum es wichtig ist. Kurze Sätze, keine Meinung, nichts erfinden. Fünf bis zehn Sätze, reiner Text.",
  pt: "Reescreve esta notícia para um ecrã pequeno: o que aconteceu, onde, quem afeta e porque importa. Frases curtas, sem opinião, sem inventar nada. Cinco a dez frases, texto simples.",
  ru: "Перепиши эту новость для маленького экрана: что произошло, где, кого касается и почему это важно. Короткие фразы, без мнений, ничего не выдумывай. Пять-десять фраз, простой текст.",
};

// El abstract de PubMed viene SIEMPRE en inglés y el que lee es UN MÉDICO, así
// que acá el modelo NO hace lo mismo que con un diario. Con una noticia se
// reescribe para pantalla chica; con un paper se TRADUCE y nada más.
//
// Regla del dueño, textual: "no las quiero para público en general, las quiero
// para un médico, tienen que tener el lenguaje técnico con el que fueron
// escritos, no cambiar palabras sino traducirlas". O sea: "ensayo clínico
// aleatorizado", no "un estudio en el que se sorteó a los pacientes"; y lo que
// no tiene equivalente aceptado se queda en inglés. Simplificar un abstract es
// romperlo: el que lo lee necesita el término exacto para buscarlo después.
//
// `MEDICAL_SUMMARY_VERSION` (medical.ts) sube cuando esto cambia, así los
// cuerpos ya guardados se vuelven a traducir en vez de quedar con el prompt
// viejo.
const MEDICAL_TARGET: Record<Lang, { name: string; say: string }> = {
  es: { name: "español neutro", say: "Escribe en español neutro." },
  en: { name: "inglés", say: "Write in English." },
  fr: { name: "francés", say: "Rédige en français." },
  de: { name: "alemán", say: "Schreibe auf Deutsch." },
  pt: { name: "português", say: "Escreve em português." },
  ru: { name: "ruso", say: "Пиши на русском языке." },
};

// La primera línea de la respuesta es el título traducido. Va en el mismo
// pedido y no en uno aparte: son dos llamadas al modelo por paper si se separan,
// y el título tiene que traducirse con la MISMA regla que el cuerpo.
const TITLE_TAG = "TÍTULO:";

function medicalPrompt(lang: Lang): string {
  const target = MEDICAL_TARGET[lang];
  return [
    `El texto que sigue es el abstract de un artículo médico y va dirigido a UN MÉDICO. Tradúcelo a ${target.name}.`,
    "Es una TRADUCCIÓN, no un resumen para público general ni una reescritura.",
    "REGLA PRINCIPAL: no cambies las palabras, tradúcelas. Cada término técnico va al término que usan los médicos",
    `en ${target.name} ("ensayo clínico aleatorizado", no "un estudio en el que se sorteó a los pacientes").`,
    "Si un término no tiene equivalente aceptado, déjalo en inglés.",
    "Conserva EXACTAMENTE y sin traducir: nombres de fármacos y moléculas, dosis y vías, HR/RR/OR, IC95%, valores de p,",
    "NNT, unidades, siglas de escalas y de ensayos, nombres propios de estudios, genes, microorganismos, PMID y DOI.",
    "Conserva todas las cifras como están y la estructura del abstract con sus secciones",
    "(antecedentes, métodos, resultados, conclusiones) cuando las traiga.",
    "No agregues nada, no interpretes, no saques conclusiones propias y no digas que cambia la práctica.",
    "NO lo acortes: un abstract ya es corto. Saca sólo lo repetido y las frases de relleno.",
    "Si ya está en el idioma pedido, devuélvelo tal cual, sin reescribirlo.",
    `La PRIMERA línea es "${TITLE_TAG} " y el título traducido con la misma regla; después una línea en blanco y la traducción.`,
    "Texto plano, sin markdown.",
    target.say,
  ].join(" ");
}

// Parte la respuesta del modelo en título y cuerpo. Pura a propósito: el
// formato de una respuesta del modelo es lo primero que se rompe y esto se
// prueba sin red (./test/news_pack/run.sh). Sin la primera línea marcada, el
// título se deja como estaba y el texto entero es el cuerpo — nunca se pierde
// la traducción por culpa del formato.
export function splitTitledAnswer(out: string): { title: string; text: string } {
  const clean = out.replace(/\r\n/g, "\n").trim();
  const nl = clean.indexOf("\n");
  // Los asteriscos se sacan ANTES de mirar la línea: el modelo escribe
  // "**TÍTULO:** X" bastante seguido y buscarlos dentro del patrón deja el
  // título con las marcas pegadas. `TITLE` también, por si contesta en inglés.
  const firstLine = (nl < 0 ? clean : clean.slice(0, nl)).replace(/\*/g, "").trim();
  const m = /^(?:T[ÍI]TULO|TITLE)\s*:\s*(.+)$/i.exec(firstLine);
  if (!m) return { title: "", text: clean };
  const title = m[1]!.trim().replace(/^["“]|["”]$/g, "").trim();
  const text = nl < 0 ? "" : clean.slice(nl + 1).trim();
  return { title, text };
}

// Lo que el modelo le hace a una nota. Con un diario es masticar (reescribir
// para pantalla chica); con un paper es traducir sin tocar el registro. Si el
// modelo falla se devuelve el texto limpiado a mano: el paquete NUNCA queda sin
// la nota por culpa del modelo — en el caso médico eso significa que llega en
// inglés, que es como está escrito, y no que no llegue.
async function chew(
  accountId: number,
  text: string,
  lang: Lang,
  medical = false,
  title = "",
): Promise<{ text: string; chewed: boolean; title: string }> {
  const raw = text.slice(0, MAX_BODY);
  // Un abstract corto igual hay que traducirlo: el piso de 400 existe para no
  // gastar el modelo reescribiendo un párrafo de diario que ya se lee bien, y
  // eso no aplica a un texto que está en otro idioma.
  if (raw.length < (medical ? 120 : 400)) return { text: raw, chewed: false, title: "" };
  try {
    const system = medical ? medicalPrompt(lang) : PROMPT[lang];
    const user = medical && title ? `${TITLE_TAG} ${title}\n\n${raw}` : raw;
    // Traducir entero necesita más lugar que resumir: el cuerpo llega hasta
    // MAX_BODY (6000) y el español y el ruso se estiran contra el inglés.
    const out = await chatText({ system, user, maxTokens: medical ? 2600 : 900 });
    await addUsage(accountId, { llm: 1 });
    const parsed = medical ? splitTitledAnswer(out) : { title: "", text: out.trim() };
    if (parsed.text.length > 200) return { text: parsed.text, chewed: true, title: parsed.title };
  } catch (err) {
    console.error("news chew:", String(err).slice(0, 120));
  }
  return { text: raw, chewed: false, title: "" };
}

// Uno de cada feed y después la segunda vuelta, en vez de los primeros N de la
// lista pegada: un diario que publica cada diez minutos si no se come el
// paquete entero y los otros no aparecen nunca. `perFeed` es el tope por medio
// (cada vuelta toma como mucho uno de cada uno, así que alcanza con cortar las
// vueltas). Pura a propósito, para poder probarla sin red
// (./test/news_pack/run.sh).
export function interleave<T>(
  feeds: { feed: string; id: number; items: T[] }[],
  max: number,
  perFeed = Number.POSITIVE_INFINITY,
): { feed: string; feedId: number; item: T }[] {
  const out: { feed: string; feedId: number; item: T }[] = [];
  const masLargo = feeds.reduce((n, f) => Math.max(n, f.items.length), 0);
  const vueltas = Math.min(masLargo, Math.max(0, perFeed));
  for (let round = 0; round < vueltas && out.length < max; round++) {
    for (const f of feeds) {
      const it = f.items[round];
      if (it === undefined) continue;
      out.push({ feed: f.feed, feedId: f.id, item: it });
      if (out.length >= max) break;
    }
  }
  return out;
}

// El id del paquete es "<feedId>-<itemId>": de ahí sale a qué medio pertenece
// una nota sin tener que arrastrar el feed por todos lados.
function feedKey(id: string): string {
  const dash = id.indexOf("-");
  return dash <= 0 ? id : id.slice(0, dash);
}

// Un paquete viejo (o un feed sin fechas) no tiene `whenAt`. Vale 0 y entonces
// manda el orden en que ya estaba, que es el del feed: lo más nuevo primero.
const publishedAt = (item: PackItem): number =>
  typeof item.whenAt === "number" && Number.isFinite(item.whenAt) && item.whenAt > 0 ? item.whenAt : 0;

// LA VENTANA RODANTE: entra lo nuevo y sale lo más viejo. Pura, se prueba sin
// red. Dos recortes, y hacen falta los dos:
//   1. por medio, dejando las PER_FEED más nuevas de cada diario;
//   2. si aun así no entra en el total, se suelta la más vieja DEL MEDIO QUE
//      MÁS TIENE. Recortar la cola de una lista ordenada por fecha a secas
//      dejaría sin una sola nota al diario que publica despacio (o al que
//      publica sin fecha en el feed, que vale 0 y quedaría siempre último).
// `Array.prototype.sort` es estable, así que entre dos fechas iguales —o entre
// dos ceros— gana el que ya venía primero.
export function rollingWindow(items: PackItem[], perFeed: number, total: number): PackItem[] {
  const medios = new Map<string, PackItem[]>();
  for (const item of items) {
    const key = feedKey(item.id);
    const g = medios.get(key);
    if (g) g.push(item);
    else medios.set(key, [item]);
  }
  for (const g of medios.values()) {
    g.sort((a, b) => publishedAt(b) - publishedAt(a));
    if (g.length > perFeed) g.length = perFeed;
  }
  let quedan = 0;
  for (const g of medios.values()) quedan += g.length;
  while (quedan > total) {
    let peor: PackItem[] | null = null;
    for (const g of medios.values()) {
      if (g.length === 0) continue;
      if (
        !peor ||
        g.length > peor.length ||
        (g.length === peor.length && publishedAt(g[g.length - 1]) < publishedAt(peor[peor.length - 1]))
      )
        peor = g;
    }
    if (!peor) break;
    peor.pop();
    quedan--;
  }
  // Y el manifiesto sale con lo más nuevo arriba: es el orden con el que el
  // aparato arma el fondo de pantalla (`newspack::headlines`). La pantalla de
  // Noticias reagrupa por medio sola, así que no le cambia nada.
  const out: PackItem[] = [];
  for (const g of medios.values()) out.push(...g);
  out.sort((a, b) => publishedAt(b) - publishedAt(a));
  return out;
}

// Arma el paquete de una cuenta. Devuelve cuántas notas quedaron.
export async function rebuild(accountId: number, lang: Lang = "es"): Promise<number> {
  const store = await loadStore(accountId);
  const feeds = store.feeds ?? [];
  // Sin fuentes cargadas no hay paquete. PubMed ya no es una excepción: es un
  // feed de la lista como cualquier otro, así que si no está, no se sale a la
  // red (y por eso las pruebas del paquete siguen siendo sin red).
  if (feeds.length === 0) {
    await mutateDoc(accountId, "news", shape, (pack) => {
      pack.at = new Date().toISOString();
      pack.items = [];
      pack.bodies = {};
      pack.failed = {};
    });
    return 0;
  }

  // El masticado gasta modelo SIN que nadie lo pida (corre solo cada hora), así
  // que respeta el mismo tope mensual que las rutas metered: pasado el tope el
  // paquete se arma igual, pero con el texto limpiado a mano, que es gratis.
  const sinCupo = await overQuota(accountId);

  const previo = await loadPack(accountId);
  const conocido = new Map(previo.items.map((i) => [i.id, i]));

  // La hora de cada nota va en la zona de la CUENTA (la del lugar del clima) y
  // no en la del servidor, que es UTC. Sin lugar cargado, HUB_TZ.
  const lugar = await readDoc<{ timezone?: unknown } | null>(accountId, "hub-settings", null);
  const tz = typeof lugar?.timezone === "string" && lugar.timezone ? lugar.timezone : DEFAULT_TZ;

  // Los titulares de todos los feeds, intercalados: uno de cada uno y después
  // la segunda vuelta, así un diario que publica mucho no se come el paquete.
  const porFeed: { feed: string; id: number; items: Awaited<ReturnType<typeof readFeed>>["items"] }[] = [];
  const unavailable = new Set<number>();

  // Qué ids son de PubMed. Se mira la URL una vez y se guarda: más abajo hace
  // falta por id, cuando ya no se tiene el feed a mano.
  const medicalIds = new Set(feeds.filter((f) => isMedicalFeed(f.url)).map((f) => f.id));

  for (const f of feeds) {
    try {
      // Única diferencia con un diario: de dónde salen los titulares. Después
      // entra al MISMO reparto, al mismo cupo por medio y a la misma ventana.
      const r = isMedicalFeed(f.url) ? await readMedicalFeed() : await readFeed(f.url);
      if (r.items.length) porFeed.push({ feed: f.name || r.title, id: f.id, items: r.items });
      else unavailable.add(f.id);
    } catch (err) {
      unavailable.add(f.id);
      console.error("news feed:", f.url.slice(0, 60), String(err).slice(0, 100));
    }
  }
  // Se miran MÁS candidatos de los que van a entrar: abajo, la nota de la que
  // no se pudo sacar cuerpo se descarta sin gastar un lugar del cupo, y hay que
  // tener de dónde reponerla. Sin esto, un diario cuyas notas no se dejan
  // extraer aportaba tres o cuatro y nadie lo compensaba.
  const orden = interleave(porFeed, CANDIDATES_PER_FEED * porFeed.length, CANDIDATES_PER_FEED);

  const items: PackItem[] = [];
  const bodies: Record<string, Body> = {};
  let chewedCount = 0;
  let medicalChewed = 0;   // los papers tienen presupuesto propio
  // Por medio: cuántas entraron, cuántas se bajaron del diario en esta pasada
  // (el costo) y cuántas se descartaron por no tener cuerpo (el diagnóstico:
  // "ese diario aporta poco" y "de ese diario no se puede sacar el texto" son
  // cosas distintas y se arreglan en lugares distintos).
  const aceptadas = new Map<number, number>();
  const bajadas = new Map<number, number>();
  const sinCuerpo = new Map<number, number>();
  const suma = (m: Map<number, number>, k: number) => m.set(k, (m.get(k) ?? 0) + 1);
  let bajadasTotal = 0;
  // Las notas de las que ya se intentó y no se pudo sacar cuerpo. Se olvidan
  // solas a los dos días, que es cuando ya no están ni en el feed.
  const ahora = Date.now();
  const fallidas: Record<string, number> = {};
  for (const [id, at] of Object.entries(previo.failed)) {
    if (typeof at === "number" && ahora - at < FAIL_FORGET_MS) fallidas[id] = at;
  }
  for (const { feed, feedId, item } of orden) {
    if ((aceptadas.get(feedId) ?? 0) >= PER_FEED) continue;
    const id = `${feedId}-${item.id}`;
    // Lo que ya estaba y no cambió de título no se vuelve a bajar ni a masticar:
    // eso es lo que hace que la pasada de cada hora sea barata.
    const antes = conocido.get(id);
    const medical = medicalIds.has(feedId);
    // Para un paper, `antes.title` es la TRADUCCIÓN: lo que hay que comparar
    // con el feed es el título de origen.
    const tituloPrevio = antes?.srcTitle ?? antes?.title;
    if (antes && tituloPrevio === item.title && previo.bodies[id] && (!medical || antes.medicalVersion === MEDICAL_SUMMARY_VERSION)) {
      // El `whenAt` completa los paquetes armados antes de la ventana rodante.
      items.push(antes.whenAt ? antes : { ...antes, whenAt: item.whenAt });
      bodies[id] = previo.bodies[id];
      suma(aceptadas, feedId);
      continue;
    }

    // Ya se intentó hace poco y el diario no dejó sacar el texto: no se vuelve
    // a gastar una bajada en ella hasta dentro de FAIL_RETRY_MS. Esto es lo que
    // hace que el cupo del medio se llene con las notas de más abajo en vez de
    // chocar cada hora contra las mismas cuatro rotas.
    if (fallidas[id] && ahora - fallidas[id] < FAIL_RETRY_MS) continue;

    // Ir al diario es lo único que cuesta (hasta ARTICLE_BUDGET_MS cada nota),
    // así que el tope se mide en BAJADAS y no en notas: los topes por medio y
    // por pasada son lo que evita que la pasada de cada hora se vuelva eterna
    // con muchos feeds o con un diario que cambió de formato. Una nota que trae
    // su texto en el propio feed no gasta nada y entra igual.
    if (item.link && ((bajadas.get(feedId) ?? 0) >= NEW_PER_FEED || bajadasTotal >= NEW_PER_RUN)) continue;

    let text = item.desc ?? "";
    if (item.link) {
      suma(bajadas, feedId);
      bajadasTotal++;
      try {
        const page = await download(item.link, "text/html,application/xhtml+xml,*/*;q=0.8", ARTICLE_BUDGET_MS);
        const a = extractArticle(page.body);
        if (a.text.length > text.length) text = a.text;
      } catch (err) {
        const why = err instanceof DownloadError ? err.reason : "down";
        console.error("news article:", item.link.slice(0, 60), why);
      }
    }
    // Sin cuerpo no entra (un titular suelto no es una nota), pero tampoco
    // gasta un lugar del cupo: se sigue con el candidato siguiente del mismo
    // medio.
    if (text.length < 200) {
      suma(sinCuerpo, feedId);
      fallidas[id] = ahora;
      continue;
    }

    // Los papers tienen su PROPIO presupuesto de modelo, aparte del de los
    // diarios. Una noticia sin masticar se lee igual (es el texto del diario,
    // en el idioma del aparato); un paper sin traducir llega EN INGLÉS, que es
    // justo lo que no se quiere. Y el gasto está topeado solo: PubMed aporta
    // como mucho NEWS_MEDICAL_ITEMS por pasada, y lo ya traducido no se vuelve
    // a traducir.
    const puedeMasticar = !sinCupo && (medical ? medicalChewed < MEDICAL_DIGEST_PER_RUN : chewedCount < DIGEST_PER_RUN);
    // El prefijo de la etiqueta ([NEJM · RCT]) no se manda al modelo y se
    // vuelve a poner acá: es una marca nuestra, no parte del título.
    const etiqueta = medical ? (/^\[[^\]]*\]\s*/.exec(item.title)?.[0] ?? "") : "";
    const plano = medical ? item.title.slice(etiqueta.length) : "";
    const { text: final, chewed, title: traducido } = puedeMasticar
      ? await chew(accountId, text, lang, medical, plano)
      : { text: text.slice(0, MAX_BODY), chewed: false, title: "" };
    if (chewed) { if (medical) medicalChewed++; else chewedCount++; }
    const titulo = traducido ? `${etiqueta}${traducido}`.slice(0, 500) : item.title;
    delete fallidas[id];
    const body: Body = { id, title: titulo, feed, when: whenLabel(item.whenAt, tz), text: final };
    bodies[id] = body;
    items.push({
      id,
      feed,
      title: titulo,
      when: whenLabel(item.whenAt, tz),
      sha: (await sha256Hex(final)).slice(0, 16),
      bytes: final.length,
      chewed,
      link: item.sourceLink ?? item.link ?? "",
      whenAt: item.whenAt,
      medicalVersion: medical ? MEDICAL_SUMMARY_VERSION : undefined,
      srcTitle: medical ? item.title : undefined,
    });
    suma(aceptadas, feedId);
  }

  // Si ningún feed produjo una nota utilizable, se consideran indisponibles
  // todos los configurados. Esto también cubre páginas que contestaron pero
  // sólo trajeron titulares o cuerpos demasiado cortos.
  if (items.length === 0) {
    for (const feed of feeds) unavailable.add(feed.id);
  }
  const seen = new Set(items.map((item) => item.id));
  for (const old of carryUnavailable(previo.items, previo.bodies, unavailable)) {
    if (seen.has(old.id)) continue;
    items.push(old);
    bodies[old.id] = previo.bodies[old.id];
    seen.add(old.id);
  }
  // La ventana rodante: hasta PER_FEED por medio y PACK_ITEMS en total, y lo
  // que se suelta es lo más VIEJO, no lo que quedó último en el arreglo.
  const ventana = rollingWindow(items, PER_FEED, PACK_ITEMS);
  const selectedIds = new Set(ventana.map((item) => item.id));
  for (const id of Object.keys(bodies)) {
    if (!selectedIds.has(id)) delete bodies[id];
  }

  // La lista de fallidas no puede crecer para siempre: se queda con las más
  // recientes, que son las que todavía están en el feed.
  const recortadas = Object.entries(fallidas).sort((a, b) => b[1] - a[1]).slice(0, FAIL_MAX);
  await mutateDoc(accountId, "news", shape, (pack) => {
    pack.at = new Date().toISOString();
    pack.items = ventana;
    pack.bodies = bodies;
    pack.failed = Object.fromEntries(recortadas);
  });
  // Una línea por medio, porque es lo único que contesta "¿por qué de ese
  // diario veo tres titulares?": puede ser que aporte pocos, que no se le pueda
  // sacar el cuerpo, o que se haya acabado el tope de bajadas de la pasada.
  const porMedio = porFeed
    .map((f) => {
      const enPaquete = ventana.filter((item) => feedKey(item.id) === String(f.id)).length;
      const extra = [
        (bajadas.get(f.id) ?? 0) ? `${bajadas.get(f.id)} bajadas` : "",
        (sinCuerpo.get(f.id) ?? 0) ? `${sinCuerpo.get(f.id)} sin cuerpo` : "",
      ].filter(Boolean).join(", ");
      return `${f.feed}: ${enPaquete}/${PER_FEED}${extra ? ` (${extra})` : ""}`;
    })
    .join(" · ");
  console.log(
    `news: cuenta ${accountId}, ${ventana.length} notas de ${porFeed.length} medios ` +
      `(${chewedCount} masticadas${medicalChewed ? `, ${medicalChewed} papers traducidos` : ""}${sinCupo ? ", sin cupo" : ""}${bajadasTotal >= NEW_PER_RUN ? ", tope de bajadas de la pasada" : ""})` +
      (porMedio ? ` — ${porMedio}` : ""),
  );
  return ventana.length;
}

// El temporizador del proceso. Sobrevive a un redeploy porque el paquete está
// en el volumen y la primera pasada al arrancar lo refresca si quedó viejo.
let timer: ReturnType<typeof setInterval> | null = null;
const running = new Map<number, Promise<number>>();

export function refreshPack(accountId: number, lang: Lang = "es"): Promise<number> {
  const active = running.get(accountId);
  if (active) return active;
  const job = rebuild(accountId, lang).finally(() => running.delete(accountId));
  running.set(accountId, job);
  return job;
}

export function startRefresher(): void {
  if (timer) return;
  const pasada = async () => {
    try {
      // Sin multiusuario hay una sola cuenta. Con multiusuario, las cuentas se
      // refrescan cuando su aparato pide el paquete (ver la ruta de abajo): un
      // barrido de todas cada hora gastaría modelo por gente que no lo usa.
      await refreshPack(DEFAULT_ACCOUNT);
    } catch (err) {
      console.error("news refresher:", String(err).slice(0, 200));
    }
  };
  // La primera al minuto de arrancar, para no pelear con el arranque.
  setTimeout(pasada, 60 * 1000);
  timer = setInterval(pasada, REFRESH_MS);
  console.log(`news: refresco cada ${Math.round(REFRESH_MS / 60000)} min`);
}

export const news = new Hono<AppEnv>();

// El manifiesto: todo lo que hay, sin cuerpos. Es lo que el aparato compara
// contra lo que ya tiene en la tarjeta.
news.get("/pack", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const pack = await loadPack(acc);
  const viejo = !pack.at || Date.now() - Date.parse(pack.at) > REFRESH_MS;
  // La preparación del paquete (hasta NEW_PER_RUN bajadas de 12 s) puede tardar
  // mucho más que el timeout HTTP del lector. Se dispara en segundo plano y se
  // devuelve inmediatamente el último paquete bueno; la siguiente
  // sincronización recogerá el nuevo.
  if (viejo || pack.items.length === 0)
    void refreshPack(acc, lang).catch((err) => console.error("news pack:", String(err).slice(0, 200)));
  return c.json({
    ok: true,
    at: pack.at,
    items: pack.items.map(({ id, feed, title, when, sha, bytes, chewed }) => ({ id, feed, title, when, sha, bytes, chewed })),
  });
});

news.get("/status", async (c) => {
  const acc = accountOf(c);
  const pack = await loadPack(acc);
  return c.json({
    ok: true,
    building: running.has(acc),
    at: pack.at,
    items: pack.items.length,
    chewed: pack.items.filter((item) => item.chewed).length,
  });
});

news.get("/preview", async (c) => {
  const pack = await loadPack(accountOf(c));
  return c.json({
    ok: true,
    at: pack.at,
    items: pack.items.map(({ id, feed, title, when, chewed, link }) => ({ id, feed, title, when, chewed, link })),
  });
});

news.post("/refresh", async (c) => {
  const acc = accountOf(c);
  const lang = normalizeLang(c.req.query("lang"));
  const pack = await loadPack(acc);
  void refreshPack(acc, lang).catch((err) => console.error("news refresh:", String(err).slice(0, 200)));
  return c.json({ ok: true, building: true, items: pack.items.length }, 202);
});

// Una nota, entera. El aparato baja las que le falten, de a una.
news.get("/item", async (c) => {
  const id = (c.req.query("id") ?? "").toString();
  const pack = await loadPack(accountOf(c));
  const body = pack.bodies[id];
  if (!body) return c.json({ ok: false, error: "not found" }, 404);
  return c.json({ ok: true, ...body });
});
