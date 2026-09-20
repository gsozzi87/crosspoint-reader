// REV-016: un POST que se reintenta no se aplica dos veces.
//
// Dos partes: la caché pura (topes, vencimiento, desalojo) y el escenario de
// verdad — el servidor aplica el POST, la respuesta se pierde, el aparato
// reintenta con el MISMO X-Request-Id y no tiene que quedar una nota doble.
import { expect, test } from "bun:test";

import { cacheable, MAX_BODY, ReplayCache, type Stored } from "../../server/src/idempotency";

const body = (n: number) => new Uint8Array(n);
const stored = (n: number, at = 0, status = 200): Stored => ({ status, type: "application/json", body: body(n), at });

test("la clave separa cuenta, ruta y request id", () => {
  const a = ReplayCache.key(1, "/api/notes", "abc");
  expect(ReplayCache.key(2, "/api/notes", "abc")).not.toBe(a);   // otra cuenta
  expect(ReplayCache.key(1, "/api/hub/edit", "abc")).not.toBe(a); // otra ruta
  expect(ReplayCache.key(1, "/api/notes", "abd")).not.toBe(a);    // otro id
  expect(ReplayCache.key(1, "/api/notes", "abc")).toBe(a);
});

test("se devuelve lo guardado y no se ejecuta de nuevo", () => {
  const c = new ReplayCache();
  const k = ReplayCache.key(1, "/api/notes", "r1");
  expect(c.get(k)).toBeNull();
  c.put(k, stored(10, Date.now()));
  expect(c.get(k)?.status).toBe(200);
});

test("lo viejo vence y deja de devolverse", () => {
  const c = new ReplayCache(1000, 10, 1_000_000);
  const k = ReplayCache.key(1, "/api/notes", "r1");
  c.put(k, stored(10, 0));
  expect(c.get(k, 500)).not.toBeNull();
  expect(c.get(k, 2000)).toBeNull();
  expect(c.size).toBe(0);
});

test("no crece sin límite: tope de entradas y de bytes", () => {
  const c = new ReplayCache(60_000, 3, 1_000_000);
  for (let i = 0; i < 10; i++) c.put(ReplayCache.key(1, "/api/notes", `r${i}`), stored(100, Date.now()));
  expect(c.size).toBe(3);
  // La primera sigue afuera y la última adentro: se tira lo más viejo.
  expect(c.get(ReplayCache.key(1, "/api/notes", "r0"))).toBeNull();
  expect(c.get(ReplayCache.key(1, "/api/notes", "r9"))).not.toBeNull();

  const porBytes = new ReplayCache(60_000, 1000, 1000);
  for (let i = 0; i < 10; i++) porBytes.put(ReplayCache.key(1, "/api/voice", `v${i}`), stored(400, Date.now()));
  expect(porBytes.totalBytes).toBeLessThanOrEqual(1000);
  expect(porBytes.size).toBeLessThanOrEqual(3);
});

test("una respuesta enorme no se guarda en vez de comerse la memoria", () => {
  const c = new ReplayCache();
  const k = ReplayCache.key(1, "/api/voice", "grande");
  expect(c.put(k, stored(MAX_BODY + 1, Date.now()))).toBe(false);
  expect(c.get(k)).toBeNull();
  expect(c.totalBytes).toBe(0);
});

test("un 5xx NO se guarda: el reintento existe justamente para eso", () => {
  // Guardarlo convertiría una caída de un segundo en diez minutos de la misma
  // caída para ese request id.
  expect(cacheable(500, 10)).toBe(false);
  expect(cacheable(502, 10)).toBe(false);
  expect(cacheable(200, 10)).toBe(true);
  expect(cacheable(201, 10)).toBe(true);
  // Un 4xx sí: es determinista y no aplica nada.
  expect(cacheable(400, 10)).toBe(true);
  expect(cacheable(429, 10)).toBe(true);
  expect(cacheable(200, MAX_BODY + 1)).toBe(false);
});

test("el mismo id en otra cuenta no devuelve la respuesta ajena", () => {
  // Es la parte de seguridad: dos aparatos pueden generar el mismo id.
  const c = new ReplayCache();
  c.put(ReplayCache.key(1, "/api/notes", "mismo"), stored(10, Date.now(), 200));
  expect(c.get(ReplayCache.key(2, "/api/notes", "mismo"))).toBeNull();
});
