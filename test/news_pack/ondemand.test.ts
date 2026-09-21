// REV-085: NOTICIAS A DEMANDA, el camino entero y sin red.
//
// El dueño lo pidió así: "cuando entremos a noticias, que se actualice, sino
// que muestre solo los titulares que nos aportan las RSSs". Lo que eso cambia,
// y lo que se prueba acá:
//
//   1. el repaso NO gasta modelo ni entra a los diarios: sólo titulares;
//   2. abrir una nota es lo que paga — ahí se baja el artículo y se mastica;
//   3. abierta una vez, queda guardada: la segunda es gratis;
//   4. un fallo al traer el CUERPO no saca el titular del paquete.
//
// El punto 4 es el que importa: antes, una nota de la que no se podía sacar
// texto se descartaba entera, y con los cuerpos a demanda esa regla habría
// vaciado la lista.
//
// OJO: archivo con su propio proceso (ver el comentario de medical_pack).
import { expect, mock, test } from "bun:test";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const dir = mkdtempSync(join(tmpdir(), "news-ondemand-"));
process.env.STORE_FILE = join(dir, "store.json");
process.env.NEWS_FILE = join(dir, "news.json");
process.env.HUB_SETTINGS_FILE = join(dir, "hub-settings.json");

const SRC = join(import.meta.dir, "../../server/src");
const T0 = Date.parse("2026-09-19T12:00:00Z");

// Cuántas veces se entró a un diario y cuántas se llamó al modelo. Son los dos
// números que este cambio existe para bajar.
let bajadas = 0;
let llamadas = 0;
let cuerpoDisponible = true;

await mock.module(`${SRC}/rss.ts`, () => ({
  DEFAULT_TZ: "UTC",
  whenLabel: (at: number) => new Date(at).toISOString().slice(11, 16),
  DownloadError: class extends Error { reason = "down"; },
  readFeed: async () => ({
    title: "Diario",
    items: [
      { id: 1, title: "Primera", when: "", whenAt: T0, link: "https://diario.test/1", desc: "resumen corto" },
      { id: 2, title: "Segunda", when: "", whenAt: T0 - 60_000, link: "https://diario.test/2", desc: "resumen corto" },
    ],
  }),
  download: async () => {
    bajadas++;
    return { body: cuerpoDisponible ? "<p>" + "texto del articulo ".repeat(40) + "</p>" : "" };
  },
  extractArticle: (html: string) => ({ text: html.replace(/<[^>]+>/g, "") }),
}));

await mock.module(`${SRC}/llm.ts`, () => ({
  chatText: async () => {
    llamadas++;
    // Largo a propósito: `chew()` sólo da por buena una respuesta de más de 200
    // caracteres, porque una respuesta corta suele ser el modelo diciendo que
    // no puede en vez del texto reescrito.
    return "cuerpo masticado para pantalla chica. ".repeat(10);
  },
}));

const { rebuild, loadPack, ensureBody } = await import(`${SRC}/news.ts`);
const { mutate } = await import(`${SRC}/store.ts`);

await mutate(1, (s: { feeds?: unknown[] }) => {
  s.feeds = [{ id: 1, name: "Diario", url: "https://diario.test/rss" }];
});

test("el repaso trae titulares y NO gasta ni una bajada ni una llamada", async () => {
  await rebuild(1, "es");
  const pack = await loadPack(1);
  expect(pack.items.length).toBe(2);
  expect(pack.items.every((i: { pending?: boolean }) => i.pending === true)).toBe(true);
  expect(Object.keys(pack.bodies).length).toBe(0);
  expect(bajadas).toBe(0);
  expect(llamadas).toBe(0);
});

test("abrir la nota es lo que paga, y deja el cuerpo guardado", async () => {
  const body = await ensureBody(1, "1-1", "es");
  expect(body?.text.startsWith("cuerpo masticado")).toBe(true);
  expect(bajadas).toBe(1);
  expect(llamadas).toBe(1);

  const pack = await loadPack(1);
  const item = pack.items.find((i: { id: string }) => i.id === "1-1")!;
  expect(item.pending).toBeUndefined();
  expect(item.chewed).toBe(true);
  expect(item.sha.length).toBeGreaterThan(0);
  expect(item.bytes).toBeGreaterThan(0);
  // La otra sigue pendiente: sólo se paga lo que se abre.
  expect(pack.items.find((i: { id: string }) => i.id === "1-2")!.pending).toBe(true);
});

test("la segunda visita es gratis", async () => {
  await ensureBody(1, "1-1", "es");
  expect(bajadas).toBe(1);
  expect(llamadas).toBe(1);
});

test("un repaso posterior no vuelve a pagar lo ya abierto ni pierde lo pendiente", async () => {
  await rebuild(1, "es");
  const pack = await loadPack(1);
  expect(pack.items.length).toBe(2);
  expect(pack.bodies["1-1"].text.startsWith("cuerpo masticado")).toBe(true);
  expect(pack.items.find((i: { id: string }) => i.id === "1-2")!.pending).toBe(true);
  expect(bajadas).toBe(1);
  expect(llamadas).toBe(1);
});

test("si el diario no deja sacar el texto, el TITULAR se queda", async () => {
  cuerpoDisponible = false;
  const body = await ensureBody(1, "1-2", "es");
  expect(body).toBe(null);
  const pack = await loadPack(1);
  // La nota sigue en la lista — lo que falló es el cuerpo, no la nota.
  expect(pack.items.some((i: { id: string }) => i.id === "1-2")).toBe(true);
  expect(pack.failed["1-2"]).toBeGreaterThan(0);
  // Y no se llamó al modelo por algo que no tenía texto.
  expect(llamadas).toBe(1);
});

test("y el repaso siguiente TAMPOCO lo saca", async () => {
  await rebuild(1, "es");
  const pack = await loadPack(1);
  expect(pack.items.some((i: { id: string }) => i.id === "1-2")).toBe(true);
});
