// REV-016: el recorrido completo de una operación que se aplicó, perdió la
// respuesta, se encoló y se reprodujo más tarde.
//
// El hueco que encontró el revisor: `postOrQueue()` intenta en línea con un id,
// y si termina encolando, `enqueue()` generaba OTRO id. Entonces la secuencia
//
//   1. el aparato manda POST /api/notes  (id X)
//   2. el servidor lo aplica y escribe el store
//   3. la respuesta se pierde entera (conexión cortada, 5xx con el trabajo hecho)
//   4. el aparato lo manda a la cola offline
//   5. más tarde la cola se vacía  (id Y, nuevo)
//
// dejaba la nota DUPLICADA, y desde el aparato no había forma de saberlo.
//
// Acá se recorre eso contra los middlewares de verdad, con las dos variantes:
// el id que se conserva (el arreglo) y el id nuevo (como estaba).
import { describe, expect, mock, test } from "bun:test";
import { Hono } from "../../server/node_modules/hono/dist/index.js";
import { join } from "node:path";

const SRC = join(import.meta.dir, "../../server/src");

await mock.module(`${SRC}/usage.ts`, () => ({
  addUsage: async () => {},
  overQuota: async () => false,
  audioSeconds: () => 0,
  QUOTA_CODE: "quota",
  QUOTA_MSG: { es: "", en: "", fr: "", de: "", pt: "", ru: "" },
  quotasOn: false,
}));
await mock.module(`${SRC}/tenant.ts`, () => ({ accountOf: () => 1 }));

const { idempotency, ReplayCache } = await import(`${SRC}/idempotency.ts`);
const { metering } = await import(`${SRC}/metering.ts`);

// Un servidor con el store de notas de mentira: lo único que importa es cuántas
// veces se aplicó la operación.
function servidor() {
  const app = new Hono();
  app.use("*", metering());
  app.use("*", idempotency(new ReplayCache(), () => 1));
  const notas: string[] = [];
  app.post("/api/notes", async (c) => {
    const body = (await c.req.json()) as { text: string };
    notas.push(body.text);
    return c.json({ ok: true, id: notas.length });
  });
  return { app, notas };
}

// Un POST del aparato, con su X-Request-Id.
const enviar = (app: Hono, id: string, text: string) =>
  app.request("/api/notes", {
    method: "POST",
    headers: { "content-type": "application/json", "x-request-id": id },
    body: JSON.stringify({ text }),
  });

describe("se aplicó, se perdió la respuesta, se encoló y se vació", () => {
  test("con el MISMO id (el arreglo): la nota queda UNA vez", async () => {
    const { app, notas } = servidor();

    // 1-2. El aparato manda y el servidor aplica.
    const primera = await enviar(app, "op-42", "comprar pan");
    expect(primera.status).toBe(200);
    expect(notas).toEqual(["comprar pan"]);

    // 3. La respuesta se pierde: el aparato nunca la ve. Para el servidor esto
    //    es invisible, por eso no hay nada que simular de este lado.
    // 4-5. El aparato encola la operación CON SU ID y más tarde la reproduce.
    const replay = await enviar(app, "op-42", "comprar pan");
    expect(replay.status).toBe(200);
    // Le vuelve la misma respuesta que se había perdido, con el mismo id de nota.
    expect(await replay.json()).toEqual({ ok: true, id: 1 });
    expect(notas).toEqual(["comprar pan"]);
  });

  test("con un id NUEVO (como estaba antes): la nota queda DUPLICADA", async () => {
    // Esto es el defecto, escrito como prueba: mientras `enqueue()` generara su
    // propio id, el servidor no tenía cómo reconocer el replay.
    const { app, notas } = servidor();
    await enviar(app, "op-42", "comprar pan");
    await enviar(app, "otro-id-nuevo", "comprar pan");
    expect(notas).toEqual(["comprar pan", "comprar pan"]);
  });

  test("dos operaciones distintas del usuario siguen siendo dos", async () => {
    // La guardia no puede tragarse una nota que el usuario escribió dos veces a
    // propósito: son dos operaciones lógicas y llevan ids distintos.
    const { app, notas } = servidor();
    await enviar(app, "op-1", "comprar pan");
    await enviar(app, "op-2", "comprar pan");
    expect(notas.length).toBe(2);
  });

  test("la cola puede reproducir varias operaciones y cada una conserva la suya", async () => {
    const { app, notas } = servidor();
    await enviar(app, "op-1", "pan");
    await enviar(app, "op-2", "leche");
    await enviar(app, "op-3", "café");
    // Se reproduce la cola entera (todas perdieron su respuesta).
    await enviar(app, "op-1", "pan");
    await enviar(app, "op-2", "leche");
    await enviar(app, "op-3", "café");
    expect(notas).toEqual(["pan", "leche", "café"]);
  });
});

describe("el orden de los middlewares en api.ts", () => {
  test("medición por AFUERA, idempotencia por adentro", async () => {
    // Si alguien los da vuelta, el replay deja de poder avisar que no hay que
    // cobrar (REV-044) y el 429 del tope dejaría de contestarse antes de tocar
    // nada. El orden es parte del contrato y no se ve leyendo un solo archivo.
    const fuente = await Bun.file(join(SRC, "api.ts")).text();
    const iMedicion = fuente.indexOf("api.use(\"*\", metering());");
    const iIdem = fuente.indexOf("api.use(\"*\", idempotency(");
    expect(iMedicion).toBeGreaterThan(-1);
    expect(iIdem).toBeGreaterThan(-1);
    expect(iMedicion).toBeLessThan(iIdem);
  });
});
