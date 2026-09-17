// La poda de /api/log: qué se guarda y qué se tira.
//
// Es lógica pura sobre texto, así que se prueba de escritorio. Lo que hay que
// defender no es "corta bien", es que NUNCA deje la página vacía: el log es la
// única forma de diagnosticar el aparato sin cable, y una poda demasiado
// entusiasta se lleva puesto justo lo que uno fue a buscar.
import { describe, expect, test } from "bun:test";
import { prune } from "../../server/src/devicelog";

const HORA = 60 * 60 * 1000;
const AHORA = Date.parse("2026-09-17T12:00:00.000Z");

function tanda(iso: string, cuerpo: string): string {
  return `\n===== ${iso} =====\n${cuerpo}`;
}

describe("prune", () => {
  test("tira las tandas de hace más de un día y deja las de adentro", () => {
    const viejo = tanda("2026-09-14T08:00:00.000Z", "=== 1.5.88-ws397 | arranque por boton ===\nviejo");
    const ayer = tanda("2026-09-16T20:00:00.000Z", "de ayer a la noche");
    const hoy = tanda("2026-09-17T11:00:00.000Z", "de recién");
    const out = prune(viejo + ayer + hoy, AHORA);
    expect(out).not.toContain("1.5.88-ws397");
    expect(out).not.toContain("viejo");
    expect(out).toContain("de ayer a la noche");
    expect(out).toContain("de recién");
  });

  test("corta en el PRIMER sello que entra, no en el último", () => {
    const a = tanda("2026-09-17T09:00:00.000Z", "primera de hoy");
    const b = tanda("2026-09-17T11:00:00.000Z", "segunda de hoy");
    const out = prune(a + b, AHORA);
    expect(out).toContain("primera de hoy");
    expect(out).toContain("segunda de hoy");
  });

  test("el borde son 24 h exactas: lo de justo adentro se queda", () => {
    const justo = new Date(AHORA - 24 * HORA).toISOString();
    const out = prune(tanda(justo, "en el borde"), AHORA);
    expect(out).toContain("en el borde");
  });

  test("con TODO vencido queda la última subida, no una página vacía", () => {
    const a = tanda("2026-09-01T08:00:00.000Z", "la primera");
    const b = tanda("2026-09-10T08:00:00.000Z", "la ultima que subio");
    const out = prune(a + b, AHORA);
    expect(out).not.toContain("la primera");
    expect(out).toContain("la ultima que subio");
    expect(out.trim().length).toBeGreaterThan(0);
  });

  test("sin sellos no se toca nada (mejor de más que borrar a ciegas)", () => {
    const crudo = "un log escrito a mano\nsin ningun sello\n";
    expect(prune(crudo, AHORA)).toBe(crudo);
  });

  test("un sello con fecha ilegible no cuenta como corte", () => {
    const roto = "\n===== no-es-una-fecha =====\ncuerpo\n";
    const bueno = tanda("2026-09-17T11:00:00.000Z", "de recién");
    const out = prune(roto + bueno, AHORA);
    expect(out).toContain("de recién");
    expect(out).not.toContain("cuerpo");
  });

  test("el vacío sigue vacío", () => {
    expect(prune("", AHORA)).toBe("");
  });

  test("una sola tanda reciente se deja entera, sin comerse la primera línea", () => {
    const solo = tanda("2026-09-17T11:00:00.000Z", "todo el cuerpo");
    const out = prune(solo, AHORA);
    expect(out).toContain("todo el cuerpo");
    expect(out).toContain("2026-09-17T11:00:00.000Z");
  });
});
