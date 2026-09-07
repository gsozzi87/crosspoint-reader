// Respaldo del clima cuando Open-Meteo no contesta desde Railway (el aparato
// venía recibiendo 502 y el hub quedaba en blanco). api.met.no es gratis y sin
// clave; lo único que exige es un User-Agent que identifique la aplicación.
//
// Devuelve exactamente la misma forma que arma hub.ts con Open-Meteo, así que
// del lado del aparato no cambia nada.
const UA = "ws397-crosspoint-reader/1.0 (github.com/gsozzi87/crosspoint-reader)";

type Instant = {
  air_temperature: number;
  relative_humidity: number;
  wind_speed: number;
};
type Step = {
  time: string;
  data: {
    instant: { details: Instant };
    next_1_hours?: { summary?: { symbol_code?: string }; details?: { precipitation_amount?: number } };
    next_6_hours?: { summary?: { symbol_code?: string }; details?: { precipitation_amount?: number } };
  };
};

// symbol_code de met.no -> código WMO, para reusar describeWeather() y no tener
// una segunda tabla de textos por idioma.
export function wmoFromSymbol(symbol: string): number {
  const s = (symbol || "").replace(/_(day|night|polartwilight)$/, "");
  if (s === "clearsky") return 0;
  if (s === "fair") return 1;
  if (s === "partlycloudy") return 2;
  if (s === "cloudy") return 3;
  if (s === "fog") return 45;
  if (s.startsWith("lightrainshowers") || s.startsWith("rainshowers") || s.startsWith("heavyrainshowers")) return 80;
  if (s.startsWith("lightrain")) return 51;
  if (s.startsWith("heavyrain")) return 65;
  if (s.startsWith("rain")) return 63;
  if (s.startsWith("sleet")) return 67;
  if (s.startsWith("lightsnow")) return 71;
  if (s.startsWith("heavysnow")) return 75;
  if (s.startsWith("snow")) return 73;
  if (s.includes("thunder")) return 95;
  return 3;
}

export type MetNoData = {
  current: { temperature_2m: number; relative_humidity_2m: number; weather_code: number; apparent_temperature: number; wind_speed_10m: number };
  hourly: { time: string[]; temperature_2m: number[]; weather_code: number[]; precipitation_probability: number[] };
  daily: { time: string[]; temperature_2m_max: number[]; temperature_2m_min: number[]; weather_code: number[]; precipitation_probability_max: number[]; sunrise: string[]; sunset: string[] };
};

// Las horas de met.no vienen en UTC: se pasan a la hora local del lugar para que
// el aparato muestre "18:00" y no la hora de Greenwich.
function localIso(utcIso: string, tz: string): string {
  const d = new Date(utcIso);
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: tz, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit",
  }).formatToParts(d);
  const get = (t: string) => parts.find((p) => p.type === t)?.value ?? "00";
  return `${get("year")}-${get("month")}-${get("day")}T${get("hour")}:${get("minute")}`;
}

export async function metNoForecast(lat: number, lon: number, tz: string): Promise<MetNoData> {
  const url = `https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=${lat.toFixed(4)}&lon=${lon.toFixed(4)}`;
  const res = await fetch(url, { headers: { "User-Agent": UA }, signal: AbortSignal.timeout(8000) });
  if (!res.ok) throw new Error(`met.no ${res.status}`);
  const body = (await res.json()) as { properties: { timeseries: Step[] } };
  const steps = body.properties?.timeseries ?? [];
  if (!steps.length) throw new Error("met.no sin datos");

  const first = steps[0];
  const symbolOf = (s: Step) => s.data.next_1_hours?.summary?.symbol_code ?? s.data.next_6_hours?.summary?.symbol_code ?? "cloudy";
  const rainOf = (s: Step) => {
    const mm = s.data.next_1_hours?.details?.precipitation_amount ?? s.data.next_6_hours?.details?.precipitation_amount ?? 0;
    // met.no no da probabilidad en el plan gratis: se estima desde los mm.
    return mm <= 0 ? 0 : mm < 0.5 ? 30 : mm < 2 ? 60 : 90;
  };

  const hourly = { time: [] as string[], temperature_2m: [] as number[], weather_code: [] as number[], precipitation_probability: [] as number[] };
  for (const s of steps.slice(0, 48)) {
    hourly.time.push(localIso(s.time, tz));
    hourly.temperature_2m.push(s.data.instant.details.air_temperature);
    hourly.weather_code.push(wmoFromSymbol(symbolOf(s)));
    hourly.precipitation_probability.push(rainOf(s));
  }

  // Máximas y mínimas por día, agrupando la serie en la zona del lugar.
  const byDay = new Map<string, { max: number; min: number; codes: number[]; rain: number }>();
  for (const s of steps) {
    const day = localIso(s.time, tz).slice(0, 10);
    const t = s.data.instant.details.air_temperature;
    const cur = byDay.get(day) ?? { max: t, min: t, codes: [], rain: 0 };
    cur.max = Math.max(cur.max, t);
    cur.min = Math.min(cur.min, t);
    cur.codes.push(wmoFromSymbol(symbolOf(s)));
    cur.rain = Math.max(cur.rain, rainOf(s));
    byDay.set(day, cur);
  }
  const days = [...byDay.entries()].sort((a, b) => a[0].localeCompare(b[0])).slice(0, 6);
  const daily = {
    time: days.map(([d]) => d),
    temperature_2m_max: days.map(([, v]) => v.max),
    temperature_2m_min: days.map(([, v]) => v.min),
    // El código del día es el peor del día (el que más se nota).
    weather_code: days.map(([, v]) => v.codes.reduce((a, b) => (b > a ? b : a), 0)),
    precipitation_probability_max: days.map(([, v]) => v.rain),
    sunrise: [] as string[],
    sunset: [] as string[],
  };

  const d = first.data.instant.details;
  return {
    current: {
      temperature_2m: d.air_temperature,
      relative_humidity_2m: d.relative_humidity,
      weather_code: wmoFromSymbol(symbolOf(first)),
      apparent_temperature: d.air_temperature,  // met.no no da sensación térmica
      wind_speed_10m: d.wind_speed * 3.6,       // m/s -> km/h
    },
    hourly,
    daily,
  };
}
