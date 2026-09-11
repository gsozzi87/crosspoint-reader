#include "UnitsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ListStyle.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/SevenSegment.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "input/MotionInput.h"
#include "voice/SpeechToText.h"

// =============================================================================
//  Las unidades
// =============================================================================
//
// Todo el conversor son estas tablas y tres cuentas. Cada familia tiene una
// unidad base (metro, gramo, °C, ml, m², m/s) y cada unidad dice cuánto vale en
// esa base; la temperatura además lleva desplazamiento, porque 0 °C no es 0 °F.
//
// La familia de cocina es la única distinta y es la que más se usa: ahí la base
// son GRAMOS y las medidas de volumen (taza, cucharada) pasan a gramos con la
// densidad del ingrediente elegido, que es justamente lo que uno no se acuerda
// ("una taza de harina son 120 g, pero una de azúcar son 200").
namespace {
constexpr const char* TAG = "UNITS";
constexpr const char* STATE_PATH = "/.crosspoint/units.txt";

enum UnitKind : uint8_t {
  Linear,      // factor y desplazamiento contra la base de la familia
  CookVolume,  // cocina: el factor son mililitros, y a gramos se pasa con la densidad
  CookMass,    // cocina: el factor ya son gramos
};

struct Unit {
  const char* key;         // estable, es lo que se guarda en la SD
  StrId name;              // nombre traducido
  const char* symbol;      // "" cuando no hay símbolo internacional (taza, cucharada)
  double factor;
  double offset;
  UnitKind kind;
  int8_t partner;          // la unidad "hermana": la del resultado grande
  const char* const* words;  // palabras clave del dictado, normalizadas, NULL al final
};

struct Family {
  const char* key;
  StrId name;
  const Unit* units;
  uint8_t count;
  bool cooking;
};

// --- palabras del dictado ----------------------------------------------------
//
// Van SIN acentos y en minúscula porque el texto que llega del servidor se
// normaliza igual antes de compararlo (ver `normalize`). Una palabra de más de
// dos letras compara por prefijo, así que "milla" agarra "millas" y "kilometr"
// agarra kilómetros, kilometro, kilometres y quilômetros; las de una o dos
// letras ("m", "kg") tienen que coincidir enteras o cualquier palabra las
// contendría.
const char* const W_MM[] = {"mm", "milimetr", "millimet", "миллиметр", nullptr};
const char* const W_CM[] = {"cm", "centimetr", "centimet", "zentimet", "сантиметр", nullptr};
const char* const W_M[] = {"m", "metro", "meter", "metre", "метр", nullptr};
const char* const W_KM[] = {"km", "kilometr", "kilomet", "quilomet", "километр", nullptr};
const char* const W_IN[] = {"pulgada", "inch", "pouce", "zoll", "polegada", "дюйм", nullptr};
const char* const W_FT[] = {"pie", "pes", "foot", "feet", "pied", "fuss", "фут", nullptr};
const char* const W_YD[] = {"yarda", "yard", "jarda", "ярд", nullptr};
// "mille" (mil en francés) queda afuera a propósito: es el 1000, no la milla.
const char* const W_MI[] = {"milla", "mile", "meile", "milha", "миля", nullptr};

const char* const W_G[] = {"g", "gram", "грамм", nullptr};
const char* const W_KG[] = {"kg", "kilogram", "quilogram", "kilo", "килограмм", nullptr};
const char* const W_T[] = {"t", "tonelada", "tonne", "тонна", nullptr};
const char* const W_OZ[] = {"oz", "onza", "onca", "ounce", "unze", "унци", nullptr};
const char* const W_LB[] = {"lb", "libra", "pound", "livre", "pfund", "фунт", nullptr};

const char* const W_C[] = {"celsius", "centigrad", "grado", "grau", "grad", "degre", "degree", "градус", nullptr};
const char* const W_F[] = {"fahrenheit", "фаренгейт", nullptr};
const char* const W_K[] = {"kelvin", "кельвин", nullptr};

const char* const W_ML[] = {"ml", "mililitr", "millilit", "миллилитр", nullptr};
const char* const W_L[] = {"l", "litro", "liter", "litre", "литр", nullptr};
const char* const W_CUP[] = {"taza", "cup", "tasse", "xicara", "chavena", "чашк", nullptr};
// La cuchara genérica cae en cucharada; el modificador ("de café", "de chá",
// "чайная") la baja a cucharadita más abajo.
const char* const W_TBSP[] = {"cucharada", "cda",   "tablespoon", "tbsp",  "essloffel",
                              "cuchara",   "cuiller", "colher",   "ложк",  nullptr};
const char* const W_TSP[] = {"cucharadita", "cdta", "teaspoon", "tsp", "teeloffel", nullptr};
const char* const W_FLOZ[] = {"floz", nullptr};
const char* const W_PT[] = {"pinta", "pint", "пинт", nullptr};
const char* const W_GAL[] = {"galon", "gallon", "gallone", "галлон", nullptr};

// Las de superficie se dicen casi siempre como "metros cuadrados": eso lo
// resuelve el modificador de abajo, que remapea la unidad de longitud a la de
// superficie. Acá quedan las que tienen nombre propio.
const char* const W_CM2[] = {nullptr};
const char* const W_M2[] = {nullptr};
const char* const W_HA[] = {"ha", "hectarea", "hectare", "hektar", "гектар", nullptr};
const char* const W_KM2[] = {nullptr};
const char* const W_FT2[] = {nullptr};
const char* const W_AC[] = {"acre", "акр", nullptr};

const char* const W_KMH[] = {"kmh", "kph", nullptr};
const char* const W_MS[] = {nullptr};
const char* const W_MPH[] = {"mph", nullptr};
const char* const W_KN[] = {"nudo", "knot", "knoten", "noeud", "узел", "узл", nullptr};

const Unit LENGTH_UNITS[] = {
    {"mm", StrId::STR_UNITS_U_MM, "mm", 0.001, 0, Linear, 4, W_MM},
    {"cm", StrId::STR_UNITS_U_CM, "cm", 0.01, 0, Linear, 4, W_CM},
    {"m", StrId::STR_UNITS_U_M, "m", 1.0, 0, Linear, 5, W_M},
    {"km", StrId::STR_UNITS_U_KM, "km", 1000.0, 0, Linear, 7, W_KM},
    {"in", StrId::STR_UNITS_U_IN, "in", 0.0254, 0, Linear, 1, W_IN},
    {"ft", StrId::STR_UNITS_U_FT, "ft", 0.3048, 0, Linear, 2, W_FT},
    {"yd", StrId::STR_UNITS_U_YD, "yd", 0.9144, 0, Linear, 2, W_YD},
    {"mi", StrId::STR_UNITS_U_MI, "mi", 1609.344, 0, Linear, 3, W_MI},
};

const Unit MASS_UNITS[] = {
    {"g", StrId::STR_UNITS_U_G, "g", 1.0, 0, Linear, 3, W_G},
    {"kg", StrId::STR_UNITS_U_KG, "kg", 1000.0, 0, Linear, 4, W_KG},
    {"t", StrId::STR_UNITS_U_T, "t", 1000000.0, 0, Linear, 1, W_T},
    {"oz", StrId::STR_UNITS_U_OZ, "oz", 28.349523125, 0, Linear, 0, W_OZ},
    {"lb", StrId::STR_UNITS_U_LB, "lb", 453.59237, 0, Linear, 1, W_LB},
};

// Base °C. Fahrenheit: base = F·5/9 − 32·5/9, y −32·5/9 = −160/9.
const Unit TEMP_UNITS[] = {
    {"c", StrId::STR_UNITS_U_C, "°C", 1.0, 0.0, Linear, 1, W_C},
    {"f", StrId::STR_UNITS_U_F, "°F", 5.0 / 9.0, -160.0 / 9.0, Linear, 0, W_F},
    {"k", StrId::STR_UNITS_U_K, "K", 1.0, -273.15, Linear, 0, W_K},
};

const Unit VOLUME_UNITS[] = {
    {"ml", StrId::STR_UNITS_U_ML, "ml", 1.0, 0, Linear, 5, W_ML},
    {"l", StrId::STR_UNITS_U_L, "l", 1000.0, 0, Linear, 7, W_L},
    {"cup", StrId::STR_UNITS_U_CUP, "", 236.5882365, 0, Linear, 0, W_CUP},
    {"tbsp", StrId::STR_UNITS_U_TBSP, "", 14.78676478, 0, Linear, 0, W_TBSP},
    {"tsp", StrId::STR_UNITS_U_TSP, "", 4.92892159, 0, Linear, 0, W_TSP},
    {"floz", StrId::STR_UNITS_U_FLOZ, "", 29.5735295625, 0, Linear, 0, W_FLOZ},
    {"pt", StrId::STR_UNITS_U_PT, "pt", 473.176473, 0, Linear, 1, W_PT},
    {"gal", StrId::STR_UNITS_U_GAL, "gal", 3785.411784, 0, Linear, 1, W_GAL},
};

const Unit AREA_UNITS[] = {
    {"cm2", StrId::STR_UNITS_U_CM2, "cm²", 0.0001, 0, Linear, 1, W_CM2},
    {"m2", StrId::STR_UNITS_U_M2, "m²", 1.0, 0, Linear, 4, W_M2},
    {"ha", StrId::STR_UNITS_U_HA, "ha", 10000.0, 0, Linear, 5, W_HA},
    {"km2", StrId::STR_UNITS_U_KM2, "km²", 1000000.0, 0, Linear, 2, W_KM2},
    {"ft2", StrId::STR_UNITS_U_FT2, "ft²", 0.09290304, 0, Linear, 1, W_FT2},
    {"ac", StrId::STR_UNITS_U_AC, "ac", 4046.8564224, 0, Linear, 2, W_AC},
};

const Unit SPEED_UNITS[] = {
    {"kmh", StrId::STR_UNITS_U_KMH, "km/h", 1.0 / 3.6, 0, Linear, 2, W_KMH},
    {"ms", StrId::STR_UNITS_U_MS, "m/s", 1.0, 0, Linear, 0, W_MS},
    {"mph", StrId::STR_UNITS_U_MPH, "mph", 0.44704, 0, Linear, 0, W_MPH},
    {"kn", StrId::STR_UNITS_U_KN, "kn", 0.5144444444, 0, Linear, 0, W_KN},
};

// Cocina: base GRAMOS. Las de volumen llevan sus mililitros y se pasan a gramos
// con la densidad del ingrediente.
const Unit KITCHEN_UNITS[] = {
    {"cup", StrId::STR_UNITS_U_CUP, "", 236.5882365, 0, CookVolume, 4, W_CUP},
    {"tbsp", StrId::STR_UNITS_U_TBSP, "", 14.78676478, 0, CookVolume, 4, W_TBSP},
    {"tsp", StrId::STR_UNITS_U_TSP, "", 4.92892159, 0, CookVolume, 4, W_TSP},
    {"ml", StrId::STR_UNITS_U_ML, "ml", 1.0, 0, CookVolume, 4, W_ML},
    {"g", StrId::STR_UNITS_U_G, "g", 1.0, 0, CookMass, 0, W_G},
    {"oz", StrId::STR_UNITS_U_OZ, "oz", 28.349523125, 0, CookMass, 4, W_OZ},
    {"lb", StrId::STR_UNITS_U_LB, "lb", 453.59237, 0, CookMass, 4, W_LB},
};

const Family FAMILIES[] = {
    {"length", StrId::STR_UNITS_FAM_LENGTH, LENGTH_UNITS, 8, false},
    {"mass", StrId::STR_UNITS_FAM_MASS, MASS_UNITS, 5, false},
    {"temp", StrId::STR_UNITS_FAM_TEMP, TEMP_UNITS, 3, false},
    {"volume", StrId::STR_UNITS_FAM_VOLUME, VOLUME_UNITS, 8, false},
    {"area", StrId::STR_UNITS_FAM_AREA, AREA_UNITS, 6, false},
    {"speed", StrId::STR_UNITS_FAM_SPEED, SPEED_UNITS, 4, false},
    {"kitchen", StrId::STR_UNITS_FAM_KITCHEN, KITCHEN_UNITS, 7, true},
};
constexpr int FAMILY_COUNT = sizeof(FAMILIES) / sizeof(FAMILIES[0]);
constexpr int FAMILY_KITCHEN = 6;
constexpr int FAMILY_TEMP = 2;
constexpr int FAMILY_AREA = 4;
constexpr int FAMILY_SPEED = 5;
constexpr int FAMILY_VOLUME = 3;

// Ingredientes de la familia de cocina. Los gramos por mililitro salen de las
// equivalencias de taza que usa cualquier recetario: 120 g de harina, 200 g de
// azúcar y 227 g de mantequilla por taza de 236,6 ml.
const char* const W_FLOUR[] = {"harina", "flour", "farine", "mehl", "farinha", "мук", nullptr};
const char* const W_SUGAR[] = {"azucar", "sugar", "sucre", "zucker", "acucar", "сахар", nullptr};
const char* const W_BUTTER[] = {"mantequilla", "manteca", "butter", "beurre", "manteiga", "масл", nullptr};
const char* const W_WATER[] = {"agua",  "water", "eau",  "wasser", "leche", "milk",
                               "lait",  "milch", "leite", "вод",   "молок", nullptr};

struct Ingredient {
  StrId name;
  double gramsPerMl;
  const char* const* words;
};
const Ingredient INGREDIENTS[] = {
    {StrId::STR_UNITS_ING_FLOUR, 120.0 / 236.5882365, W_FLOUR},
    {StrId::STR_UNITS_ING_SUGAR, 200.0 / 236.5882365, W_SUGAR},
    {StrId::STR_UNITS_ING_BUTTER, 227.0 / 236.5882365, W_BUTTER},
    {StrId::STR_UNITS_ING_WATER, 1.0, W_WATER},
};
constexpr int INGREDIENT_COUNT = sizeof(INGREDIENTS) / sizeof(INGREDIENTS[0]);

// Modificadores: palabras que no son la unidad pero la cambian.
const char* const W_SQUARE[] = {"cuadrad", "square", "carre", "quadrat", "квадрат", nullptr};
const char* const W_HOUR[] = {"hora", "hour", "heure", "stunde", "час", "h", nullptr};
const char* const W_SECOND[] = {"segundo", "second", "seconde", "sekunde", "секунд", "s", nullptr};
const char* const W_LIQUID[] = {"liquid", "fluid", "fluide", "flussig", "жидк", nullptr};
const char* const W_SPOON_SMALL[] = {"cafe", "cha", "te", "tea", "tee", "postre", "чайн", nullptr};
const char* const W_SPOON_BIG[] = {"sopa", "soupe", "table", "ess", "столов", nullptr};
// "doce grados bajo cero", "minus zwölf Grad": sólo cuentan en temperatura.
const char* const W_BELOW_ZERO[] = {"menos", "minus", "moins", "negativ", "bajo",
                                    "abaixo", "unter", "минус", nullptr};

// --- números dichos con letras ----------------------------------------------
struct NumberWord {
  const char* word;
  double value;
};
// Se comparan ENTERAS (no por prefijo): "seis" no puede agarrar "seiscientos".
const NumberWord NUMBER_WORDS[] = {
    // español
    {"cero", 0},        {"un", 1},          {"uno", 1},        {"una", 1},        {"dos", 2},
    {"tres", 3},        {"cuatro", 4},      {"cinco", 5},      {"seis", 6},       {"siete", 7},
    {"ocho", 8},        {"nueve", 9},       {"diez", 10},      {"once", 11},      {"doce", 12},
    {"trece", 13},      {"catorce", 14},    {"quince", 15},    {"dieciseis", 16}, {"diecisiete", 17},
    {"dieciocho", 18},  {"diecinueve", 19}, {"veinte", 20},    {"veintiuno", 21}, {"veintiun", 21},
    {"veintidos", 22},  {"veintitres", 23}, {"veinticuatro", 24}, {"veinticinco", 25}, {"veintiseis", 26},
    {"veintisiete", 27}, {"veintiocho", 28}, {"veintinueve", 29}, {"treinta", 30}, {"cuarenta", 40},
    {"cincuenta", 50},  {"sesenta", 60},    {"setenta", 70},   {"ochenta", 80},   {"noventa", 90},
    {"cien", 100},      {"ciento", 100},    {"doscientos", 200}, {"trescientos", 300}, {"cuatrocientos", 400},
    {"quinientos", 500}, {"seiscientos", 600}, {"setecientos", 700}, {"ochocientos", 800}, {"novecientos", 900},
    {"mil", 1000},
    // inglés
    {"zero", 0},        {"one", 1},         {"two", 2},        {"three", 3},      {"four", 4},
    {"five", 5},        {"six", 6},         {"seven", 7},      {"eight", 8},      {"nine", 9},
    {"ten", 10},        {"eleven", 11},     {"twelve", 12},    {"thirteen", 13},  {"fourteen", 14},
    {"fifteen", 15},    {"sixteen", 16},    {"seventeen", 17}, {"eighteen", 18},  {"nineteen", 19},
    {"twenty", 20},     {"thirty", 30},     {"forty", 40},     {"fifty", 50},     {"sixty", 60},
    {"seventy", 70},    {"eighty", 80},     {"ninety", 90},    {"hundred", 100},  {"thousand", 1000},
    // francés
    {"deux", 2},        {"trois", 3},       {"quatre", 4},     {"cinq", 5},       {"sept", 7},
    {"huit", 8},        {"neuf", 9},        {"dix", 10},       {"onze", 11},      {"douze", 12},
    {"treize", 13},     {"quatorze", 14},   {"quinze", 15},    {"seize", 16},     {"vingt", 20},
    {"trente", 30},     {"quarante", 40},   {"cinquante", 50}, {"soixante", 60},  {"cent", 100},
    {"mille", 1000},
    // alemán
    {"null", 0},        {"eins", 1},        {"ein", 1},        {"eine", 1},       {"zwei", 2},
    {"drei", 3},        {"vier", 4},        {"funf", 5},       {"sechs", 6},      {"sieben", 7},
    {"acht", 8},        {"neun", 9},        {"zehn", 10},      {"elf", 11},       {"zwolf", 12},
    {"dreizehn", 13},   {"vierzehn", 14},   {"funfzehn", 15},  {"sechzehn", 16},  {"siebzehn", 17},
    {"achtzehn", 18},   {"neunzehn", 19},   {"zwanzig", 20},   {"dreissig", 30},  {"vierzig", 40},
    {"funfzig", 50},    {"sechzig", 60},    {"siebzig", 70},   {"achtzig", 80},   {"neunzig", 90},
    {"hundert", 100},   {"tausend", 1000},
    // portugués
    {"um", 1},          {"uma", 1},         {"dois", 2},       {"duas", 2},       {"quatro", 4},
    {"sete", 7},        {"oito", 8},        {"nove", 9},       {"dez", 10},       {"doze", 12},
    {"treze", 13},      {"quatorze", 14},   {"dezesseis", 16}, {"dezasseis", 16}, {"dezessete", 17},
    {"dezassete", 17},  {"dezoito", 18},    {"dezenove", 19},  {"dezanove", 19},  {"vinte", 20},
    {"trinta", 30},     {"sessenta", 60},   {"oitenta", 80},   {"cem", 100},
    // ruso
    {"ноль", 0},        {"один", 1},        {"одна", 1},       {"два", 2},        {"две", 2},
    {"три", 3},         {"четыре", 4},      {"пять", 5},       {"шесть", 6},      {"семь", 7},
    {"восемь", 8},      {"девять", 9},      {"десять", 10},    {"одиннадцать", 11}, {"двенадцать", 12},
    {"тринадцать", 13}, {"четырнадцать", 14}, {"пятнадцать", 15}, {"шестнадцать", 16}, {"семнадцать", 17},
    {"восемнадцать", 18}, {"девятнадцать", 19}, {"двадцать", 20}, {"тридцать", 30}, {"сорок", 40},
    {"пятьдесят", 50},  {"шестьдесят", 60}, {"семьдесят", 70}, {"восемьдесят", 80}, {"девяносто", 90},
    {"сто", 100},       {"тысяча", 1000},   {"тысячи", 1000},
};
constexpr int NUMBER_WORD_COUNT = sizeof(NUMBER_WORDS) / sizeof(NUMBER_WORDS[0]);

const char* const W_HALF[] = {"medio", "media", "half", "demi", "halb", "halbe",
                              "meio",  "meia", "пол",  "половина", nullptr};

// --- normalización ------------------------------------------------------------
// El texto llega del servidor con acentos y mayúsculas, y en ruso en cirílico.
// Las tablas de arriba están en minúscula y sin acentos: acá se pliega todo a
// eso para que "Três Xícaras" y "tres xicaras" comparen igual.
const char* latinFold(uint32_t cp) {
  if (cp == 0xDF) return "ss";                             // ß
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;  // mayúsculas latinas
  switch (cp) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
    case 0xE6: return "ae";
    case 0xE7: return "c";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
    case 0xF1: return "n";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: return "o";
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
    case 0xFD: case 0xFF: return "y";
    case 0x153: return "oe";  // œ
    default: return nullptr;
  }
}

