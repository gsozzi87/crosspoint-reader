// REV-054: el error del proveedor, dicho para que se pueda leer en el aparato.
//
// El cuerpo real que devolvió Groq en el aparato del dueño (1.5.119) y que
// llegaba crudo y recortado a la pantalla:
//
//   http error (502): api.groq.com 429: {"error":{"message":"Rate limit reached
//   for model `openai/gpt-oss-120b` in organization `org_01m215hc0...
//
// El dato que importa —cuánto esperar— quedaba del otro lado del recorte,
// detrás de un id de organización que no le sirve a nadie.
import { expect, test } from "bun:test";
import { describeProviderError, isRateLimit, providerMessage } from "../../server/src/providerError";

const GROQ_429 = JSON.stringify({
  error: {
    message:
      "Rate limit reached for model `openai/gpt-oss-120b` in organization `org_01m215hc0abcdef` " +
      "service tier `on_demand` on tokens per day (TPD): Limit 100000, Used 100000. " +
      "Please try again in 7m32.1s.",
    type: "tokens",
    code: "rate_limit_exceeded",
  },
});

test("se saca el mensaje del proveedor y no el JSON crudo", () => {
  expect(providerMessage(GROQ_429)).toStartWith("Rate limit reached for model");
  // Un cuerpo que no es JSON se usa tal cual.
  expect(providerMessage("Bad Gateway")).toBe("Bad Gateway");
  expect(providerMessage("")).toBe("");
});

test("un 429 es un tope de uso, y también cuando viene como 400", () => {
  expect(isRateLimit(429, "lo que sea")).toBe(true);
  expect(isRateLimit(400, "Rate limit reached for model X")).toBe(true);
  expect(isRateLimit(400, "quota exceeded")).toBe(true);
  expect(isRateLimit(400, "model not found")).toBe(false);
  expect(isRateLimit(500, "internal error")).toBe(false);
});

test("el tope de uso se explica y dice cuánto esperar", () => {
  const m = describeProviderError("api.groq.com", 429, GROQ_429, "openai/gpt-oss-120b");
  expect(m).toContain("límite de uso");
  expect(m).toContain("api.groq.com");
  expect(m).toContain("openai/gpt-oss-120b");
  // Lo único accionable del mensaje del proveedor: cuánto falta.
  expect(m).toContain("7m32.1s");
  // Y qué hacer si no querés esperar.
  expect(m).toContain("cambiar de modelo");
  // Nada del id de la organización, que no le sirve a nadie y se comía el
  // recorte de 120 caracteres del aparato.
  expect(m).not.toContain("org_01m215hc0");
  // Entra en lo que el aparato muestra.
  expect(m.length).toBeLessThanOrEqual(200);
});

test("sin 'try again in' igual se explica", () => {
  const body = JSON.stringify({ error: { message: "Rate limit reached." } });
  const m = describeProviderError("api.groq.com", 429, body, "modelo-x");
  expect(m).toContain("límite de uso");
  expect(m).not.toContain("Probá de nuevo en");
});

test("lo que NO es un tope sigue llegando como venía", () => {
  const body = JSON.stringify({ error: { message: "model `x` does not exist" } });
  const m = describeProviderError("api.groq.com", 404, body, "x");
  expect(m).toBe("api.groq.com 404: model `x` does not exist");
  expect(m).not.toContain("límite de uso");
});
