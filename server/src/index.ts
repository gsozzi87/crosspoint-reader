// Punto de entrada. Railway: Root Directory = server, start = bun run src/index.ts.
import { Hono } from "hono";
import { api } from "./api";
import { firmware } from "./firmware";
import { board } from "./board";
import { warmUp } from "./tts";
import { normalizeLang } from "./lang";

const app = new Hono();

app.get("/", (c) => c.text("ws397 server ok"));
app.route("/firmware", firmware);
app.route("/board", board);  // página web para el teléfono (pide el token del aparato)
app.route("/api", api);

warmUp(normalizeLang(process.env.HUB_LANG ?? "es"));  // Piper carga el modelo una vez

const port = Number(process.env.PORT ?? 3000);
console.log(`ws397 server on :${port}`);

export default { port, fetch: app.fetch };
