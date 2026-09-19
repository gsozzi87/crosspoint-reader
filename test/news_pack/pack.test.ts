// El reparto de titulares entre feeds, sin red. Es la parte del paquete de
// noticias que decide QUÉ entra, y la que rompe de forma silenciosa: un diario
// que publica mucho se come el paquete y los otros no aparecen nunca.
import { expect, test } from "bun:test";

import { carryUnavailable, interleave, rollingWindow, splitTitledAnswer, type PackItem } from "../../server/src/news";

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

// El tope POR MEDIO. Es lo que el dueño ve: "de cada periódico me manda tres o
// cuatro y en la web veo no menos de diez".
test("el tope por medio corta a un diario que publica mucho, no a los otros", () => {
  const out = interleave([feed(1, "Mucho", 30), feed(2, "Poco", 2)], 100, 12);
  expect(out.filter((o) => o.feed === "Mucho")).toHaveLength(12);
  expect(out.filter((o) => o.feed === "Poco")).toHaveLength(2);
});

test("con todos los feeds cortos el tope por medio no inventa nada", () => {
  const out = interleave([feed(1, "A", 3), feed(2, "B", 4), feed(3, "C", 2)], 100, 12);
  expect(out).toHaveLength(9);
  expect(out.filter((o) => o.feed === "B")).toHaveLength(4);
});

test("un solo feed llega hasta su tope por medio", () => {
  expect(interleave([feed(1, "Solo", 50)], 100, 12)).toHaveLength(12);
  expect(interleave([feed(1, "Solo", 5)], 100, 12)).toHaveLength(5);
});

test("el tope total sigue mandando por encima del tope por medio", () => {
  expect(interleave([feed(1, "A", 30), feed(2, "B", 30)], 7, 12)).toHaveLength(7);
});

test("sin tope por medio se comporta como antes", () => {
  expect(interleave([feed(1, "A", 30), feed(2, "B", 1)], 10)).toHaveLength(10);
});

test("un tope por medio de cero no devuelve nada", () => {
  expect(interleave([feed(1, "A", 30)], 10, 0)).toEqual([]);
});

// La ventana rodante: entra lo nuevo, sale lo más viejo.
const nota = (id: string, whenAt: number): PackItem =>
  ({ id, feed: id.split("-")[0], title: id, when: "", sha: id, bytes: 500, chewed: false, link: "", whenAt });

test("se sueltan las más viejas de cada medio, no las últimas del arreglo", () => {
  const items = [
    nota("1-a", 100), nota("1-b", 500), nota("1-c", 300),
    nota("2-a", 400), nota("2-b", 200),
  ];
  const out = rollingWindow(items, 2, 10);
  expect(out.map((i) => i.id)).toEqual(["1-b", "2-a", "1-c", "2-b"]);  // lo más nuevo arriba
  expect(out.find((i) => i.id === "1-a")).toBeUndefined();             // la más vieja del medio 1
});

test("un titular nuevo entra y el más viejo se va", () => {
  const ayer = [nota("1-a", 100), nota("1-b", 200), nota("1-c", 300)];
  const hoy = rollingWindow([nota("1-d", 400), ...ayer], 3, 10);
  expect(hoy.map((i) => i.id)).toEqual(["1-d", "1-c", "1-b"]);
});

test("el tope total no deja a un medio sin una sola nota", () => {
  const items = [
    ...Array.from({ length: 12 }, (_, i) => nota(`1-${i}`, 9000 + i)),  // el que publica mucho y recién
    nota("2-a", 10), nota("2-b", 20),                                    // el que publica despacio
  ];
  const out = rollingWindow(items, 12, 10);
  expect(out).toHaveLength(10);
  expect(out.filter((i) => i.id.startsWith("2-")).length).toBeGreaterThan(0);
});

test("sin fechas manda el orden en el que venían (el del feed)", () => {
  const items = [nota("1-a", 0), nota("1-b", 0), nota("1-c", 0)];
  expect(rollingWindow(items, 2, 10).map((i) => i.id)).toEqual(["1-a", "1-b"]);
});

test("un paquete viejo sin whenAt no se pierde ni se cuelga", () => {
  const viejo = { id: "1-a", feed: "A", title: "una", when: "", sha: "a", bytes: 10, chewed: false, link: "" };
  const out = rollingWindow([viejo as PackItem, nota("1-b", 500)], 5, 10);
  expect(out.map((i) => i.id)).toEqual(["1-b", "1-a"]);
});

test("la ventana con lo que no entra vacía y con listas vacías", () => {
  expect(rollingWindow([], 12, 40)).toEqual([]);
  expect(rollingWindow([nota("1-a", 1)], 12, 0)).toEqual([]);
});

// La respuesta del modelo para un paper viene con el título traducido en la
// primera línea. El formato de una respuesta del modelo es lo primero que se
// rompe, y romperse acá NO puede costar la traducción del cuerpo.
test("separa el título traducido del cuerpo", () => {
  const out = splitTitledAnswer("TÍTULO: Ensayo aleatorizado de semaglutida\n\nMÉTODOS: 1961 adultos...");
  expect(out.title).toBe("Ensayo aleatorizado de semaglutida");
  expect(out.text).toBe("MÉTODOS: 1961 adultos...");
});

test("aguanta las formas en que el modelo se sale del formato", () => {
  // sin tilde, en minúsculas, con negrita de markdown y con comillas
  expect(splitTitledAnswer('**TITULO: "Estudio X"**\n\ncuerpo').title).toBe("Estudio X");
  expect(splitTitledAnswer("**TÍTULO:** Estudio W\n\ncuerpo").title).toBe("Estudio W");
  expect(splitTitledAnswer("TITLE: Estudio V\n\ncuerpo").title).toBe("Estudio V");
  expect(splitTitledAnswer("titulo:   Estudio Y  \n\ncuerpo").title).toBe("Estudio Y");
  // línea en blanco de más entre el título y el cuerpo
  expect(splitTitledAnswer("TÍTULO: Z\n\n\n   cuerpo largo").text).toBe("cuerpo largo");
});

test("sin la primera línea marcada, el cuerpo entero se conserva", () => {
  // Es lo que hace que un modelo que ignora el formato no rompa la nota: el
  // título se deja como estaba y la traducción no se pierde.
  const cuerpo = "Traducción sin título delante, con todas sus frases.";
  const out = splitTitledAnswer(cuerpo);
  expect(out.title).toBe("");
  expect(out.text).toBe(cuerpo);
  // Y un título SIN cuerpo no se lleva puesta la nota: el cuerpo queda vacío y
  // el llamador se queda con el texto de antes (hay un piso de 200 caracteres).
  expect(splitTitledAnswer("TÍTULO: solo el titulo")).toEqual({ title: "solo el titulo", text: "" });
});
