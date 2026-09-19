// El bucle que arma el paquete, entero y SIN RED: se mockean el lector de
// feeds, la bajada del artículo y el modelo, y los documentos van a archivos
// temporales. Las funciones puras se prueban en pack.test.ts; esto prueba la
// POLÍTICA, que es donde estaba el problema que reportó el dueño ("las
// noticias me manda sólo 3 o 4 de cada periódico").
//
// El caso que importa es el diario "Duro": sólo una de cada tres notas deja
// sacar el cuerpo. Antes cada descarte gastaba un lugar del cupo y nadie lo
// reponía, así que ese medio aportaba tres o cuatro titulares para siempre.
import { expect, mock, test } from "bun:test";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const dir = mkdtempSync(join(tmpdir(), "news-pack-"));
process.env.STORE_FILE = join(dir, "store.json");
process.env.NEWS_FILE = join(dir, "news.json");
process.env.HUB_SETTINGS_FILE = join(dir, "hub-settings.json");

const SRC = join(import.meta.dir, "../../server/src");
const T0 = Date.parse("2026-09-19T12:00:00Z");

// Cada vuelta de `nuevas` es un diario que publicó titulares nuevos: los viejos
// bajan en la lista del feed, que es como rueda la ventana de verdad.
let nuevas = 0;
const items = (pref: string, n: number) =>
  Array.from({ length: n }, (_, i) => {
    const k = nuevas + n - i;  // el más nuevo primero, como viene un feed
    return { id: k, title: `${pref} ${k}`, when: "", whenAt: T0 + k * 600_000, link: `https://${pref}.test/${k}`, desc: "" };
  });

let bajadas = 0;
await mock.module(`${SRC}/rss.ts`, () => ({
  DEFAULT_TZ: "UTC",
  whenLabel: (at: number) => new Date(at).toISOString().slice(11, 16),
  DownloadError: class extends Error { reason = "down"; },
  readFeed: async (url: string) => {
    if (url.includes("mucho")) return { title: "Mucho", items: items("mucho", 30) };
    if (url.includes("duro")) return { title: "Duro", items: items("duro", 30) };
    return { title: "Poco", items: items("poco", 2) };
  },
  download: async (link: string) => {
    bajadas++;
    if (link.includes("duro") && Number(link.split("/").pop()) % 3 !== 0) return { body: "<p>no</p>" };
    return { body: `<article>${"palabra ".repeat(120)}</article>` };
  },
  extractArticle: (html: string) => ({ text: html.includes("<p>no</p>") ? "" : "palabra ".repeat(120) }),
}));
await mock.module(`${SRC}/llm.ts`, () => ({ chatText: async () => "masticado ".repeat(40) }));

const { rebuild, loadPack } = await import(`${SRC}/news.ts`);
const { mutate } = await import(`${SRC}/store.ts`);

const porMedio = (items: { id: string }[]) => {
  const m: Record<string, number> = {};
  for (const it of items) m[it.id.split("-")[0]] = (m[it.id.split("-")[0]] ?? 0) + 1;
  return m;
};

test("cada medio llega a su cupo, incluso el que no deja sacar el texto", async () => {
  await mutate(1, (s) => {
    s.feeds = [
      { id: 1, name: "Mucho", url: "https://mucho.test/rss" },
      { id: 2, name: "Duro", url: "https://duro.test/rss" },
      { id: 3, name: "Poco", url: "https://poco.test/rss" },
    ];
  });
  for (let i = 0; i < 4; i++) await rebuild(1);
  const pack = await loadPack(1);
  const cuenta = porMedio(pack.items);
  expect(cuenta["1"]).toBe(12);                       // el tope por medio, no el total
  expect(cuenta["2"]).toBeGreaterThanOrEqual(10);     // el duro también llega: 10 de sus 30 tienen cuerpo
  expect(cuenta["3"]).toBe(2);                        // el chico aporta lo que tiene y no molesta
  expect(pack.items.every((i) => typeof i.whenAt === "number" && i.whenAt > 0)).toBe(true);
});

test("la nota rota no se vuelve a bajar en cada pasada", async () => {
  const antes = bajadas;
  await rebuild(1);                                   // todo conocido y lo roto anotado
  expect(bajadas - antes).toBe(0);
});

test("la ventana rueda: entra lo nuevo y sale lo más viejo", async () => {
  const antes = await loadPack(1);
  const viejo = antes.items.filter((i) => i.id.startsWith("1-")).at(-1)!;
  nuevas += 6;                                        // el diario publicó seis titulares nuevos
  await rebuild(1);
  const despues = await loadPack(1);
  expect(porMedio(despues.items)["1"]).toBe(12);      // sigue en el cupo, no crece
  expect(despues.items.some((i) => i.id === viejo.id)).toBe(false);
  expect(despues.items.filter((i) => i.id.startsWith("1-"))[0].whenAt!).toBeGreaterThan(viejo.whenAt!);
  // Y no quedan cuerpos huérfanos en el volumen de lo que salió de la ventana.
  expect(Object.keys(despues.bodies).sort()).toEqual(despues.items.map((i) => i.id).sort());
});

test("una caída de todos los diarios no vacía el paquete", async () => {
  const antes = await loadPack(1);
  await mock.module(`${SRC}/rss.ts`, () => ({
    DEFAULT_TZ: "UTC",
    whenLabel: () => "",
    DownloadError: class extends Error { reason = "down"; },
    readFeed: async () => { throw new Error("sin red"); },
    download: async () => { throw new Error("sin red"); },
    extractArticle: () => ({ text: "" }),
  }));
  await rebuild(1);
  const despues = await loadPack(1);
  expect(despues.items.length).toBe(antes.items.length);
});