std::string normalize(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) {
      out += static_cast<char>(tolower(c));
      i++;
      continue;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
      const unsigned char d = static_cast<unsigned char>(in[i + 1]);
      const uint32_t cp = ((c & 0x1Fu) << 6) | (d & 0x3Fu);
      if (const char* folded = latinFold(cp)) {
        out += folded;
        i += 2;
        continue;
      }
      uint32_t lower = cp;
      if (cp >= 0x410 && cp <= 0x42F) lower = cp + 0x20;  // А-Я -> а-я
      if (cp == 0x401) lower = 0x451;                     // Ё -> ё
      out += static_cast<char>(0xC0 | (lower >> 6));
      out += static_cast<char>(0x80 | (lower & 0x3F));
      i += 2;
      continue;
    }
    out += static_cast<char>(c);
    i++;
  }
  return out;
}

bool isWordByte(const unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

std::vector<std::string> tokenize(const std::string& norm) {
  std::vector<std::string> tokens;
  std::string current;
  for (size_t i = 0; i < norm.size(); i++) {
    const unsigned char c = static_cast<unsigned char>(norm[i]);
    const bool decimalMark = (c == '.' || c == ',') && !current.empty() && isdigit(static_cast<unsigned char>(current.back())) &&
                             i + 1 < norm.size() && isdigit(static_cast<unsigned char>(norm[i + 1]));
    if (isWordByte(c) || decimalMark) {
      current += static_cast<char>(c);
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

// Una palabra clave engancha con el token si son iguales (claves de una o dos
// letras) o si el token empieza con ella (todo lo demás, para que los plurales
// y las declinaciones entren solas).
bool tokenMatches(const std::string& token, const char* keyword) {
  const size_t len = strlen(keyword);
  if (len == 0) return false;
  if (len <= 2) return token == keyword;
  return token.size() >= len && token.compare(0, len, keyword) == 0;
}

int matchLength(const std::string& token, const char* const* words) {
  int best = 0;
  for (const char* const* w = words; *w; w++) {
    if (tokenMatches(token, *w) && static_cast<int>(strlen(*w)) > best) best = static_cast<int>(strlen(*w));
  }
  return best;
}

bool anyMatch(const std::vector<std::string>& tokens, const char* const* words) {
  for (const auto& t : tokens) {
    if (matchLength(t, words) > 0) return true;
  }
  return false;
}

// ¿Está la palabra cerca del token de la unidad? Tres tokens a cada lado
// alcanzan para "metros por hora" o "square feet" y no cruzan de frase.
bool nearWord(const std::vector<std::string>& tokens, const int index, const char* const* words) {
  const int from = std::max(0, index - 3);
  const int to = std::min(static_cast<int>(tokens.size()) - 1, index + 3);
  for (int i = from; i <= to; i++) {
    if (i == index) continue;
    if (matchLength(tokens[i], words) > 0) return true;
  }
  return false;
}

int findUnit(const int family, const char* key) {
  const Family& f = FAMILIES[family];
  for (int i = 0; i < f.count; i++) {
    if (strcmp(f.units[i].key, key) == 0) return i;
  }
  return -1;
}

double numberFromToken(const std::string& token, bool& ok) {
  ok = false;
  if (token.empty() || !isdigit(static_cast<unsigned char>(token[0]))) return 0;
  std::string copy = token;
  for (char& c : copy) {
    if (c == ',') c = '.';
  }
  char* end = nullptr;
  const double v = strtod(copy.c_str(), &end);
  if (end == copy.c_str()) return 0;
  ok = true;
  return v;
}

bool numberWordValue(const std::string& token, double& value) {
  for (int i = 0; i < NUMBER_WORD_COUNT; i++) {
    if (token == NUMBER_WORDS[i].word) {
      value = NUMBER_WORDS[i].value;
      return true;
    }
  }
  // Los compuestos alemanes ("funfundzwanzig") no entran en ninguna tabla
  // razonable: se parten por el "und" y se suman las dos mitades.
  const size_t und = token.find("und");
  if (und != std::string::npos && und > 0 && und + 3 < token.size()) {
    double a = 0, b = 0;
    if (numberWordValue(token.substr(0, und), a) && numberWordValue(token.substr(und + 3), b)) {
      value = a + b;
      return true;
    }
  }
  return false;
}

struct Spoken {
  bool hasValue = false;
  double value = 0;
  bool hasUnit = false;
  int family = -1;
  int unit = -1;
  int ingredient = -1;
};

// Lo dicho, ya transcripto, convertido en número + unidad. Devuelve false
// cuando no encontró NADA que aplicar (ahí la pantalla lo dice y deja el
// carrusel como estaba).
bool parseSpoken(const std::string& text, const int currentFamily, Spoken& out) {
  const std::vector<std::string> tokens = tokenize(normalize(text));
  if (tokens.empty()) return false;

  std::vector<bool> consumed(tokens.size(), false);

  // 1. El número. Si lo dictó con cifras, gana la cifra.
  for (size_t i = 0; i < tokens.size(); i++) {
    bool ok = false;
    const double v = numberFromToken(tokens[i], ok);
    if (!ok) continue;
    out.hasValue = true;
    out.value = v;
    consumed[i] = true;
    break;
  }
  if (!out.hasValue) {
    double total = 0, current = 0;
    bool any = false;
    for (size_t i = 0; i < tokens.size(); i++) {
      double v = 0;
      if (numberWordValue(tokens[i], v)) {
        consumed[i] = true;
        any = true;
        if (v == 100) {
          current = (current == 0 ? 1 : current) * 100;
        } else if (v == 1000) {
          total += (current == 0 ? 1 : current) * 1000;
          current = 0;
        } else {
          current += v;
        }
        continue;
      }
      if (matchLength(tokens[i], W_HALF) > 0) {
        consumed[i] = true;
        any = true;
        current += 0.5;
      }
    }
    if (any) {
      out.hasValue = true;
      out.value = total + current;
    }
  }

  // 2. El ingrediente (sólo lo usa la cocina, pero se busca siempre: es lo que
  //    decide que "tres tazas de harina" es cocina y no volumen).
  for (int i = 0; i < INGREDIENT_COUNT; i++) {
    if (anyMatch(tokens, INGREDIENTS[i].words)) {
      out.ingredient = i;
      break;
    }
  }

  // 3. La unidad: gana la palabra clave más larga, y a igual largo la familia
  //    en la que ya estaba el usuario.
  int bestScore = 0;
  int bestToken = -1;
  for (size_t i = 0; i < tokens.size(); i++) {
    if (consumed[i]) continue;
    for (int f = 0; f < FAMILY_COUNT; f++) {
      for (int u = 0; u < FAMILIES[f].count; u++) {
        const int len = matchLength(tokens[i], FAMILIES[f].units[u].words);
        if (len == 0) continue;
        const int score = len * 2 + (f == currentFamily ? 1 : 0);
        if (score <= bestScore) continue;
        bestScore = score;
        bestToken = static_cast<int>(i);
        out.family = f;
        out.unit = u;
      }
    }
  }

  if (out.family >= 0) {
    out.hasUnit = true;
    const char* key = FAMILIES[out.family].units[out.unit].key;
    // "metros cuadrados" / "square feet": la misma palabra, otra familia.
    if (nearWord(tokens, bestToken, W_SQUARE)) {
      const std::string squared = std::string(key) + "2";
      const int idx = findUnit(FAMILY_AREA, squared.c_str());
      if (idx >= 0) {
        out.family = FAMILY_AREA;
        out.unit = idx;
        key = FAMILIES[out.family].units[out.unit].key;
      }
    }
    const auto remap = [&out](const int family, const char* target) {
      const int idx = findUnit(family, target);
      if (idx < 0) return;
      out.family = family;
      out.unit = idx;
    };
    // "kilómetros por hora", "millas por hora", "metros por segundo".
    if (strcmp(key, "km") == 0 && nearWord(tokens, bestToken, W_HOUR)) {
      remap(FAMILY_SPEED, "kmh");
    } else if (strcmp(key, "mi") == 0 && nearWord(tokens, bestToken, W_HOUR)) {
      remap(FAMILY_SPEED, "mph");
    } else if (strcmp(key, "m") == 0 && nearWord(tokens, bestToken, W_SECOND)) {
      remap(FAMILY_SPEED, "ms");
    } else if (strcmp(key, "oz") == 0 && nearWord(tokens, bestToken, W_LIQUID)) {
      // "onza líquida" es de volumen; la onza a secas es de peso.
      remap(FAMILY_VOLUME, "floz");
    } else if (strcmp(key, "tbsp") == 0 && nearWord(tokens, bestToken, W_SPOON_SMALL) &&
               !nearWord(tokens, bestToken, W_SPOON_BIG)) {
      // "cuillère à café", "colher de chá", "чайная ложка".
      remap(out.family, "tsp");
    }
    key = FAMILIES[out.family].units[out.unit].key;
    // Con ingrediente a la vista, la medida es de cocina: ahí una taza son
    // gramos de algo, que es lo que se quiere saber.
    if (out.ingredient >= 0) remap(FAMILY_KITCHEN, key);
  }

  // Sin unidad dictada la familia sigue siendo la que el usuario tenía puesta
  // (es lo que hace applySpoken), así que "menos doce" en Temperatura también
  // tiene que quedar bajo cero.
  const int signFamily = out.hasUnit ? out.family : currentFamily;
  if (out.hasValue && signFamily == FAMILY_TEMP && anyMatch(tokens, W_BELOW_ZERO)) out.value = -out.value;

  return out.hasValue || out.hasUnit;
}

// --- números en pantalla -------------------------------------------------------
const char* decimalSeparator() { return I18N.getLanguage() == Language::EN ? "." : ","; }

// Los decimales según el tamaño: 3 km no necesita tres decimales y 0,004 l sin
// ellos es un cero pelado. Los miles se agrupan con un espacio, que es lo que
// hace legible "1 609 344".
std::string formatNumber(const double value) {
  const double a = fabs(value);
  int decimals = 2;
  if (a >= 1000) decimals = 0;
  else if (a >= 100) decimals = 1;
  else if (a >= 1) decimals = 2;
  else if (a >= 0.01) decimals = 3;
  else decimals = 5;
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  const size_t dot = s.find('.');
  const size_t intEnd = dot == std::string::npos ? s.size() : dot;
  const size_t intStart = (!s.empty() && s[0] == '-') ? 1 : 0;
  if (intEnd - intStart > 4) {
    for (size_t pos = intEnd; pos > intStart + 3;) {
      pos -= 3;
      s.insert(pos, " ");
    }
  }
  if (dot != std::string::npos) {
    const size_t at = s.find('.');
    s.replace(at, 1, decimalSeparator());
  }
  return s;
}

// Un texto de dígitos con los segmentos de `SevenSegment.h`. Dibuja cifras, la
// coma decimal, el espacio de los miles y el menos de las temperaturas; si el
// número quedó tan largo que los dígitos saldrían como hilos, lo escribe con la
// fuente de título y listo. Devuelve el ancho que ocupó.
int drawSevenSegText(const GfxRenderer& r, const char* text, const int x, const int y, const int maxW, const int maxH,
                     const int maxDigitW) {
  const int n = static_cast<int>(strlen(text));
  if (n == 0) return 0;
  const auto glyphWidth = [](const char c, const int dw) {
    if (c == '.' || c == ',') return dw / 3;
    if (c == ' ') return dw / 3;
    return dw;
  };
  const auto totalWidth = [&](const int dw) {
    int w = 0;
    for (int i = 0; i < n; i++) {
      w += glyphWidth(text[i], dw);
      if (i + 1 < n) w += dw / 5;
    }
    return w;
  };
  int dw = maxDigitW;
  int width = totalWidth(dw);
  if (width > maxW) {
    dw = std::max(1, dw * maxW / width);
    width = totalWidth(dw);
    while (width > maxW && dw > 1) {
      dw--;
      width = totalWidth(dw);
    }
  }
  if (dw < 12) {  // ya no se lee como número: mejor la fuente de título
    const std::string fitted = r.truncatedText(UI_14_FONT_ID, text, maxW);
    r.drawText(UI_14_FONT_ID, x, y + std::max(0, (maxH - r.getLineHeight(UI_14_FONT_ID)) / 2), fitted.c_str());
    return r.getTextWidth(UI_14_FONT_ID, fitted.c_str());
  }
  const int dh = std::min(maxH, dw * 3 / 2);
  const int t = std::max(3, dw / 6);
  const int top = y + (maxH - dh) / 2;
  int cx = x;
  for (int i = 0; i < n; i++) {
    const char c = text[i];
    if (c >= '0' && c <= '9') {
      sevenseg::digit(r, c - '0', cx, top, dw, dh, t);
    } else if (c == '.' || c == ',') {
      r.fillRect(cx, top + dh - t, t, t, true);
    } else if (c == '-') {
      r.fillRect(cx, top + dh / 2 - t / 2, dw, t, true);
    }
    cx += glyphWidth(c, dw);
    if (i + 1 < n) cx += dw / 5;
  }
  return width;
}

}  // namespace

// =============================================================================
//  Ciclo de vida
// =============================================================================

void UnitsActivity::onEnter() {
  Activity::onEnter();
  loadState();
  rebuildSlots();
  cursor = 0;  // el cursor arranca en Decirlo: un OK y ya estás dictando
  requestUpdate();
}

void UnitsActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  saveState();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();  // el heap que dejó la sesión de WiFi no se recupera de otra forma
  }
}

// =============================================================================
//  Estado: familia, unidad y cantidad
// =============================================================================

void UnitsActivity::rebuildSlots() {
  slotCount = 0;
  slots[slotCount++] = {Field::Say, -1};
  slots[slotCount++] = {Field::Kind, -1};
  if (FAMILIES[familyIdx].cooking) slots[slotCount++] = {Field::Ingredient, -1};
  slots[slotCount++] = {Field::Unit, -1};
  if (familyIdx != FAMILY_TEMP) negative = false;  // sólo la temperatura baja de cero
  if (familyIdx == FAMILY_TEMP) slots[slotCount++] = {Field::Sign, -1};
  for (int i = 0; i < DIGITS; i++) slots[slotCount++] = {Field::Digit, static_cast<int8_t>(i)};
  if (cursor >= slotCount) cursor = slotCount - 1;
}

void UnitsActivity::moveCursor(const int delta) {
  if (slotCount <= 0) return;
  cursor = (cursor + delta % slotCount + slotCount) % slotCount;
  requestUpdate();
}

void UnitsActivity::selectFamily(const int newFamily, const int newUnit) {
  familyIdx = (newFamily % FAMILY_COUNT + FAMILY_COUNT) % FAMILY_COUNT;
  const int count = FAMILIES[familyIdx].count;
  unitIdx = (newUnit % count + count) % count;
  rebuildSlots();
}

void UnitsActivity::adjust(const int delta) {
  switch (slot().field) {
    case Field::Say:
      // El botón de dictar no tiene valor que mover, y una palanca muerta
      // confunde: acá corre el cursor, que es lo único que queda por hacer.
      moveCursor(delta > 0 ? -1 : 1);
      return;
    case Field::Kind: {
      const int next = (familyIdx + delta % FAMILY_COUNT + FAMILY_COUNT) % FAMILY_COUNT;
      selectFamily(next, 0);
      break;
    }
    case Field::Ingredient:
      ingredientIdx = (ingredientIdx + delta % INGREDIENT_COUNT + INGREDIENT_COUNT) % INGREDIENT_COUNT;
      break;
    case Field::Unit: {
      const int count = FAMILIES[familyIdx].count;
      unitIdx = (unitIdx + delta % count + count) % count;
      break;
    }
    case Field::Sign:
      negative = !negative;
      break;
    case Field::Digit: {
      const int i = slot().digit;
      if (i < 0 || i >= DIGITS) break;
      digits[i] = static_cast<uint8_t>((digits[i] + delta % 10 + 10) % 10);
      break;
    }
  }
  requestUpdate();
}

double UnitsActivity::amount() const {
  double whole = 0;
  for (int i = 0; i < INT_DIGITS; i++) whole = whole * 10 + digits[i];
  double frac = 0, scale = 1;
  for (int i = INT_DIGITS; i < DIGITS; i++) {
    scale /= 10;
    frac += digits[i] * scale;
  }
  const double value = whole + frac;
  return negative ? -value : value;
}

void UnitsActivity::setAmount(double value) {
  negative = value < 0 && familyIdx == FAMILY_TEMP;
  value = fabs(value);
  if (!(value > 0)) value = 0;  // NaN incluido
  if (value > 9999.99) value = 9999.99;
  long long scaled = llround(value * 100.0);
  for (int i = DIGITS - 1; i >= 0; i--) {
    digits[i] = static_cast<uint8_t>(scaled % 10);
    scaled /= 10;
  }
}

double UnitsActivity::density() const {
  const int i = (ingredientIdx % INGREDIENT_COUNT + INGREDIENT_COUNT) % INGREDIENT_COUNT;
  return INGREDIENTS[i].gramsPerMl;
}

namespace {
double factorOf(const Unit& u, const double gramsPerMl) {
  return u.kind == CookVolume ? u.factor * gramsPerMl : u.factor;
}
}  // namespace

double UnitsActivity::convertTo(const int targetUnit) const {
  const Family& f = FAMILIES[familyIdx];
  const double d = density();
  const Unit& from = f.units[unitIdx];
  const Unit& to = f.units[targetUnit];
  const double base = amount() * factorOf(from, d) + from.offset;
  const double factor = factorOf(to, d);
  if (factor == 0) return 0;
  return (base - to.offset) / factor;
}

int UnitsActivity::heroUnit() const {
  const Family& f = FAMILIES[familyIdx];
  int p = f.units[unitIdx].partner;
  if (p < 0 || p >= f.count || p == unitIdx) p = (unitIdx + 1) % f.count;
  return p;
}

// =============================================================================
//  Lo último que se usó, en la SD
// =============================================================================

void UnitsActivity::loadState() {
  char buf[64] = {0};
  if (Storage.readFileToBuffer(STATE_PATH, buf, sizeof(buf)) == 0) return;
  char famKey[24] = {0};
  char unitKey[24] = {0};
  int ing = 0;
  if (sscanf(buf, "%23s %23s %d", famKey, unitKey, &ing) < 2) return;
  for (int f = 0; f < FAMILY_COUNT; f++) {
    if (strcmp(FAMILIES[f].key, famKey) != 0) continue;
    const int u = findUnit(f, unitKey);
    selectFamily(f, u >= 0 ? u : 0);
    break;
  }
  if (ing >= 0 && ing < INGREDIENT_COUNT) ingredientIdx = ing;
}

void UnitsActivity::saveState() const {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %s %d\n", FAMILIES[familyIdx].key, FAMILIES[familyIdx].units[unitIdx].key,
           ingredientIdx);
  Storage.writeFile(STATE_PATH, String(buf));
}

