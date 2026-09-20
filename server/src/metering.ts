// Tope mensual y conteo de consumo, como middleware.
//
// Lo único que se cobra es lo que le cuesta plata al operador: el modelo y la
// transcripción. Pasado el tope, estas seis rutas contestan 429 en el idioma
// del pedido y TODO lo demás (hub, calendario, biblia, música, noticias) sigue
// andando. Sin `DATABASE_URL` o sin topes puestos, `overQuota` es false y esto
// no hace nada.
//
// Una tabla y no una lista de rutas: hay rutas que gastan modelo pero NO mandan
// audio, y con la lista pelada los 40 KB de JSON de un capítulo de la Biblia se
// cobraban como segundos de audio. `audio` dice si el cuerpo es una toma del
// micrófono; `llm` cuántas llamadas al modelo hace la ruta.
//
// Vive en su propio archivo (y no adentro de api.ts) para poder componerlo en
// una prueba junto al middleware de idempotencia: el defecto de REV-044 no
// estaba en ninguno de los dos sino en el ORDEN entre ellos, y eso no se ve
// leyendo cada uno por separado.
import { isReplay } from "./idempotency";
import { normalizeLang } from "./lang";
import { accountOf } from "./tenant";
import { addUsage, audioSeconds, overQuota, QUOTA_CODE, QUOTA_MSG } from "./usage";

export type MeteredRoute = { path: string; llm: number; audio: boolean };

export const METERED: MeteredRoute[] = [
  { path: "/api/ask", llm: 1, audio: false },
  { path: "/api/voice", llm: 1, audio: true },
  { path: "/api/transcribe", llm: 0, audio: true },
  { path: "/api/translate", llm: 1, audio: true },
  { path: "/api/bible/ask", llm: 1, audio: false },
  { path: "/api/calendar/dictate", llm: 1, audio: false },
];

export function meteredFor(path: string, table: MeteredRoute[] = METERED): MeteredRoute | undefined {
  return table.find((e) => path === e.path || path.startsWith(`${e.path}/`));
}

// ¿Se le cobra al usuario esta respuesta?
//
// Pura para poder probarla: son las dos reglas juntas.
//   - un 4xx/5xx no se cobra (un 502 del proveedor no es de nadie);
//   - un REPLAY tampoco, porque no ejecutó STT, ni modelo, ni TTS: es la misma
//     respuesta que ya se cobró la primera vez (REV-044).
export function shouldCharge(status: number, replay: boolean): boolean {
  return status < 400 && !replay;
}

export function metering(table: MeteredRoute[] = METERED) {
  return async (c: any, next: () => Promise<void>) => {
    const m = meteredFor(c.req.path, table);
    if (!m) return next();
    const acc = accountOf(c);
    if (await overQuota(acc)) {
      const lang = normalizeLang(c.req.query("lang"));
      return c.json({ ok: false, error: QUOTA_MSG[lang], code: QUOTA_CODE }, 429);
    }
    const declaredBytes = Number(c.req.header("content-length") ?? 0) || 0;
    const type = c.req.header("content-type");
    await next();
    const bytes = m.audio ? (c.get("bodyBytes") ?? declaredBytes) : 0;
    if (shouldCharge(c.res.status, isReplay(c))) {
      void addUsage(acc, { llm: m.llm, sttSeconds: m.audio ? audioSeconds(bytes, type) : 0 });
    }
  };
}
