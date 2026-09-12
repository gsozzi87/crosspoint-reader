import { beforeAll, afterAll, expect, test } from "bun:test";
import { Hono } from "hono";
import { db, initDb, dbWriteDoc, sha256Hex } from "../src/db";
import { load, mutate, nextId } from "../src/store";
import { reserveUsage, releaseUsage, usageOf } from "../src/usage";
import { accountApi, auth } from "../src/accounts";
import type { AppEnv } from "../src/tenant";

// This suite writes fixtures: refuse to run against an ordinary deployment.
if (process.env.PRODUCT_AUDIT_TESTS !== "1" || !process.env.DATABASE_URL?.includes("/crosspoint_audit_test")) {
  throw new Error("Use a disposable crosspoint_audit_test database and PRODUCT_AUDIT_TESTS=1");
}
let a: number, b: number;
const app = new Hono<AppEnv>();
app.use("/account/*", async (c, next) => {
  c.set("accountId", c.req.header("x-test-account") === "b" ? b : a);
  c.set("via", "session");
  await next();
});
app.route("/account", accountApi);
app.route("/auth", auth);
beforeAll(async () => {
  await initDb();
  const rows = await db()`INSERT INTO accounts (email, pass_hash) VALUES ('audit-a@example.test','test'),('audit-b@example.test','test') RETURNING id`;
  a = Number(rows[0].id); b = Number(rows[1].id);
});
afterAll(async () => { await db().close(); });

test("load sees externally committed changes and does not expose mutable cached objects", async () => {
  const initial = await load(a);
  initial.notes.push({id: 100, text: "not committed", createdAt: "2026-09-12"});
  expect((await load(a)).notes).toHaveLength(0);
  await dbWriteDoc(a, "store", {...initial, notes: [{id: 101, text: "other replica", createdAt: "2026-09-12"}]});
  expect((await load(a)).notes[0].text).toBe("other replica");
});
test("concurrent store mutations retain every write and unique ids", async () => {
  await Promise.all(Array.from({length: 12}, (_, i) => mutate(b, s => {
    s.notes.push({id: nextId(s), text: "note " + i, createdAt: "2026-09-12"});
  })));
  const notes = (await load(b)).notes;
  expect(notes).toHaveLength(12);
  expect(new Set(notes.map(n => n.id)).size).toBe(12);
});
test("failed mutations do not become visible", async () => {
  await expect(mutate(b, s => {s.notes.length = 0; throw new Error("rollback");})).rejects.toThrow("rollback");
  expect((await load(b)).notes).toHaveLength(12);
});
test("quota reservation admits one concurrent call and refunds its original month", async () => {
  expect(process.env.MONTHLY_LLM_CALLS).toBe("1");
  const tickets = await Promise.all(Array.from({length: 12}, () => reserveUsage(a, {llm: 1})));
  const admitted = tickets.filter(t => t !== null);
  expect(admitted).toHaveLength(1);
  expect((await usageOf(a)).llmCalls).toBe(1);
  await releaseUsage(a, admitted[0]!);
  expect((await usageOf(a)).llmCalls).toBe(0);
  expect(await reserveUsage(a, {sttSeconds: 11})).toBeNull();
});
test("a pairing code is consumed by exactly one account", async () => {
  await db()`INSERT INTO pairings (code, device_id, token_hash) VALUES ('123456','ABCDEF123456',${sha256Hex("a".repeat(64))})`;
  const responses = await Promise.all(["a", "b"].map(account => app.request("/account/pair", {
    method: "POST", headers: {"content-type": "application/json", "x-test-account": account},
    body: JSON.stringify({code: "123456"}),
  })));
  expect(responses.map(r => r.status).sort()).toEqual([200, 404]);
  const rows = await db()`SELECT * FROM devices WHERE device_id = 'ABCDEF123456'`;
  expect(rows).toHaveLength(1);
});
test("an obsolete pairing credential cannot replace the current device owner", async () => {
  await db()`INSERT INTO devices (account_id, device_id, token_hash) VALUES (${a},'ABCDEF654321',${sha256Hex("b".repeat(64))})`;
  await db()`INSERT INTO pairings (code, device_id, token_hash) VALUES ('654321','ABCDEF654321',${sha256Hex("c".repeat(64))})`;
  const response = await app.request("/account/pair", {
    method: "POST", headers: {"content-type": "application/json", "x-test-account": "b"},
    body: JSON.stringify({code: "654321"}),
  });
  expect(response.status).toBe(403);
  const rows = await db()`SELECT account_id FROM devices WHERE device_id = 'ABCDEF654321'`;
  expect(Number(rows[0].account_id)).toBe(a);
});
test("public registration cannot claim ADMIN_EMAIL privileges", async () => {
  expect(process.env.ADMIN_EMAIL).toBe("operator@example.test");
  const response = await app.request("/auth/register", {method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify({email: "operator@example.test", password: "audit-password-only"}),
  });
  expect(response.status).toBe(200);
  expect((await response.json()).isAdmin).toBe(false);
  const rows = await db()`SELECT is_admin FROM accounts WHERE email = 'operator@example.test'`;
  expect(rows[0].is_admin).toBe(false);
});
