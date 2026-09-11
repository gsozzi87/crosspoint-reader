// Nota rápida, sin clasificador de intención.
//
//   POST /api/notes   (Bearer del aparato, lo chequea api.ts)
//   body: { text, lang? }
//   200: { ok: true, id }
//   400: { ok: false, error: "text required" }
//
// Por qué existe: hasta ahora TODA nota entraba por /api/voice, o sea que para
// guardar un párrafo había que esperar una llamada al LLM que no aportaba nada
// (el "Pensando..." que se le hacía eterno al usuario). Acá no se llama a
// ningún modelo: el aparato transcribe con /api/transcribe y manda el texto tal
// cual. Las notas pueden ser largas: hasta 20.000 caracteres, y lo que pase de
// ahí se corta con puntos suspensivos en vez de rebotar el pedido.
import { Hono } from "hono";
import { mutate, nextId } from "./store";
import { readBody } from "./net";
import { accountOf, type AppEnv } from "./tenant";

export const MAX_NOTE_CHARS = 20_000;

export const notes = new Hono<AppEnv>();

export function clampNote(raw: unknown): string {
  const text = (raw ?? "").toString().trim();
  return text.length > MAX_NOTE_CHARS ? text.slice(0, MAX_NOTE_CHARS - 1).trimEnd() + "…" : text;
}

notes.post("/", async (c) => {
  const b = await readBody(c);
  const text = clampNote(b.text);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const acc = accountOf(c);
  const id = await mutate(acc, (store) => {
    const newId = nextId(store);
    store.notes.push({ id: newId, text, createdAt: new Date().toISOString() });
    return newId;
  });
  console.log(`nota nueva #${id} (${text.length} caracteres)`);
  return c.json({ ok: true, id });
});
