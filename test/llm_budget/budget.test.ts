// REV-049: qué campo de presupuesto se manda y con cuánto.
import { expect, test } from "bun:test";
import {
  budgetField,
  isReasoningModel,
  planBudget,
  REASONING_HEADROOM,
  usesCompletionTokens,
} from "../../server/src/llmBudget";

const GROQ = "https://api.groq.com/openai/v1";
const OPENAI = "https://api.openai.com/v1";
const DEEPSEEK = "https://api.deepseek.com/v1";

test("qué modelos razonan", () => {
  for (const m of ["openai/gpt-oss-120b", "openai/gpt-oss-20b", "o3-mini", "o1", "deepseek-reasoner",
                   "qwen3-32b-thinking", "magistral-small", "qwq-32b"]) {
    expect(isReasoningModel(m)).toBe(true);
  }
  for (const m of ["llama-3.3-70b-versatile", "gpt-4o-mini", "deepseek-chat", "claude-haiku-4-5", ""]) {
    expect(isReasoningModel(m)).toBe(false);
  }
});

test("el campo sale del HOST, no del modelo", () => {
  expect(usesCompletionTokens(GROQ)).toBe(true);
  expect(usesCompletionTokens(OPENAI)).toBe(true);
  // DeepSeek y cualquier compatible desconocido sólo entienden el viejo: un
  // campo que el proveedor no conoce es un 400, así que ante la duda va ese.
  expect(usesCompletionTokens(DEEPSEEK)).toBe(false);
  expect(usesCompletionTokens("no es una url")).toBe(false);
  // Y no se confunde con un host que apenas se le parezca.
  expect(usesCompletionTokens("https://api.groq.com.evil.test/v1")).toBe(false);
});

test("un modelo que razona recibe lugar para pensar", () => {
  const p = planBudget(GROQ, "openai/gpt-oss-120b", 10);
  expect(p.field).toBe("max_completion_tokens");
  expect(p.reasoning).toBe(true);
  // Éste es EL defecto: con 10 tokens el modelo piensa y no le queda ninguno
  // para escribir, y contesta 200 con el texto vacío.
  expect(p.value).toBe(10 + REASONING_HEADROOM);
});

test("un modelo que no razona no paga el extra", () => {
  const p = planBudget(GROQ, "llama-3.3-70b-versatile", 900);
  expect(p.value).toBe(900);
  expect(p.reasoning).toBe(false);
});

test("el clasificador de voz (chatJson, 1024) también entra con lugar", () => {
  expect(planBudget(GROQ, "openai/gpt-oss-120b", 1024).value).toBe(1024 + REASONING_HEADROOM);
});

test("budgetField arma UN solo campo", () => {
  expect(budgetField(GROQ, "openai/gpt-oss-120b", 64)).toEqual({ max_completion_tokens: 64 + REASONING_HEADROOM });
  expect(budgetField(DEEPSEEK, "deepseek-chat", 900)).toEqual({ max_tokens: 900 });
  // Nunca los dos a la vez: mandar los dos es un 400 en OpenAI.
  expect(Object.keys(budgetField(OPENAI, "gpt-4o-mini", 100)).length).toBe(1);
});

test("un presupuesto absurdo no se convierte en cero", () => {
  expect(planBudget(DEEPSEEK, "deepseek-chat", 0).value).toBe(1);
  expect(planBudget(DEEPSEEK, "deepseek-chat", Number.NaN).value).toBe(1);
});