// =============================================================================
//  Dictado
// =============================================================================

void UnitsActivity::fail(const StrId why, const std::string& detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  notice = I18N.get(why);
  state = EDIT;
  requestUpdate();
}

void UnitsActivity::startRecording() {
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  notice.clear();
  spoken.clear();
  state = RECORDING;
  requestUpdate();
}

void UnitsActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    state = EDIT;  // un toque sin querer: no se sube nada
    requestUpdate();
    return;
  }
  wifiActivated = true;
  beginConnect();
}

void UnitsActivity::beginConnect() {
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) {  // ya conectado o sin redes guardadas: sin cartel de más
    pumpConnect();
    return;
  }
  requestUpdate();
}

void UnitsActivity::pumpConnect() {
  if (wifiPicker) return;
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase phase = wifi.pump();
  if (phase == FriendlyWifi::Phase::Connected) {
    onWifiSelectionComplete(true);
    return;
  }
  if (phase == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             onWifiSelectionComplete(!result.isCancelled);
                           });
    return;
  }
  if (wifi.revision() != rev) requestUpdate();
}

void UnitsActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = TRANSCRIBING;  // la subida sale del loop, con la pantalla ya pintada
  requestPending = true;
  requestUpdate();
}

void UnitsActivity::performTranscribe() {
  requestPending = false;
  std::string detail;
  const bool ok = SpeechToText::transcribe(recorder, spoken, detail);
  recorder.release();
  WiFi.setSleep(true);
  if (!ok) {
    fail(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  applySpoken();
}

// El texto vuelve del servidor y el número y la unidad los saca el aparato: es
// una cuenta de tablas, no hace falta pedirle nada más a nadie.
void UnitsActivity::applySpoken() {
  Spoken parsed;
  state = EDIT;
  if (!parseSpoken(spoken, familyIdx, parsed)) {
    notice = spoken.empty() ? std::string(tr(STR_UNITS_NOT_UNDERSTOOD))
                            : std::string(tr(STR_UNITS_NOT_UNDERSTOOD)) + " (" + spoken + ")";
    requestUpdate();
    return;
  }
  if (parsed.hasUnit) {
    if (parsed.ingredient >= 0) ingredientIdx = parsed.ingredient;
    selectFamily(parsed.family, parsed.unit);
  }
  if (parsed.hasValue) setAmount(parsed.value);
  notice = spoken;
  // Con la cantidad puesta, lo que el usuario quiere ver es el resultado: el
  // cursor se corre al primer dígito para que la palanca sirva para retocarlo.
  cursor = slotCount - DIGITS;
  LOG_INF(TAG, "\"%s\" -> %s %s", spoken.c_str(), FAMILIES[familyIdx].key, FAMILIES[familyIdx].units[unitIdx].key);
  requestUpdate();
}

// =============================================================================
//  Entrada
// =============================================================================

void UnitsActivity::loop() {
  switch (state) {
    case EDIT: {
      // Inclinar el aparato a los costados mueve el campo elegido: la palanca
      // queda libre para el valor, que es lo que se toca todo el tiempo.
      if (MOTION.take(MotionInput::Event::TiltRight)) moveCursor(1);
      if (MOTION.take(MotionInput::Event::TiltLeft)) moveCursor(-1);
      navigator.onPrevious([this] { adjust(1); });
      navigator.onNext([this] { adjust(-1); });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (slot().field == Field::Say) {
          startRecording();
        } else {
          moveCursor(1);
        }
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        if (cursor > 0) {
          moveCursor(-1);
        } else {
          finish();
        }
      }
      break;
    }
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        state = EDIT;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        stopRecording();
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case CONNECTING:
      if (!wifiPicker && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        recorder.release();
        state = EDIT;
        requestUpdate();
        break;
      }
      pumpConnect();
      break;
    case TRANSCRIBING:
      if (requestPending) performTranscribe();
      break;
  }
}

// =============================================================================
//  Pantalla
// =============================================================================

void UnitsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_UNITS_TITLE));

  const char* confirmLabel = "";
  if (state == EDIT) {
    renderInput();
    confirmLabel = slot().field == Field::Say ? tr(STR_UNITS_SAY) : tr(STR_SELECT);
  } else {
    renderVoice();
    if (state == RECORDING) confirmLabel = tr(STR_SELECT);
  }

  const bool jog = state == EDIT;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), confirmLabel, jog ? tr(STR_DIR_UP) : "", jog ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de refrescos limpios la lleva el coordinador del panel.
  renderer.displayBuffer();
}

