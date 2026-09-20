// REV-016: un POST que se reintenta no se puede aplicar dos veces.
//
// El aparato manda `X-Request-Id` y lo MANTIENE entre los reintentos de una
// misma petición (ServerClient::request, un id por llamada y no por intento).
// Eso ya estaba; lo que faltaba era el otro lado. El servidor lo devolvía en
// `/api/ping` y nada más, así que este escenario creaba dos cosas:
//
//   1. el aparato manda POST /api/notes (o /api/voice, o un recordatorio);
//   2. el servidor lo aplica y escribe el store;
//   3. la conexión se cae ANTES de que la respuesta llegue al aparato
//      (el keepalive la corta a los ~14 s desde 1.5.103), o Railway contesta
//      5xx con el trabajo ya hecho;
//   4. `retryable(-1)` y `retryable(5xx)` son true -> se reintenta;
//   5. el servidor lo aplica OTRA VEZ.
//
// El resultado es una nota, un recordatorio o un evento duplicado, y en
// `/api/voice` además una segunda pasada de STT + modelo + TTS que se paga.
//
// Lo que NO hacía falta arreglar, y por eso esto es una red y no el único
// control: `/api/hub/done` ya es idempotente por el `at` de la ocurrencia
// (F01, 1.5.53), `store.addListItem` no repite ítems por texto (1.5.70) y un
// borrado repetido deja el mismo estado. Los huecos eran los que CREAN algo.
//
// La regla es "cualquier POST bajo /api que traiga X-Request-Id", no una lista
// de rutas a mano: una lista hay que acordarse de actualizarla y se congela.
// La web no manda ese header, así que no la toca.

// Lo que se guarda de una respuesta ya contestada.
export type Stored = {
  status: number;
  type: string;
  body: Uint8Array;
  at: number; // ms
};

// Topes. El contenedor de Railway tiene 512 MB y ahí adentro viven también
// Piper y el masticado de noticias, así que lo que manda es el total en bytes
// y no la cantidad de entradas: una sola respuesta de /api/voice son cientos
// de KB de ADPCM.
export const TTL_MS = 10 * 60 * 1000;
export const MAX_ENTRIES = 256;
export const MAX_BODY = 512 * 1024;
export const MAX_TOTAL = 8 * 1024 * 1024;

// Caché por (cuenta, ruta, request id) con vencimiento y tope de memoria.
// Pura a propósito: se prueba sin servidor en ./test/idempotency/run.sh.
export class ReplayCache {
  private map = new Map<string, Stored>();
  private bytes = 0;

  constructor(
    readonly ttlMs: number = TTL_MS,
    readonly maxEntries: number = MAX_ENTRIES,
    readonly maxTotal: number = MAX_TOTAL,
  ) {}

  static key(accountId: number, path: string, requestId: string): string {
    return `${accountId}\u0000${path}\u0000${requestId}`;
  }

  get(key: string, now = Date.now()): Stored | null {
    const hit = this.map.get(key);
    if (!hit) return null;
    if (now - hit.at > this.ttlMs) {
      this.drop(key);
      return null;
    }
    // Se reinserta para que el orden del Map sea el de uso: el primero que
    // devuelve el iterador es el más viejo, que es el que se tira.
    this.map.delete(key);
    this.map.set(key, hit);
    return hit;
  }

  put(key: string, value: Stored): boolean {
    if (value.body.byteLength > MAX_BODY) return false;
    this.drop(key);
    this.map.set(key, value);
    this.bytes += value.body.byteLength;
    this.evict(value.at);
    return true;
  }

  // Saca lo vencido y después lo más viejo hasta entrar en los topes.
  private evict(now: number): void {
    for (const [k, v] of this.map) {
      if (now - v.at > this.ttlMs) this.drop(k);
    }
    while (this.map.size > this.maxEntries || this.bytes > this.maxTotal) {
      const oldest = this.map.keys().next();
      if (oldest.done) break;
      this.drop(oldest.value);
    }
  }

  private drop(key: string): void {
    const old = this.map.get(key);
    if (!old) return;
    this.bytes -= old.body.byteLength;
    this.map.delete(key);
  }

  get size(): number {
    return this.map.size;
  }
  get totalBytes(): number {
    return this.bytes;
  }
  clear(): void {
    this.map.clear();
    this.bytes = 0;
  }
}

// Qué respuestas se pueden repetir tal cual.
//
// Un 5xx NO se guarda: el reintento existe justamente para pasar por arriba de
// un fallo temporal, y guardarlo convertiría una caída de un segundo en diez
// minutos de la misma caída. Un 4xx sí, porque es determinista (el mismo cuerpo
// va a dar el mismo error) y repetirlo no aplica nada.
export function cacheable(status: number, bodyBytes: number): boolean {
  if (status >= 500) return false;
  return bodyBytes <= MAX_BODY;
}
