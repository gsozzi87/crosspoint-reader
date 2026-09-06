// Idiomas del aparato: es, en, zh, fr, de, pt, ru. El aparato manda el que
// tiene en Settings (?lang=xx o "lang" en el JSON); todo lo que vuelve a la
// pantalla o al parlante sale en ese idioma, y la transcripción escucha en él.
export type Lang = "es" | "en" | "zh" | "fr" | "de" | "pt" | "ru";
export const LANGS: Lang[] = ["es", "en", "zh", "fr", "de", "pt", "ru"];

export function normalizeLang(raw: string | null | undefined): Lang {
  const code = (raw ?? "").toLowerCase().slice(0, 2) as Lang;
  return LANGS.includes(code) ? code : "es";
}

// Cómo se le pide a Claude que hable.
export const LANGUAGE_NAME: Record<Lang, string> = {
  es: "español rioplatense, informal",
  en: "English",
  zh: "简体中文 (Mandarin Chinese, Simplified)",
  fr: "français",
  de: "Deutsch",
  pt: "português",
  ru: "русский",
};

// Idioma de destino del traductor cuando el usuario no lo dice.
export function defaultTranslateTarget(lang: Lang): string {
  return lang === "en" ? "español" : "English";
}

// Texto corto del clima por código WMO de Open-Meteo.
const WEATHER: Record<Lang, string[]> = {
  //          clear, partly, cloudy, fog, drizzle, rain, snow, showers, storm
  es: ["Despejado", "Algo nublado", "Nublado", "Niebla", "Llovizna", "Lluvia", "Nieve", "Chaparrones", "Tormenta"],
  en: ["Clear", "Partly cloudy", "Cloudy", "Fog", "Drizzle", "Rain", "Snow", "Showers", "Thunderstorm"],
  zh: ["晴", "少云", "多云", "雾", "毛毛雨", "雨", "雪", "阵雨", "雷暴"],
  fr: ["Dégagé", "Peu nuageux", "Nuageux", "Brouillard", "Bruine", "Pluie", "Neige", "Averses", "Orage"],
  de: ["Klar", "Leicht bewölkt", "Bewölkt", "Nebel", "Nieselregen", "Regen", "Schnee", "Schauer", "Gewitter"],
  pt: ["Céu limpo", "Parcialmente nublado", "Nublado", "Névoa", "Chuvisco", "Chuva", "Neve", "Pancadas", "Tempestade"],
  ru: ["Ясно", "Малооблачно", "Облачно", "Туман", "Морось", "Дождь", "Снег", "Ливни", "Гроза"],
};

export function describeWeather(code: number, lang: Lang): string {
  const w = WEATHER[lang];
  if (code === 0) return w[0];
  if (code <= 2) return w[1];
  if (code === 3) return w[2];
  if (code <= 48) return w[3];
  if (code <= 57) return w[4];
  if (code <= 67) return w[5];
  if (code <= 77) return w[6];
  if (code <= 82) return w[7];
  if (code <= 86) return w[6];
  return w[8];
}

// Etiquetas cortas del widget: máxima, mínima, humedad; hoy, mañana.
export const LABELS: Record<Lang, { max: string; min: string; hum: string; today: string; tomorrow: string }> = {
  es: { max: "Máx", min: "Mín", hum: "Hum", today: "hoy", tomorrow: "mañana" },
  en: { max: "High", min: "Low", hum: "Hum", today: "today", tomorrow: "tomorrow" },
  zh: { max: "最高", min: "最低", hum: "湿度", today: "今天", tomorrow: "明天" },
  fr: { max: "Max", min: "Min", hum: "Hum", today: "aujourd'hui", tomorrow: "demain" },
  de: { max: "Max", min: "Min", hum: "Feuchte", today: "heute", tomorrow: "morgen" },
  pt: { max: "Máx", min: "Mín", hum: "Umid", today: "hoje", tomorrow: "amanhã" },
  ru: { max: "Макс", min: "Мин", hum: "Влажн", today: "сегодня", tomorrow: "завтра" },
};

// Frase del día, una lista chica por idioma (Fase 3 la trae del servidor con más variedad).
export const QUOTES: Record<Lang, string[]> = {
  es: [
    "Lo que no se empieza hoy nunca se termina mañana.",
    "Un libro es un sueño que tenés en las manos.",
    "Hacé lo que puedas, con lo que tengas, donde estés.",
    "La paciencia es amarga, pero su fruto es dulce.",
    "Leer es viajar sin moverse.",
    "Cada día es una página nueva.",
    "Quien tiene un porqué soporta casi cualquier cómo.",
  ],
  en: [
    "What is not started today is never finished tomorrow.",
    "A book is a dream you hold in your hands.",
    "Do what you can, with what you have, where you are.",
    "Patience is bitter, but its fruit is sweet.",
    "Reading is travelling without moving.",
    "Every day is a new page.",
    "He who has a why can bear almost any how.",
  ],
  zh: ["今日事，今日毕。", "书是握在手中的梦。", "尽己所能，就地取材。", "耐心是苦的，果实是甜的。", "读书是不动身的旅行。", "每一天都是新的一页。", "知道为何而活的人，几乎能承受任何处境。"],
  fr: [
    "Ce qui n'est pas commencé aujourd'hui ne sera jamais fini demain.",
    "Un livre est un rêve que l'on tient dans les mains.",
    "Fais ce que tu peux, avec ce que tu as, là où tu es.",
    "La patience est amère, mais son fruit est doux.",
    "Lire, c'est voyager sans bouger.",
    "Chaque jour est une page nouvelle.",
    "Qui a un pourquoi supporte presque n'importe quel comment.",
  ],
  de: [
    "Was heute nicht begonnen wird, ist morgen nie fertig.",
    "Ein Buch ist ein Traum, den man in den Händen hält.",
    "Tu, was du kannst, mit dem, was du hast, wo du bist.",
    "Geduld ist bitter, aber ihre Frucht ist süß.",
    "Lesen ist Reisen, ohne sich zu bewegen.",
    "Jeder Tag ist eine neue Seite.",
    "Wer ein Warum hat, erträgt fast jedes Wie.",
  ],
  pt: [
    "O que não se começa hoje nunca se termina amanhã.",
    "Um livro é um sonho que você segura nas mãos.",
    "Faça o que puder, com o que tiver, onde estiver.",
    "A paciência é amarga, mas seu fruto é doce.",
    "Ler é viajar sem sair do lugar.",
    "Cada dia é uma página nova.",
    "Quem tem um porquê suporta quase qualquer como.",
  ],
  ru: [
    "Что не начато сегодня, не будет закончено завтра.",
    "Книга — это мечта, которую держишь в руках.",
    "Делай, что можешь, с тем, что имеешь, там, где ты есть.",
    "Терпение горько, но плод его сладок.",
    "Читать — значит путешествовать, не двигаясь с места.",
    "Каждый день — новая страница.",
    "У кого есть зачем, тот выдержит почти любое как.",
  ],
};