// El cartel del dictado, con el ejemplo debajo: sin ejemplo nadie sabe qué se
// puede decir, y "tres tazas de harina" lo explica solo.
void UnitsActivity::renderVoice() {
  const int mid = renderer.getScreenHeight() / 2;
  const char* title = tr(STR_UNITS_LISTENING);
  if (state == TRANSCRIBING) title = tr(STR_UNITS_UNDERSTANDING);
  if (state == CONNECTING) {
    if (!wifiPicker) FriendlyWifi::drawStatus(renderer, wifi, mid);
    return;
  }
  renderer.drawCenteredText(UI_14_FONT_ID, mid - 40, title);
  listui::hint(renderer, mid + 8, tr(STR_UNITS_EXAMPLE));
}

void UnitsActivity::renderInput() {
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  const int hintY = listui::contentBottom(renderer) - listui::HINT_H;
  int y = listui::contentTop();

  // Fila de dictado. El detalle es lo último que se escuchó (o el error), que
  // es donde el usuario mira para saber si el aparato le entendió.
  {
    listui::RowSpec say;
    say.title = tr(STR_UNITS_SAY);
    say.detail = notice.empty() ? tr(STR_UNITS_SAY_HINT) : notice.c_str();
    say.bold = true;
    say.selected = slot().field == Field::Say;
    listui::row(renderer, x, y, w, listui::ROW2_H, say);
    y += listui::ROW2_H;
  }

  {
    listui::RowSpec kind;
    kind.title = tr(STR_UNITS_KIND);
    kind.meta = I18N.get(FAMILIES[familyIdx].name);
    kind.selected = slot().field == Field::Kind;
    listui::row(renderer, x, y, w, listui::ROW1_H, kind);
    y += listui::ROW1_H;
  }

  if (FAMILIES[familyIdx].cooking) {
    listui::RowSpec ing;
    ing.title = tr(STR_UNITS_INGREDIENT);
    ing.meta = I18N.get(INGREDIENTS[ingredientIdx].name);
    ing.selected = slot().field == Field::Ingredient;
    listui::row(renderer, x, y, w, listui::ROW1_H, ing);
    y += listui::ROW1_H;
  }

  const Unit& from = FAMILIES[familyIdx].units[unitIdx];
  std::string unitLabel = I18N.get(from.name);
  if (from.symbol[0] != '\0') unitLabel += std::string(" · ") + from.symbol;
  {
    listui::RowSpec unit;
    unit.title = tr(STR_UNITS_UNIT);
    unit.meta = unitLabel.c_str();
    unit.selected = slot().field == Field::Unit;
    listui::row(renderer, x, y, w, listui::ROW1_H, unit);
    y += listui::ROW1_H;
  }

  y += listui::GAP;
  y = renderAmount(x, y, w);
  y += listui::GAP;
  y = renderResult(x, y, w);
  y += listui::GAP;
  renderEquivalences(x, y, w, hintY - listui::GAP);

  const char* hint = tr(STR_UNITS_HINT_FIELD);
  if (slot().field == Field::Digit) hint = tr(STR_UNITS_HINT_DIGIT);
  if (slot().field == Field::Say) hint = tr(STR_UNITS_SAY_HINT);
  listui::hint(renderer, hintY, hint);
}

