// Consumo por cuenta y tope mensual.
//
// Con 1000 aparatos vendidos las claves de IA las pone el operador (ver
// `config.ts`), así que la cuenta de lo que gasta cada usuario la lleva el
// servidor: llamadas al LLM y segundos de audio transcritos, por mes y por
// cuenta, en la tabla `usage`.
//
//   MONTHLY_LLM_CALLS     tope de llamadas al modelo por cuenta y por mes
//   MONTHLY_STT_SECONDS   tope de segundos de audio transcritos
//
// Sin poner ninguna de las dos no hay tope. Sin `DATABASE_URL` tampoco: en modo
// de un solo usuario no se cuenta nada y todo funciona como siempre.
//
// Pasado el tope, /api/ask, /api/voice, /api/transcribe y /api/translate
// contestan 429 con el mensaje en el idioma del pedido. TODO lo demás (hub,
// calendario, biblia offline, música, fotos, noticias) sigue andando: el aparato
// no se convierte en un ladrillo por haberse pasado de preguntas.
import { db, multiUser, num } from "./db";
import type { Lang } from "./lang";

const MAX_LLM = Number(process.env.MONTHLY_LLM_CALLS ?? "") || 0;
const MAX_STT = Number(process.env.MONTHLY_STT_SECONDS ?? "") || 0;

export const quotasOn = multiUser && (MAX_LLM > 0 || MAX_STT > 0);

export const QUOTA_CODE = "quota";

export const QUOTA_MSG: Record<Lang, string> = {
  es: "Se acabaron las consultas del mes. El resto del aparato sigue funcionando.",
  en: "You've used up this month's requests. Everything else on the device still works.",
  fr: "Les requêtes du mois sont épuisées. Le reste de l'appareil fonctionne toujours.",
  de: "Die Anfragen dieses Monats sind aufgebraucht. Der Rest des Geräts funktioniert weiter.",
  pt: "As consultas do mês acabaram. O resto do aparelho continua funcionando.",
  ru: "Запросы на этот месяц закончились. Остальное устройство продолжает работать.",
};

/** El primer día del mes en curso, que es la clave de la tabla. */
function month(): string {
  const d = new Date();
  return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, "0")}-01`;
}

export type Usage = { llmCalls: number; sttSeconds: number };

export async function usageOf(accountId: number): Promise<Usage> {
  if (!multiUser) return { llmCalls: 0, sttSeconds: 0 };
  const rows = (await db()`
    SELECT llm_calls, stt_seconds FROM usage WHERE account_id = ${accountId} AND month = ${month()}::date`) as any[];
  if (!rows.length) return { llmCalls: 0, sttSeconds: 0 };
  return { llmCalls: num(rows[0].llm_calls), sttSeconds: num(rows[0].stt_seconds) };
}

export const LIMITS = { llmCalls: MAX_LLM, sttSeconds: MAX_STT };

/** true = ya se pasó del tope y hay que contestarle 429. */
export async function overQuota(accountId: number): Promise<boolean> {
  if (!quotasOn) return false;
  const u = await usageOf(accountId);
  if (MAX_LLM > 0 && u.llmCalls >= MAX_LLM) return true;
  if (MAX_STT > 0 && u.sttSeconds >= MAX_STT) return true;
  return false;
}

/** Suma lo consumido. Nunca tira: que falle la contabilidad no puede tirar el pedido. */
export async function addUsage(accountId: number, add: { llm?: number; sttSeconds?: number }): Promise<void> {
  if (!multiUser) return;
  const llm = Math.max(0, Math.round(add.llm ?? 0));
  const stt = Math.max(0, Math.round(add.sttSeconds ?? 0));
  if (!llm && !stt) return;
  try {
    await db()`
      INSERT INTO usage (account_id, month, llm_calls, stt_seconds)
      VALUES (${accountId}, ${month()}::date, ${llm}, ${stt})
      ON CONFLICT (account_id, month) DO UPDATE
        SET llm_calls = usage.llm_calls + EXCLUDED.llm_calls,
            stt_seconds = usage.stt_seconds + EXCLUDED.stt_seconds`;
  } catch (err) {
    console.error("usage:", err);
  }
}

/**
 * Cuántos segundos de audio trae un cuerpo, mirando nada más que su tamaño.
 * WAV de 16 kHz mono 16 bits = 32.000 B/s; el ADPCM que manda el aparato es la
 * cuarta parte. Alcanza de sobra para contar: no hay que decodificar nada.
 */
export function audioSeconds(bytes: number, contentType: string | null | undefined): number {
  const adpcm = (contentType ?? "").includes("adpcm");
  const perSecond = adpcm ? 8_000 : 32_000;
  return Math.max(0, Math.round(bytes / perSecond));
}

export type UsageReservation = { month: string; llm: number; stt: number };

/** Reserve before calling a provider; the conditional UPDATE serializes replicas. */
export async function reserveUsage(accountId: number, add: { llm?: number; sttSeconds?: number }): Promise<UsageReservation | null> {
  const ticket = { month: month(), llm: Math.max(0, Math.round(add.llm ?? 0)), stt: Math.max(0, Math.round(add.sttSeconds ?? 0)) };
  if (!multiUser) return ticket;
  await db()`
    INSERT INTO usage (account_id, month, llm_calls, stt_seconds)
    VALUES (${accountId}, ${ticket.month}::date, 0, 0)
    ON CONFLICT (account_id, month) DO NOTHING`;
  const rows = (await db()`
    UPDATE usage SET llm_calls = llm_calls + ${ticket.llm}, stt_seconds = stt_seconds + ${ticket.stt}
    WHERE account_id = ${accountId} AND month = ${ticket.month}::date
      AND (${ticket.llm} = 0 OR ${MAX_LLM} <= 0 OR llm_calls + ${ticket.llm} <= ${MAX_LLM})
      AND (${ticket.stt} = 0 OR ${MAX_STT} <= 0 OR stt_seconds + ${ticket.stt} <= ${MAX_STT})
    RETURNING account_id`) as any[];
  return rows.length ? ticket : null;
}

/** Refund failures against the reserved month, including across midnight/month end. */
export async function releaseUsage(accountId: number, ticket: UsageReservation): Promise<void> {
  if (!multiUser) return;
  await db()`
    UPDATE usage SET llm_calls = GREATEST(0, llm_calls - ${ticket.llm}), stt_seconds = GREATEST(0, stt_seconds - ${ticket.stt})
    WHERE account_id = ${accountId} AND month = ${ticket.month}::date`;
}
