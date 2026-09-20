// REV-044: un replay no vuelve a cobrar consumo.
//
// El defecto no estaba en ninguno de los dos middlewares sino en el ORDEN entre
// ellos, y eso no se ve leyendo cada uno por separado: el de medición envuelve
// al de idempotencia, hace `await next()` y al volver sólo ve el ESTADO de la
// respuesta. Un reintento que la caché resuelve sin ejecutar STT, modelo ni TTS
// le parecía un 2xx normal y volvía a sumar uso. Un usuario con tope mensual lo
// gastaba más rápido sólo porque la red lo obligó a reintentar.
//
// Por eso la prueba COMPONE los dos middlewares de verdad en vez de mirarlos
// sueltos, con un espía sobre addUsage.
import { beforeEach, describe, expect, mock, test } from "bun:test";
// Hono por ruta relativa: este archivo vive en test/ y las dependencias del
// servidor están en server/node_modules. `dist/index.js` es la entrada ESM que
// declara el propio package.json de hono (exports["."].import).
import { Hono } from "../../server/node_modules/hono/dist/index.js";
import { join } from "node:path";

const SRC = join(import.meta.dir, "../../server/src");

// El contador de consumo, espiado. `overQuota` en false: acá se mide el cobro,
// no el tope.
const cobros: { llm?: number; sttSeconds?: number }[] = [];
await mock.module(`${SRC}/usage.ts`, () => ({
  addUsage: async (_acc: number, add: { llm?: number; sttSeconds?: number }) => { cobros.push(add); },
  overQuota: async () => false,
  audioSeconds: (bytes: number) => Math.round(bytes / 32000),
  QUOTA_CODE: "quota",
  QUOTA_MSG: { es: "sin cupo", en: "", fr: "", de: "", pt: "", ru: "" },
  quotasOn: false,
}));
await mock.module(`${SRC}/tenant.ts`, () => ({ accountOf: () => 1 }));

const { idempotency, ReplayCache } = await import(`${SRC}/idempotency.ts`);
const { metering, shouldCharge } = await import(`${SRC}/metering.ts`);

// El mismo orden que api.ts: medición por AFUERA, idempotencia por adentro.
function servidor() {
  const app = new Hono();
  const cache = new ReplayCache();
  app.use("*", metering());
  app.use("*", idempotency(cache, () => 1));
  let veces = 0;
  app.post("/api/voice", (c) => {
    veces++;  // representa el STT + modelo + TTS, que es lo que se cobra
    return c.json({ ok: true, text: "respuesta", ejecuciones: veces });
  });
  return { app, ejecutado: () => veces };
}

const post = (app: Hono, id?: string) =>
  app.request("/api/voice", {
    method: "POST",
    headers: { "content-type": "audio/adpcm", ...(id ? { "x-request-id": id } : {}) },
    body: new Uint8Array(3200),
  });

beforeEach(() => { cobros.length = 0; });

describe("el replay no se cobra dos veces", () => {
  test("el primer pedido sí se cobra", async () => {
    const { app, ejecutado } = servidor();
    const r = await post(app, "id-1");
    expect(r.status).toBe(200);
    expect(ejecutado()).toBe(1);
    expect(cobros.length).toBe(1);
  });

  test("el REINTENTO con el mismo id no ejecuta nada y no cobra nada", async () => {
    const { app, ejecutado } = servidor();
    await post(app, "id-1");
    expect(cobros.length).toBe(1);

    const r2 = await post(app, "id-1");
    expect(r2.status).toBe(200);
    // Misma respuesta que la primera vez, sin volver a pasar por el handler.
    expect(await r2.json()).toEqual({ ok: true, text: "respuesta", ejecuciones: 1 });
    expect(ejecutado()).toBe(1);
    // Y ACÁ estaba el defecto: antes esto era 2.
    expect(cobros.length).toBe(1);
  });

  test("un pedido de verdad con OTRO id sí se cobra", async () => {
    const { app, ejecutado } = servidor();
    await post(app, "id-1");
    await post(app, "id-2");
    expect(ejecutado()).toBe(2);
    expect(cobros.length).toBe(2);
  });

  test("sin X-Request-Id (la web) cada pedido se cobra, como siempre", async () => {
    const { app, ejecutado } = servidor();
    await post(app);
    await post(app);
    expect(ejecutado()).toBe(2);
    expect(cobros.length).toBe(2);
  });

  test("una ruta que no está en METERED no cobra nunca", async () => {
    const app = new Hono();
    const cache = new ReplayCache();
    app.use("*", metering());
    app.use("*", idempotency(cache, () => 1));
    app.post("/api/notes", (c) => c.json({ ok: true, id: 1 }));
    await app.request("/api/notes", {
      method: "POST",
      headers: { "content-type": "application/json", "x-request-id": "n-1" },
      body: "{}",
    });
    expect(cobros.length).toBe(0);
  });
});

describe("shouldCharge", () => {
  test("las dos reglas juntas", () => {
    expect(shouldCharge(200, false)).toBe(true);
    expect(shouldCharge(200, true)).toBe(false);   // replay: no ejecutó nada
    expect(shouldCharge(500, false)).toBe(false);  // un 502 del proveedor no es de nadie
    expect(shouldCharge(429, false)).toBe(false);
    expect(shouldCharge(500, true)).toBe(false);
  });
});
