// Los textos son los de las dos capturas del 18-09-2026 (LIBROS_CONTRATO.md),
// tal cual los muestra Telegram: la línea que abre la lista, cada título con
// su comando, y la ficha con "Publicado: 1967 | 345 páginas".
import { describe, expect, test } from "bun:test";
import { classifyReply, fileNameFor, formatOfLabel, lettersToWord, looksLikeCard, navOfLabel, pageOfLabel, parseCard, parseList, slugify } from "../../server/src/librosParse";

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

// La captura del 19-09-2026: el dueño buscó "Ángeles Mastretta" y entre los
// resultados vino el AUTOR, con cuántos libros tiene entre corchetes.
const BUSQUEDA_AUTOR = [
  "Ahí van algunos libros que coinciden con tu búsqueda:",
  "",
  "Arráncame la vida /b9F1E",
  "Ángeles Mastretta [11] /aMst1",
  "Mujeres de ojos grandes /b9N6E",
].join("\n");

// Y esto es lo que contestó el bot al comando del autor: OTRA LISTA (su
// catálogo), no una ficha. Hasta 1.5.114 esto entraba entero en la
// descripción del libro. Viene paginada: los botones son flechas.
const LISTA_AUTOR = [
  "Aquí van los libros que he encontrado para Ángeles Mastretta:",
  "",
  "Arráncame la vida /b9F1E",
  "El cielo de los leones /bzL2E",
  "El mundo iluminado /bBL2E",
  "Mal de amores /bh51y2",
  "Mujeres de ojos grandes /b9N6E",
  "Puerto libre /bDL2E",
  "La emoción de las cosas /bXa2E",
  "Maridos /bWa2E",
  "El viento de las horas /bAL2E",
  "Ninguna eternidad como la mía /bCL2E",
].join("\n");

const FLECHAS = ["⏮", "◀", "📅", "▶", "⏭"];

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

describe("el autor entre los resultados", () => {
  test("«Ángeles Mastretta [11]» es el autor: el número sale aparte y no se queda pegado al título", () => {
    const r = parseList(BUSQUEDA_AUTOR);
    expect(r.length).toBe(3);
    expect(r[1]).toEqual({ title: "Ángeles Mastretta", code: "/aMst1", count: 11 });
    // Los libros de al lado siguen sin `count`.
    expect(r[0]).toEqual({ title: "Arráncame la vida", code: "/b9F1E" });
  });

  test("un paréntesis con el año NO es una cuenta de libros", () => {
    expect(parseList("Cien años de soledad (1967) /b0E_D")).toEqual([
      { title: "Cien años de soledad (1967)", code: "/b0E_D" },
    ]);
    // Corchetes vacíos, con cero o sin título delante: se dejan como están.
    expect(parseList("[11] /aX")).toEqual([{ title: "[11]", code: "/aX" }]);
    expect(parseList("Tomo [0] /aY")).toEqual([{ title: "Tomo [0]", code: "/aY" }]);
    expect(parseList("Algo [] /aZ")).toEqual([{ title: "Algo []", code: "/aZ" }]);
  });

  test("el catálogo del autor: diez títulos con su comando", () => {
    const r = parseList(LISTA_AUTOR);
    expect(r.length).toBe(10);
    expect(r[0]).toEqual({ title: "Arráncame la vida", code: "/b9F1E" });
    expect(r[3]).toEqual({ title: "Mal de amores", code: "/bh51y2" });
    expect(r[9]).toEqual({ title: "Ninguna eternidad como la mía", code: "/bCL2E" });
  });
});

describe("classifyReply: una lista no es una ficha", () => {
  test("el catálogo del autor con flechas es una LISTA", () => {
    expect(classifyReply(LISTA_AUTOR, FLECHAS)).toBe("list");
    expect(classifyReply(LISTA_AUTOR, [])).toBe("list");
  });

  test("la ficha de verdad sigue siendo una ficha", () => {
    expect(classifyReply(FICHA, BOTONES)).toBe("card");
    // Sin botones tampoco se confunde: tiene año y páginas.
    expect(classifyReply(FICHA, [])).toBe("card");
    expect(looksLikeCard(FICHA)).toBe(true);
    expect(looksLikeCard(LISTA_AUTOR)).toBe(false);
  });

  test("el botón de formato manda: si ofrece el archivo, es una ficha", () => {
    expect(classifyReply(LISTA_AUTOR, ["Información", "📕 EPUB"])).toBe("card");
  });

  test("una ficha cuyo texto nombra comandos, pero trae año o páginas, sigue siendo ficha", () => {
    const t = "Obras completas - Autor\nPublicado: 1990 | 900 páginas\nTomo I /bAa1\nTomo II /bAa2";
    expect(classifyReply(t, [])).toBe("card");
  });

  test("una sola entrada se lee como ficha: no se puede distinguir sin adivinar", () => {
    expect(classifyReply("Único libro /bZz1", [])).toBe("card");
  });
});

