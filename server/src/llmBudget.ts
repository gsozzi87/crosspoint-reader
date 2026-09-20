// REV-049: cuánto presupuesto de salida se le pide a una API compatible con
// OpenAI, y qué decir cuando contesta 200 con el texto VACÍO.
//
// El caso que lo destapó: con `openai/gpt-oss-120b` en Groq, la prueba de
// /board decía `→ "" (131 ms)`. O sea llamada exitosa, rápida… y sin una
// palabra. Y como el resto del servidor no mira ese vacío, terminaba saliendo
// por el lado equivocado: `chatJson` tira "el modelo no devolvió JSON" y el
// Traductor "empty translation", los dos con 502. Desde el aparato eso es el
// mismo cartel que un proveedor caído.
//
// Dos cosas de estos modelos explican el vacío, y las dos son de presupuesto:
//
//   1. Un modelo de razonamiento gasta tokens PENSANDO antes de escribir, y
//      esos tokens salen del MISMO presupuesto. Pedirle 10 tokens (lo que
//      pedía la prueba de /board) o 900 (lo que pide una respuesta corta) se
//      los puede comer enteros el razonamiento y no queda ninguno para la
//      respuesta: `finish_reason: "length"` y `content: ""`.
//   2. `max_tokens` está DEPRECADO en las dos APIs que lo soportan de verdad
//      (OpenAI y Groq) a favor de `max_completion_tokens`, que es el que
//      contempla el razonamiento. Los demás compatibles (DeepSeek y compañía)
//      sólo entienden `max_tokens`, así que no se puede mandar el nuevo a
//      todos: un campo desconocido es un 400.
//
// Todo lo de acá es puro y sin red, para poder probarlo sin clave ni
// proveedor: ./test/llm_budget/run.sh

// Los modelos que razonan antes de contestar. La lista es por familia y no por
// nombre exacto a propósito: los proveedores publican variantes todo el tiempo
// (gpt-oss-20b, gpt-oss-120b, o3-mini, deepseek-reasoner…) y una lista exacta
// se congela.
const REASONING = /(^|[/\-_])(gpt-oss|o1|o3|o4|deepseek-reasoner|magistral|qwq)([-_.]|$)|thinking|reasoner/i;

export function isReasoningModel(model: string): boolean {
  return REASONING.test(model ?? "");
}

// Quién entiende `max_completion_tokens`. Por HOST y no por modelo: es la API
// la que define el campo. Ante la duda se manda el viejo, que es el que
// entienden todos los compatibles.
const COMPLETION_TOKENS_HOSTS = /(^|\.)(groq\.com|openai\.com)$/i;

export function usesCompletionTokens(baseUrl: string): boolean {
  try {
    return COMPLETION_TOKENS_HOSTS.test(new URL(baseUrl).hostname);
  } catch {
    return false;
  }
}

// Lo que el razonamiento se puede comer antes de empezar a escribir. No es un
// costo fijo: son tokens que el modelo YA estaba gastando — la diferencia es
// que antes se los gastaba y encima devolvía vacío, así que se pagaban dos
// veces (la llamada perdida y el reintento).
export const REASONING_HEADROOM = 1024;

export type BudgetPlan = {
  field: "max_tokens" | "max_completion_tokens";
  value: number;
  reasoning: boolean;
};

export function planBudget(baseUrl: string, model: string, wanted: number): BudgetPlan {
  const reasoning = isReasoningModel(model);
  const want = Math.max(1, Math.floor(wanted) || 1);
  return {
    field: usesCompletionTokens(baseUrl) ? "max_completion_tokens" : "max_tokens",
    value: reasoning ? want + REASONING_HEADROOM : want,
    reasoning,
  };
}

// El campo listo para meter en el cuerpo del pedido.
export function budgetField(baseUrl: string, model: string, wanted: number): Record<string, number> {
  const plan = planBudget(baseUrl, model, wanted);
  return { [plan.field]: plan.value };
}

export type EmptyInfo = {
  model: string;
  finishReason?: string;
  reasoningChars?: number;   // lo que el modelo escribió PENSANDO, si lo devolvió aparte
  completionTokens?: number;
  reasoningTokens?: number;
  budget: number;
};

// Qué decirle al dueño cuando el proveedor contesta bien y no dice nada.
//
// Las tres causas mandan a lugares distintos y hasta ahora llegaban como el
// mismo 502, así que el mensaje las separa.
export function emptyReplyMessage(info: EmptyInfo): string {
  const partes: string[] = [`${info.model} contestó 200 pero sin texto`];
  if (info.finishReason) partes.push(`finish_reason=${info.finishReason}`);
  if (info.completionTokens !== undefined) partes.push(`salida=${info.completionTokens} tokens`);
  if (info.reasoningTokens !== undefined) partes.push(`razonamiento=${info.reasoningTokens} tokens`);
  if (info.reasoningChars) partes.push(`pensó ${info.reasoningChars} caracteres y no escribió ninguno`);
  partes.push(`presupuesto=${info.budget}`);
  let pista = "";
  if (info.finishReason === "length") {
    pista = " — se quedó sin presupuesto: es un modelo de razonamiento y se lo comió pensando. Subí el límite o elegí un modelo que no razone.";
  } else if (info.reasoningChars || info.reasoningTokens) {
    pista = " — razonó pero no escribió la respuesta. Suele ser el mismo problema de presupuesto.";
  } else {
    pista = " — el proveedor no devolvió nada. Probá otro modelo en la web (Ajustes → Avanzado → IA).";
  }
  return partes.join(", ") + pista;
}
