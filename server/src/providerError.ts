// El error del proveedor, dicho en una línea que se pueda leer en el aparato.
//
// Antes se pegaba el cuerpo crudo: el aparato mostraba
//
//   http error (502): api.groq.com 429: {"error":{"message":"Rate limit reached
//   for model `openai/gpt-oss-120b` in organization `org_01m215hc0...
//
// y el dato que importa —cuánto hay que esperar— quedaba del otro lado del
// recorte, detrás de un id de organización que no le sirve a nadie. Las APIs
// compatibles con OpenAI ponen el texto bueno en `error.message`; se saca de
// ahí y se manda ESO.
//
// Puro y sin red: ./test/provider_error/run.sh

// El `error.message` del proveedor, si el cuerpo es el JSON que documentan.
export function providerMessage(body: string): string {
  try {
    const j = JSON.parse(body);
    const m = j?.error?.message ?? j?.message;
    if (typeof m === "string" && m.trim()) return m.trim();
  } catch { /* no es JSON: se usa el cuerpo tal cual */ }
  return body.trim();
}

// ¿Es un límite de uso? No alcanza con el 429: algunos proveedores lo mandan
// como 400 con el texto, y el 429 de una pasarela puede no serlo.
export function isRateLimit(status: number, message: string): boolean {
  if (status === 429) return true;
  return /rate limit|quota exceeded|too many requests/i.test(message);
}

// Lo que ve el dueño.
//
// Un límite de uso NO es "el proveedor falló": es "esperá, o cambiá de modelo",
// y eso hay que decirlo, porque las dos cosas se arreglan distinto.
export function describeProviderError(host: string, status: number, body: string, model: string): string {
  const msg = providerMessage(body);
  if (isRateLimit(status, msg)) {
    // Groq y OpenAI dicen "Please try again in 7m32.1s": se rescata.
    const espera = /try again in ([0-9hms.]+)/i.exec(msg);
    const cuando = espera ? ` Probá de nuevo en ${espera[1]}.` : "";
    return `Te pasaste del límite de uso de ${host} con ${model}.${cuando} Podés esperar o cambiar de modelo en Ajustes → Avanzado → IA.`;
  }
  return `${host} ${status}: ${msg.slice(0, 200)}`;
}
