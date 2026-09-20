// El cliente HTTP con DNS fijado (`safeFetch`), probado contra un servidor local.
//
// POR QUÉ EXISTE ESTA PRUEBA: el DNS se fija para que un feed no pueda
// redirigir a la red interna de Railway, y para eso se le pasa un `lookup`
// propio a `http.request`. Pero `http.request` NO llama a ese lookup como uno
// lo escribiría: le pasa `{ all: true }` y espera un ARRAY. Devolviéndole un
// string suelto —que es lo que hacía— la salida moría antes de abrir el socket
// y TODOS los feeds fallaban a la vez con "Invalid IP address: undefined".
// Es un error que no se ve leyendo el código: hay que ejercitarlo.
import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { createServer, type Server } from "node:http";
import { safeFetch, selectResolvedAddress } from "../../server/src/net";

let server: Server;
let port = 0;

beforeAll(async () => {
  server = createServer((req, res) => {
    if (req.url === "/lento") return;  // no contesta nunca: es para el plazo
    res.writeHead(200, { "content-type": "text/plain" });
    res.end("hola desde el servidor de prueba");
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  port = (server.address() as { port: number }).port;
});

afterAll(() => server.close());

describe("safeFetch con DNS fijado", () => {
  // 127.0.0.1 es red interna, así que la guardia lo rechaza — pero lo hace
  // DESPUÉS de resolver, o sea que el camino del lookup igual no se ejercita.
  // Para eso está el test de abajo, que llama al lookup a mano.
  test("un host de red interna no sale", async () => {
    await expect(safeFetch(`http://127.0.0.1:${port}/`, {}, { timeoutMs: 2000 })).rejects.toThrow(/interna/);
  });

  test("el lookup que se le pasa a http.request tiene que respetar options.all", async () => {
    const { request: httpRequest } = await import("node:http");
    const direccion = { address: "127.0.0.1", family: 4 as const };
    // Exactamente el callback de fetchPinned.
    const lookup = (_h: string, options: { all?: boolean }, cb: (...a: unknown[]) => void) =>
      options?.all ? cb(null, [direccion]) : cb(null, direccion.address, direccion.family);

    const status = await new Promise<number | string>((resolve) => {
      const req = httpRequest(new URL(`http://no-existe-este-host.test:${port}/`),
                              { method: "GET", lookup: lookup as never }, (res) => {
        res.resume();
        resolve(res.statusCode ?? 0);
      });
      req.once("error", (e) => resolve(String(e)));
      req.end();
    });
    expect(status).toBe(200);
  });

  // La PREMISA del pin de DNS, que es lo que de verdad hay que sostener:
  // `http.request` llama al lookup pidiendo `all: true`. Si eso deja de ser
  // cierto, el callback de `fetchPinned` estaría devolviendo un array a alguien
  // que espera otra cosa y el pin se rompe en silencio.
  //
  // (Antes acá se comprobaba que el callback VIEJO —un string suelto— hiciera
  // fallar la petición. Eso no es un contrato NUESTRO sino un detalle de cuán
  // tolerante es el runtime con la forma vieja, y cambia entre versiones de
  // Bun: pasaba en el sandbox y no en el runner de CI. Se comprueba la premisa,
  // que sí es determinista.)
  // OJO con el nombre de host: tiene que ser distinto del de la prueba de
  // arriba. Bun cachea la resolución POR HOST, así que repitiendo el nombre la
  // segunda petición no vuelve a llamar al lookup y esto medía el caché en vez
  // del contrato (en Bun 1.4.2 del runner fallaba por eso; en 1.3.11 no).
  test("http.request pide options.all: por eso el lookup devuelve un array", async () => {
    const { request: httpRequest } = await import("node:http");
    let vistoAll: unknown = "no se llamó al lookup";
    const lookup = (_h: string, options: { all?: boolean }, cb: (...a: unknown[]) => void) => {
      vistoAll = options?.all;
      cb(null, [{ address: "127.0.0.1", family: 4 }]);
    };
    await new Promise<void>((resolve) => {
      const req = httpRequest(new URL(`http://otro-host-que-no-existe.test:${port}/`),
                              { method: "GET", lookup: lookup as never }, (res) => {
        res.resume();
        res.once("end", () => resolve());
      });
      req.once("error", () => resolve());
      req.end();
    });
    expect(vistoAll).toBe(true);
  });
});

describe("selectResolvedAddress", () => {
  test("elige la primera pública y saltea las internas", () => {
    expect(selectResolvedAddress([
      { address: "10.0.0.5", family: 4 },
      { address: "93.184.216.34", family: 4 },
    ])?.address).toBe("93.184.216.34");
  });

  test("descarta lo que no es una dirección (forma inesperada del resolutor)", () => {
    expect(selectResolvedAddress([{ address: undefined as never, family: 4 }])).toBeNull();
    expect(selectResolvedAddress([{ address: "", family: 4 }])).toBeNull();
    expect(selectResolvedAddress([{ address: "no-soy-una-ip", family: 4 }])).toBeNull();
  });

  test("sin candidatos devuelve null y no revienta", () => {
    expect(selectResolvedAddress([])).toBeNull();
  });
});
