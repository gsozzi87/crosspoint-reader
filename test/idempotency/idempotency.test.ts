// REV-016: un POST que se reintenta no se aplica dos veces.
//
// Dos partes: la caché pura (topes, vencimiento, desalojo) y el escenario de
// verdad — el servidor aplica el POST, la respuesta se pierde, el aparato
// reintenta con el MISMO X-Request-Id y no tiene que quedar una nota doble.
import { expect, test } from "bun:test";

import { Hono } from "../../server/node_modules/hono/dist/index.js";

import { cacheable, idempotency, MAX_BODY, ReplayCache, type Stored } from "../../server/src/idempotency";

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

// ── No se copia lo que no se va a guardar ───────────────────────────────────
//
// `clone()` deja el cuerpo DOS veces en memoria hasta que el otro lado lo lee,
// y por este middleware pasan los cientos de KB de ADPCM de /api/voice en un
// contenedor de 512 MB compartido con Piper. Copiar para después descartar por
// tamaño o por ser un 5xx es pagar el pico sin ninguna razón.
test("una respuesta que declara ser enorme no se copia ni se guarda", async () => {
  const app = new Hono();
  const cache = new ReplayCache();
  let clones = 0;
  app.use("*", idempotency(cache, () => 1));
  // Es la forma exacta de /api/voice y /api/translate: un cuerpo en memoria con
  // su Content-Length declarado a mano.
  app.post("/api/voice", () => {
    const cuerpo = new Uint8Array(MAX_BODY + 1);
    const res = new Response(cuerpo, {
      headers: { "content-type": "application/x-ws397-voice", "content-length": String(cuerpo.byteLength) },
    });
    const real = res.clone.bind(res);
    res.clone = () => { clones++; return real(); };
    return res;
  });
  const r = await app.request("/api/voice", { method: "POST", headers: { "x-request-id": "grande" } });
  expect(r.status).toBe(200);
  // El aparato recibe la respuesta entera igual.
  expect((await r.arrayBuffer()).byteLength).toBe(MAX_BODY + 1);
  // Y no se pagó la copia ni quedó guardada.
  expect(clones).toBe(0);
  expect(cache.size).toBe(0);
});

test("un 5xx no se guarda: el reintento tiene que volver a intentar", async () => {
  const app = new Hono();
  const cache = new ReplayCache();
  let clones = 0;
  app.use("*", idempotency(cache, () => 1));
  app.post("/api/ask", (c) => {
    const res = c.json({ ok: false, error: "el proveedor se cayó" }, 502);
    const real = res.clone.bind(res);
    res.clone = () => { clones++; return real(); };
    return res;
  });
  expect((await app.request("/api/ask", { method: "POST", headers: { "x-request-id": "cinco" } })).status).toBe(502);
  expect(clones).toBe(0);
  expect(cache.size).toBe(0);
});

test("una respuesta que sí entra se guarda y se repite", async () => {
  const app = new Hono();
  const cache = new ReplayCache();
  let veces = 0;
  app.use("*", idempotency(cache, () => 1));
  app.post("/api/voice", (c) => {
    veces++;
    return new Response(new Uint8Array(300_000), { headers: { "content-type": "application/x-ws397-voice" } });
  });
  const h = { "x-request-id": "justa" };
  const a = await app.request("/api/voice", { method: "POST", headers: h });
  const b = await app.request("/api/voice", { method: "POST", headers: h });
  expect(veces).toBe(1);
  expect((await a.arrayBuffer()).byteLength).toBe(300_000);
  expect((await b.arrayBuffer()).byteLength).toBe(300_000);
});
