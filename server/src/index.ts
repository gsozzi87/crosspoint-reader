// Punto de entrada. Railway: Root Directory = server, start = bun run src/index.ts.
import { Hono } from "hono";
import { api } from "./api";
import { firmware } from "./firmware";

const app = new Hono();

app.get("/", (c) => c.text("ws397 server ok"));
app.route("/firmware", firmware);
app.route("/api", api);

const port = Number(process.env.PORT ?? 3000);
console.log(`ws397 server on :${port}`);

export default { port, fetch: app.fetch };
