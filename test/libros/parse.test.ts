// Los textos son los de las dos capturas del 18-09-2026 (LIBROS_CONTRATO.md),
// tal cual los muestra Telegram: la línea que abre la lista, cada título con
// su comando, y la ficha con "Publicado: 1967 | 345 páginas".
import { describe, expect, test } from "bun:test";
import { fileNameFor, formatOfLabel, parseCard, parseList, slugify } from "../../server/src/librosParse";

const LISTA_PADRE = [
  "Ahí van algunos libros que coinciden con tu búsqueda:",
  "",
  "Padre Rico, Padre Pobre /bYsdE",
  "Padre rico, padre pobre para jóvenes /bdTaE",
  "Padre rico, padre pobre (nueva edición actualizada) /bIOaE",
].join("\n");

const LISTA_CIEN = [
  "Ahí van algunos libros que coinciden con tu búsqueda:",
  "",
  "Los cien pájaros /bl4jE",
  "Cien sonetos de amor /bQ2kE",
  "Cien años de perdón /bT9mE",
  "Las mil y una noches en cien /b7HnE",
  "Cien años de soledad /b0E_D",
].join("\n");

const FICHA = [
  "Cien años de soledad - Gabriel García Márquez",
  "Publicado: 1967 | 345 páginas",
  "Novela Drama",
  "«Muchos años después, frente al pelotón de fusilamiento, el coronel Aureliano Buendía había de recordar aquella tarde remota en que su padre lo llevó a conocer el hielo».",
  "Con estas palabras empieza la novela.",
].join("\n");

const BOTONES = ["Información", "Leer online", "Epub", "Reportar error"];

describe("parseList", () => {
  test("la lista de Padre rico: tres resultados con su comando, la cabecera se ignora", () => {
    expect(parseList(LISTA_PADRE)).toEqual([
      { title: "Padre Rico, Padre Pobre", code: "/bYsdE" },
      { title: "Padre rico, padre pobre para jóvenes", code: "/bdTaE" },
      { title: "Padre rico, padre pobre (nueva edición actualizada)", code: "/bIOaE" },
    ]);
  });

  test("la lista de Cien: el último es Cien años de soledad /b0E_D (con guion bajo)", () => {
    const r = parseList(LISTA_CIEN);
    expect(r.length).toBe(5);
    expect(r[0]).toEqual({ title: "Los cien pájaros", code: "/bl4jE" });
    expect(r[4]).toEqual({ title: "Cien años de soledad", code: "/b0E_D" });
  });

  test("«no encontré» no es una lista", () => {
    expect(parseList("No encontré ningún libro con ese nombre. Prueba con otro título o autor.")).toEqual([]);
    expect(parseList("")).toEqual([]);
  });

  test("un comando repetido cuenta una vez; una línea sin comando no cuenta", () => {
    expect(parseList("A /x1\nA /x1\nsin comando\n/solo\nB /x2")).toEqual([
      { title: "A", code: "/x1" },
      { title: "B", code: "/x2" },
    ]);
  });

  test("numeración o viñeta delante del título se quita", () => {
    expect(parseList("1. Uno /aa\n• Dos /bb\n- Tres /cc")).toEqual([
      { title: "Uno", code: "/aa" },
      { title: "Dos", code: "/bb" },
      { title: "Tres", code: "/cc" },
    ]);
  });
});

describe("parseCard", () => {
  test("la ficha de Cien años de soledad", () => {
    const c = parseCard(FICHA, BOTONES);
    expect(c.title).toBe("Cien años de soledad");
    expect(c.author).toBe("Gabriel García Márquez");
    expect(c.year).toBe(1967);
    expect(c.pages).toBe(345);
    expect(c.genre).toBe("Novela Drama");
    expect(c.desc.startsWith("«Muchos años después, frente al pelotón")).toBe(true);
    expect(c.desc.endsWith("Con estas palabras empieza la novela.")).toBe(true);
    expect(c.formats).toEqual(["epub"]);
  });

  test("sin autor, sin Publicado: todo lo que sigue es descripción", () => {
    const c = parseCard("Un título suelto\nUna línea de texto.\nOtra.", ["Información"]);
    expect(c.title).toBe("Un título suelto");
    expect(c.author).toBe("");
    expect(c.year).toBeNull();
    expect(c.pages).toBeNull();
    expect(c.genre).toBe("");
    expect(c.desc).toBe("Una línea de texto.\nOtra.");
    expect(c.formats).toEqual([]);
  });

  test("el título se parte por el ÚLTIMO ' - '", () => {
    const c = parseCard("Fahrenheit 451 - edición anotada - Ray Bradbury\nPublicado: 1953 | 190 pág.", ["PDF", "📕 EPUB", "Mobi"]);
    expect(c.title).toBe("Fahrenheit 451 - edición anotada");
    expect(c.author).toBe("Ray Bradbury");
    expect(c.year).toBe(1953);
    expect(c.pages).toBe(190);
    // epub primero, los demás en el orden del bot, en minúsculas
    expect(c.formats).toEqual(["epub", "pdf", "mobi"]);
  });

  test("la línea después de Publicado con números NO es género", () => {
    const c = parseCard("T - A\nPublicado: 2001\n2 tomos\nDescripción.", []);
    expect(c.genre).toBe("");
    expect(c.desc).toBe("2 tomos\nDescripción.");
  });

  test("la descripción se corta a 2 KB", () => {
    const c = parseCard("T - A\nPublicado: 2001\nNovela\n" + "x".repeat(5000), []);
    expect(c.desc.length).toBe(2048);
  });
});

describe("formatos y nombres", () => {
  test("formatOfLabel", () => {
    expect(formatOfLabel("Epub")).toBe("epub");
    expect(formatOfLabel("📕 EPUB")).toBe("epub");
    expect(formatOfLabel("Bajar en PDF")).toBe("pdf");
    expect(formatOfLabel("Información")).toBe("");
    expect(formatOfLabel("Leer online")).toBe("");
  });

  test("slugify", () => {
    expect(slugify("Cien años de soledad")).toBe("cien-anos-de-soledad");
    expect(slugify("  ¡¿Qué?!  ")).toBe("que");
    expect(slugify("")).toBe("libro");
    expect(slugify("a".repeat(80)).length).toBe(40);
  });

  test("fileNameFor: el del bot si es sano, si no el slug con la extensión", () => {
    expect(fileNameFor("garcia_marquez-cien_anos.epub", "Cien años de soledad", "epub")).toBe("garcia_marquez-cien_anos.epub");
    expect(fileNameFor("Cien años de soledad - García Márquez.epub", "Cien años de soledad", "epub")).toBe("cien-anos-de-soledad.epub");
    expect(fileNameFor(null, "Cien años de soledad", "pdf")).toBe("cien-anos-de-soledad.pdf");
    expect(fileNameFor("raro.EPUB", "Cien años de soledad", "pdf")).toBe("raro.EPUB");
    expect(fileNameFor("../x.epub", "T", "epub")).toBe("t.epub");
    // Lo que sale tiene que pasar el filtro de saveFile.
    for (const n of [fileNameFor("libro con espacios.mobi", "Ø", "epub"), fileNameFor("", "a".repeat(80), "epub")]) {
      expect(/^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$/.test(n)).toBe(true);
    }
  });
});
