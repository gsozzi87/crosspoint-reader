// REV-047: los últimos fallos del proveedor, a la vista sin el aparato.
//
// Cuando la clave se vence, el modelo se da de baja o el proveedor devuelve
// 400, TODA función de IA contesta 502 a la vez — Hablar, Preguntarle al libro,
// el Traductor, la Biblia — y desde afuera es el mismo cartel para cualquiera
// de esas causas. El motivo lo sabe el servidor (lo escribe en `LlmError`), lo
// manda en el JSON de error… y hasta ahora se perdía apenas el aparato lo
// descartaba: para verlo había que entrar a los logs de Railway.
//
// Esto lo guarda en memoria —es diagnóstico, no un dato— y `/board` → Ajustes →
// Avanzado → IA lo muestra. Un redespliegue lo vacía, que es lo correcto: lo
// que interesa es si está fallando AHORA.
//
// En memoria y no en el volumen a propósito: escribir un archivo en el camino
// de error de cada petición es la clase de cosa que convierte un proveedor
// caído en un disco lleno.

export type ProviderFailure = {
  at: number;      // epoch ms
  kind: "llm" | "stt";
  where: string;   // qué lo pidió ("chatText", "transcribe", …)
  message: string;
};

export const MAX_FAILURES = 20;
const MSG_MAX = 300;

const ring: ProviderFailure[] = [];

export function recordProviderFailure(kind: ProviderFailure["kind"], where: string, err: unknown): void {
  const message = String(err instanceof Error ? err.message : err).slice(0, MSG_MAX);
  ring.push({ at: Date.now(), kind, where, message });
  while (ring.length > MAX_FAILURES) ring.shift();
}

// Más nuevo primero, que es como se lee.
export function providerFailures(): ProviderFailure[] {
  return ring.slice().reverse();
}

export function clearProviderFailures(): void {
  ring.length = 0;
}
