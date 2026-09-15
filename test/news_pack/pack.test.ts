// El reparto de titulares entre feeds, sin red. Es la parte del paquete de
// noticias que decide QUÉ entra, y la que rompe de forma silenciosa: un diario
// que publica mucho se come el paquete y los otros no aparecen nunca.
import { expect, test } from "bun:test";

import { carryUnavailable, interleave } from "../../server/src/news";

const feed = (id: number, name: string, n: number) => ({
  feed: name,
  id,
  items: Array.from({ length: n }, (_, i) => `${name}-${i}`),
});

test("uno de cada feed antes de la segunda vuelta", () => {
  const out = interleave([feed(1, "A", 3), feed(2, "B", 3)], 6);
  expect(out.map((o) => o.item)).toEqual(["A-0", "B-0", "A-1", "B-1", "A-2", "B-2"]);
});

test("un feed que publica mucho no se come el paquete", () => {
  const out = interleave([feed(1, "Mucho", 50), feed(2, "Poco", 2)], 6);
  const porMedio = out.filter((o) => o.feed === "Poco").length;
  expect(porMedio).toBe(2);           // las dos que tiene entran
  expect(out[0].item).toBe("Mucho-0"); // y el grande no queda afuera
  expect(out[1].item).toBe("Poco-0");
});

test("respeta el tope", () => {
  expect(interleave([feed(1, "A", 40), feed(2, "B", 40)], 7)).toHaveLength(7);
});

test("con feeds vacíos no se cuelga ni inventa", () => {
  expect(interleave([feed(1, "A", 0), feed(2, "B", 0)], 10)).toEqual([]);
  expect(interleave([], 10)).toEqual([]);
});

test("un feed corto no corta a los demás", () => {
  const out = interleave([feed(1, "Corto", 1), feed(2, "Largo", 4)], 10);
  expect(out.map((o) => o.item)).toEqual(["Corto-0", "Largo-0", "Largo-1", "Largo-2", "Largo-3"]);
});

test("el feedId viaja con cada ítem", () => {
  const out = interleave([feed(7, "A", 1), feed(9, "B", 1)], 2);
  expect(out.map((o) => o.feedId)).toEqual([7, 9]);
});

test("una caída conserva las últimas noticias buenas de ese feed", () => {
  const previous = [
    { id: "7-1", feed: "A", title: "una", when: "", sha: "a", bytes: 10, chewed: true, link: "" },
    { id: "9-1", feed: "B", title: "dos", when: "", sha: "b", bytes: 10, chewed: false, link: "" },
  ];
  const bodies = {
    "7-1": { id: "7-1", title: "una", feed: "A", when: "", text: "texto" },
    "9-1": { id: "9-1", title: "dos", feed: "B", when: "", text: "texto" },
  };
  expect(carryUnavailable(previous, bodies, new Set([7])).map((item) => item.id)).toEqual(["7-1"]);
});

test("no conserva una entrada cuyo cuerpo ya no existe", () => {
  const previous = [
    { id: "7-1", feed: "A", title: "una", when: "", sha: "a", bytes: 10, chewed: true, link: "" },
  ];
  expect(carryUnavailable(previous, {}, new Set([7]))).toEqual([]);
});
