// REV-085: los títulos de los papers viajan en UNA llamada al modelo y vuelven
// numerados. Lo único que el modelo tiene que respetar es ese formato, así que
// el parser es lo que hay que probar — y lo que hay que probar de verdad es que
// un formato roto NO pierda la nota: sin traducción, el título se queda como
// estaba y la nota sigue ahí.
import { expect, test } from "bun:test";

import { parseNumberedList } from "../../server/src/news";

test("lista numerada normal", () => {
  const out = "1. Efecto de la metformina\n2. Ensayo aleatorizado de rivaroxabán\n3. Mortalidad a 30 días";
  expect(parseNumberedList(out, 3)).toEqual([
    "Efecto de la metformina",
    "Ensayo aleatorizado de rivaroxabán",
    "Mortalidad a 30 días",
  ]);
});

test("acepta parentesis, guion, asteriscos y comillas", () => {
  const out = '1) **Primero**\n2 - "Segundo"\n3. “Tercero”';
  expect(parseNumberedList(out, 3)).toEqual(["Primero", "Segundo", "Tercero"]);
});

test("una linea que falta deja el hueco vacio, no corre a las demas", () => {
  // Esto es lo que importa: si el modelo se saltea el 2, el 3 tiene que seguir
  // siendo el 3. Un parser por orden de aparición le pondría al paper 2 el
  // título del 3, que es peor que dejarlo en inglés.
  const out = "1. Uno\n3. Tres";
  expect(parseNumberedList(out, 3)).toEqual(["Uno", "", "Tres"]);
});

test("numeros fuera de rango se ignoran", () => {
  expect(parseNumberedList("0. Cero\n4. Cuatro\n1. Uno", 2)).toEqual(["Uno", ""]);
});

test("basura o vacio devuelve todo vacio y no tira", () => {
  expect(parseNumberedList("", 2)).toEqual(["", ""]);
  expect(parseNumberedList("No puedo traducir eso.", 2)).toEqual(["", ""]);
  expect(parseNumberedList("Claro, aquí están los títulos:", 3)).toEqual(["", "", ""]);
});

test("ninguno esperado, ninguno devuelto", () => {
  expect(parseNumberedList("1. Algo", 0)).toEqual([]);
});

test("el texto se recorta y no explota con lineas larguisimas", () => {
  const largo = "x".repeat(900);
  const res = parseNumberedList(`1. ${largo}`, 1);
  expect(res[0]!.length).toBe(500);
});

test("prefijo con salto de linea de Windows", () => {
  expect(parseNumberedList("1. Uno\r\n2. Dos", 2)).toEqual(["Uno", "Dos"]);
});
