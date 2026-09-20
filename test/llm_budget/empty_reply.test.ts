// REV-049: el 200 con el texto vacío, extremo a extremo contra un proveedor
// falso.
//
// Lo que se prueba es lo que el dueño vio en producción: Groq contesta 200 en
// 131 ms con `content: ""`, y antes de este arreglo eso salía por el lado
// equivocado — `chatJson` decía "el modelo no devolvió JSON" y el Traductor
// "empty translation" —, o sea que la causa llegaba disfrazada de otra cosa.
import { expect, test, beforeEach, afterEach } from "bun:test";
import { emptyReplyMessage } from "../../server/src/llmBudget";

test("el mensaje separa las tres causas", () => {
  // 1. Se comió el presupuesto pensando: eso es lo que hay que subir.
  const sinPresupuesto = emptyReplyMessage({
    model: "openai/gpt-oss-120b",
    finishReason: "length",
    reasoningChars: 0,
    completionTokens: 10,
    reasoningTokens: 10,
    budget: 10,
  });
  expect(sinPresupuesto).toContain("finish_reason=length");
  expect(sinPresupuesto).toContain("presupuesto=10");
  expect(sinPresupuesto).toMatch(/sin presupuesto/);

  // 2. Razonó y no escribió: mismo problema, otra forma de verse.
  const razono = emptyReplyMessage({
    model: "openai/gpt-oss-120b",
    finishReason: "stop",
    reasoningChars: 1200,
    budget: 1034,
  });
  expect(razono).toContain("1200 caracteres");
  expect(razono).toMatch(/razonó pero no escribió/);

  // 3. El proveedor directamente no devolvió nada: eso NO se arregla subiendo
  //    el presupuesto y el mensaje no tiene que decir que sí.
  const nada = emptyReplyMessage({ model: "llama-3.3-70b-versatile", finishReason: "stop", budget: 900 });
  expect(nada).toMatch(/no devolvió nada/);
  expect(nada).not.toMatch(/sin presupuesto/);

  // Y en los tres casos se nombra el modelo, que es lo que hay que cambiar en
  // la web si el problema es el modelo.
  for (const m of [sinPresupuesto, razono, nada]) expect(m).toContain("contestó 200 pero sin texto");
});

// ── El camino entero, con un proveedor falso ────────────────────────────────
//
// Levanta un servidor local que contesta como Groq (200, content vacío) y
// comprueba que `chatText` tira un LlmError diagnóstico en vez de devolver ""
// y dejar que reviente tres capas más arriba.
// Config de prueba: `config()` lee `CONFIG_FILE`, asi que se apunta a un
// temporal ANTES de que el modulo se cargue.
process.env.CONFIG_FILE = `${process.env.TMPDIR ?? "/tmp"}/ws397-llm-budget-${process.pid}.json`;

let server: ReturnType<typeof Bun.serve> | null = null;
let lastBody: any = null;

beforeEach(() => {
  lastBody = null;
});
afterEach(() => {
  server?.stop(true);
  server = null;
});

async function conProveedor(respuesta: unknown, modelo: string): Promise<void> {
  server = Bun.serve({
    port: 0,
    async fetch(req) {
      lastBody = await req.json();
      return Response.json(respuesta);
    },
  });
  const base = `http://127.0.0.1:${server.port}/v1`;
  const { config, saveConfig } = await import("../../server/src/config");
  const c = await config();
  await saveConfig({ ...c, llm: { ...c.llm, provider: "openai", baseUrl: base, model: modelo, key: "clave-de-prueba" } });
}

test("200 con content vacio es un error con diagnostico, no un ''", async () => {
  await conProveedor(
    {
      choices: [{ finish_reason: "length", message: { content: "", reasoning: "pensando".repeat(20) } }],
      usage: { completion_tokens: 1034, completion_tokens_details: { reasoning_tokens: 1034 } },
    },
    "openai/gpt-oss-120b",
  );
  const { chatText } = await import("../../server/src/llm");
  let msg = "";
  try {
    await chatText({ system: "s", user: "u", maxTokens: 64, search: "off" });
  } catch (err) {
    msg = String(err instanceof Error ? err.message : err);
  }
  expect(msg).toContain("contestó 200 pero sin texto");
  expect(msg).toContain("finish_reason=length");
  expect(msg).toContain("razonamiento=1034 tokens");
  // Y NO el mensaje que salia antes, que mandaba a buscar el problema al lugar
  // equivocado (`chatJson` decia "el modelo no devolvio JSON").
  expect(msg).not.toContain("no devolvió JSON");
});

test("el modelo que razona pide lugar para razonar", async () => {
  await conProveedor({ choices: [{ finish_reason: "stop", message: { content: "ok" } }] }, "openai/gpt-oss-120b");
  const { chatText } = await import("../../server/src/llm");
  expect(await chatText({ system: "s", user: "u", maxTokens: 64, search: "off" })).toBe("ok");
  // Un host desconocido (127.0.0.1) manda el campo VIEJO, que es el que
  // entienden todos los compatibles; el valor si lleva el lugar del
  // razonamiento, que es lo que faltaba.
  expect(lastBody.max_tokens).toBe(64 + 1024);
  expect(lastBody.max_completion_tokens).toBeUndefined();
});

test("un modelo que no razona pide exactamente lo pedido", async () => {
  await conProveedor({ choices: [{ finish_reason: "stop", message: { content: "ok" } }] }, "llama-3.3-70b-versatile");
  const { chatText } = await import("../../server/src/llm");
  expect(await chatText({ system: "s", user: "u", maxTokens: 900, search: "off" })).toBe("ok");
  expect(lastBody.max_tokens).toBe(900);
});