// El carrusel: la etiqueta a la izquierda y los seis dígitos alineados a la
// derecha, con la coma en su lugar. El elegido lleva una flecha arriba, otra
// abajo y una barra: se ve de lejos cuál va a mover la palanca, y nada de eso
// cae encima de una cifra.
int UnitsActivity::renderAmount(const int x, const int y, const int w) const {
  const int left = x + listui::PAD;
  const int right = x + w - listui::PAD;
  const int digitW = 34;
  const int digitH = 54;
  const int gap = 8;
  const int dotW = 14;
  const int signW = 22;
  const int arrow = 9;
  const bool hasSign = familyIdx == FAMILY_TEMP;
  const int blockH = digitH + 2 * (arrow + 4);
  const int totalW = (hasSign ? signW + gap : 0) + DIGITS * digitW + (DIGITS - 1) * gap + dotW;
  const int startX = std::max(left, right - totalW);
  const int digitsTop = y + arrow + 4;

  renderer.drawText(UI_10_FONT_ID, left, y + (blockH - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    tr(STR_UNITS_AMOUNT));

  // Lo elegido se marca con una flecha arriba, otra abajo y una barra al pie:
  // dice qué va a mover la palanca sin tapar la cifra con una trama.
  const auto mark = [&](const int cellX, const int cellW) {
    const int mid = cellX + cellW / 2;
    const int xs[3] = {mid - arrow, mid + arrow, mid};
    const int upY = digitsTop - 4;
    const int ysUp[3] = {upY, upY, upY - arrow};
    renderer.fillPolygon(xs, ysUp, 3, true);
    const int downY = digitsTop + digitH + 4;
    const int ysDown[3] = {downY, downY, downY + arrow};
    renderer.fillPolygon(xs, ysDown, 3, true);
    renderer.fillRect(cellX, digitsTop + digitH + 1, cellW, 3, true);
  };

  int cx = startX;
  if (hasSign) {
    if (negative) renderer.fillRect(cx, digitsTop + digitH / 2 - 3, signW, 6, true);
    if (slot().field == Field::Sign) mark(cx, signW);
    cx += signW + gap;
  }
  for (int i = 0; i < DIGITS; i++) {
    sevenseg::digit(renderer, digits[i], cx, digitsTop, digitW, digitH, 6);
    if (slot().field == Field::Digit && slot().digit == i) mark(cx, digitW);
    cx += digitW;
    if (i == INT_DIGITS - 1) {
      renderer.fillRect(cx + dotW / 2 - 3, digitsTop + digitH - 6, 6, 6, true);
      cx += dotW;
    }
    if (i < DIGITS - 1) cx += gap;
  }

  const int bottom = y + blockH;
  listui::rule(renderer, x, bottom, w);
  return bottom + 1;
}

// El resultado: el número grande y, al lado, la unidad. Es lo que se mira desde
// el otro lado de la mesada, así que va con los segmentos y no con una fuente.
int UnitsActivity::renderResult(const int x, const int y, const int w) const {
  const Family& f = FAMILIES[familyIdx];
  const int hero = heroUnit();
  const Unit& to = f.units[hero];
  const std::string text = formatNumber(convertTo(hero));
  const std::string symbol = to.symbol[0] != '\0' ? std::string(to.symbol) : std::string(I18N.get(to.name));

  const int left = x + listui::PAD;
  const int blockH = 80;  // grilla de 8
  const int labelW = std::min(w / 3, renderer.getTextWidth(UI_14_FONT_ID, symbol.c_str()) + 8);
  const int numberW = w - 2 * listui::PAD - labelW - listui::META_GAP;
  const int drawn = drawSevenSegText(renderer, text.c_str(), left, y, numberW, blockH, 50);

  const int labelX = left + drawn + listui::META_GAP;
  const int ascent = renderer.getFontAscenderSize(UI_14_FONT_ID);
  const int symbolY = y + blockH / 2 - ascent;
  renderer.drawText(UI_14_FONT_ID, labelX, symbolY,
                    renderer.truncatedText(UI_14_FONT_ID, symbol.c_str(), x + w - labelX).c_str());
  if (to.symbol[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, labelX, symbolY + renderer.getLineHeight(UI_14_FONT_ID) - 4,
                      renderer.truncatedText(UI_10_FONT_ID, I18N.get(to.name), x + w - labelX).c_str());
  }

  const int bottom = y + blockH;
  listui::rule(renderer, x, bottom, w);
  return bottom + 1;
}

// Todas las demás de la familia, para que "cinco millas" muestre kilómetros,
// metros y pies de una sola vez. Una regla de 1 px entre renglones y nada más.
void UnitsActivity::renderEquivalences(const int x, const int y, const int w, const int bottom) const {
  const Family& f = FAMILIES[familyIdx];
  const int hero = heroUnit();
  int rows[16];
  int count = 0;
  for (int i = 0; i < f.count && count < 16; i++) {
    if (i == unitIdx || i == hero) continue;
    rows[count++] = i;
  }
  if (count == 0) return;

  const int available = bottom - y;
  if (available < 24) return;
  const int rowH = std::min(48, std::max(24, available / count));
  const int left = x + listui::PAD;
  const int right = x + w - listui::PAD;

  for (int i = 0; i < count; i++) {
    const int top = y + i * rowH;
    if (top + rowH > bottom + 1) break;
    const Unit& u = f.units[rows[i]];
    std::string value = formatNumber(convertTo(rows[i]));
    if (u.symbol[0] != '\0') value += std::string(" ") + u.symbol;
    const int textY = top + (rowH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, right - valueW, textY, value.c_str(), true, EpdFontFamily::BOLD);
    const int nameW = right - valueW - listui::META_GAP - left;
    if (nameW > 0) {
      renderer.drawText(UI_12_FONT_ID, left, textY,
                        renderer.truncatedText(UI_12_FONT_ID, I18N.get(u.name), nameW).c_str());
    }
    if (i + 1 < count) listui::rule(renderer, x, top + rowH - 1, w);
  }
}