describe("las flechas de la lista paginada", () => {
  test("las de la captura: ⏮ ◀ 📅 ▶ ⏭", () => {
    expect(FLECHAS.map(navOfLabel)).toEqual(["first", "prev", "", "next", "last"]);
  });

  test("variantes de texto y de símbolo", () => {
    expect(navOfLabel("Siguiente")).toBe("next");
    expect(navOfLabel("»")).toBe("last");
    expect(navOfLabel("«")).toBe("first");
    expect(navOfLabel(">>")).toBe("last");
    expect(navOfLabel("<")).toBe("prev");
    expect(navOfLabel(">")).toBe("next");
    expect(navOfLabel("Página anterior")).toBe("prev");
    expect(navOfLabel("Next page")).toBe("next");
    expect(navOfLabel("Prev")).toBe("prev");
    expect(navOfLabel("Última")).toBe("last");
    expect(navOfLabel("Ver más")).toBe("next");
  });

  test("un botón de FORMATO nunca es una flecha, y una etiqueta larga tampoco", () => {
    for (const l of ["Epub", "📕 EPUB", "Bajar en PDF", "PDF ▶"]) expect(navOfLabel(l)).toBe("");
    // "Más información" lleva "más" adentro y NO es la página siguiente.
    expect(navOfLabel("Más información")).toBe("");
    expect(navOfLabel("Información")).toBe("");
    expect(navOfLabel("Leer online")).toBe("");
    expect(navOfLabel("Reportar error")).toBe("");
    expect(navOfLabel("")).toBe("");
  });

  test("el número de página, del botón del medio o de una línea del texto", () => {
    expect(pageOfLabel("2/5")).toEqual({ at: 2, of: 5 });
    expect(pageOfLabel("Página 2 de 5")).toEqual({ at: 2, of: 5 });
    expect(pageOfLabel("1 / 3")).toEqual({ at: 1, of: 3 });
    expect(pageOfLabel("Page 3 of 7")).toEqual({ at: 3, of: 7 });
    expect(pageOfLabel("📅")).toBeNull();
    expect(pageOfLabel("Epub")).toBeNull();
    expect(pageOfLabel("Arráncame la vida /b9F1E")).toBeNull();
    expect(pageOfLabel("9 de 3")).toBeNull();   // no existe
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

describe("lettersToWord (deletreo)", () => {
  test("Ángeles Mastretta deletreado en español, con comas y «espacio»", () => {
    expect(lettersToWord("a, ene, ge, e, ele, e, ese, espacio, eme, a, ese, te, erre, e, te, te, a", "es"))
      .toBe("angeles mastretta");
  });

  test("letras sueltas con guiones o espacios, como las escribe el transcriptor", () => {
    expect(lettersToWord("a-n-g-e-l-e-s", "es")).toBe("angeles");
    expect(lettersToWord("A N G E L E S", "es")).toBe("angeles");
    expect(lettersToWord("A. N. G.", "es")).toBe("ang");
  });

  test("eñe no se confunde con ene; hache, jota, equis, i griega, doble uve", () => {
    expect(lettersToWord("a, eñe, o", "es")).toBe("año");
    expect(lettersToWord("hache, jota, equis, i griega, doble uve, zeta, ye", "es")).toBe("hjxywzy");
    expect(lettersToWord("uve doble, be larga, ve corta", "es")).toBe("wbv");
  });

  test("en inglés: Hemingway letra por letra", () => {
    expect(lettersToWord("aitch, e, em, i, en, gee, double you, ay, why", "en")).toBe("hemingway");
    expect(lettersToWord("bee, oh, oh, kay, space, tee, e, e, dee", "en")).toBe("book teed");
    expect(lettersToWord("zed, zee, ex, cue", "en")).toBe("zzxq");
  });

  test("una palabra entera en el medio queda entera; números y guion pasan", () => {
    expect(lettersToWord("cien, espacio, a, eñe, o, ese", "es")).toBe("cien años");
    expect(lettersToWord("uno, espacio, nueve, ocho, cuatro", "es")).toBe("uno nueve ocho cuatro");
    expect(lettersToWord("1, 9, 8, 4", "es")).toBe("1984");
    expect(lettersToWord("ce, o, guion, ce, o", "es")).toBe("co-co");
    expect(lettersToWord("mastretta, espacio, a mayúscula, ene", "es")).toBe("mastretta an");
  });

  test("vacío y sin nada útil", () => {
    expect(lettersToWord("", "es")).toBe("");
    expect(lettersToWord("  ,  ,  ", "es")).toBe("");
    expect(lettersToWord("mayúscula", "es")).toBe("");
  });

  test("qué tabla manda: «de» es d en español y «el» es l en inglés, pero los dos valen en las dos", () => {
    expect(lettersToWord("de, el", "es")).toBe("dl");
    expect(lettersToWord("dee, el, de", "en")).toBe("dld");
  });
});
