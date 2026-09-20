// REV-047: el anillo de fallos del proveedor.
//
// Es diagnóstico, así que lo que importa es que NO pueda hacerse daño: no
// crece sin límite, no guarda un cuerpo de error enorme y el más nuevo se lee
// primero (que es como mira el dueño la pantalla).
import { expect, test, beforeEach } from "bun:test";
import {
  clearProviderFailures,
  MAX_FAILURES,
  providerFailures,
  recordProviderFailure,
} from "../../server/src/providerLog";

beforeEach(() => clearProviderFailures());

test("vacío al principio", () => {
  expect(providerFailures()).toEqual([]);
});

test("el más nuevo primero", () => {
  recordProviderFailure("llm", "chatSearch", new Error("viejo"));
  recordProviderFailure("stt", "whisper-1", new Error("nuevo"));
  const f = providerFailures();
  expect(f.length).toBe(2);
  expect(f[0]!.message).toBe("nuevo");
  expect(f[0]!.kind).toBe("stt");
  expect(f[0]!.where).toBe("whisper-1");
  expect(f[1]!.message).toBe("viejo");
});

test("no crece sin límite: se quedan los últimos", () => {
  for (let i = 0; i < MAX_FAILURES * 3; i++) recordProviderFailure("llm", "chatSearch", new Error(`e${i}`));
  const f = providerFailures();
  expect(f.length).toBe(MAX_FAILURES);
  // El primero de la lista es el último que entró.
  expect(f[0]!.message).toBe(`e${MAX_FAILURES * 3 - 1}`);
  // Y el más viejo que queda es el que corresponde, no uno del principio.
  expect(f[f.length - 1]!.message).toBe(`e${MAX_FAILURES * 2}`);
});

test("un cuerpo de error enorme se recorta", () => {
  recordProviderFailure("llm", "chatSearch", new Error("x".repeat(5000)));
  const f = providerFailures();
  expect(f[0]!.message.length).toBeLessThanOrEqual(300);
});

test("lo que no es Error también se anota", () => {
  recordProviderFailure("llm", "chatJson", "cayó el proveedor");
  expect(providerFailures()[0]!.message).toBe("cayó el proveedor");
});

test("cada entrada lleva su hora", () => {
  const t0 = Date.now();
  recordProviderFailure("llm", "chatSearch", new Error("x"));
  const at = providerFailures()[0]!.at;
  expect(at).toBeGreaterThanOrEqual(t0);
  expect(at).toBeLessThanOrEqual(Date.now());
});
