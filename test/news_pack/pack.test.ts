// El reparto de titulares entre feeds, sin red. Es la parte del paquete de
// noticias que decide QUÉ entra, y la que rompe de forma silenciosa: un diario
// que publica mucho se come el paquete y los otros no aparecen nunca.
import { expect, test } from "bun:test";

import { interleave } from "../../server/src/news";

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
