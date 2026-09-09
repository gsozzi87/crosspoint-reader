// Tarjetas de bebé: la palabra en español y en inglés, y de qué dibujo sale.
// Las usa el juego de tarjetas del aparato; el servidor genera el dibujo
// (320x320 en 4 grises, listo para la pantalla) y la voz (Piper, ADPCM), y los
// dos viajan en el paquete de contenido (ver assets.ts). El aparato NO
// decodifica PNG ni MP3.
//
// ── Los dibujos ─────────────────────────────────────────────────────────────
// Salen de **Noto Color Emoji** (googlefonts/noto-emoji, Apache 2.0 para el
// código y OFL 1.1 para los glifos): ilustraciones LLENAS, no iconos de trazo.
// Un bebé no reconoce un contorno, reconoce una figura. No obligan a atribuir a
// nadie (por eso se eligió Noto y no OpenMoji ni Twemoji, que son CC BY-SA /
// CC BY); igual quedan acreditados en server/README.md.
//
// La pantalla del aparato pinta **4 grises**, no blanco y negro: los dibujos se
// convierten con el mismo pipeline que las fotos (`toDeviceBmp` de photos.ts) y
// salen como BMP de 2 bpp. Por eso ahora sí se puede usar un dibujo con
// volumen en vez de una silueta.
//
// `icon`:
//   "emoji:<cp>"   → el SVG de Noto (`emoji_u<cp>.svg`); varios puntos de
//                    código van separados por "-" (como en el nombre del
//                    archivo, que los separa por "_")
//   "num:<n>"      → dibujo generado acá: n puntos para contar
//   "shape:<clave>"→ dibujo generado acá: la forma llena en negro
//
// ── REGLA FIJA: español NEUTRO ──────────────────────────────────────────────
// Todo lo que ve el usuario va en español neutro, entendible en toda
// Hispanoamérica y en España. Nada de rioplatense, ni de mexicano, ni de
// peninsular. **Si una palabra no tiene una variante clara y neutra, no se
// discute: se saca y se pone otra cosa** — son tarjetas para un bebé, sobran
// los sustantivos fáciles.
//
// Lo ya resuelto (no volver atrás):
//   remera/playera/polera → camiseta      pollera → falda (no hay dibujo: fuera)
//   frutilla → fresa                      palta → aguacate
//   ananá → piña                          durazno/melocotón → fuera
//   auto/coche/carro → auto               colectivo/guagua/camión → autobús
//   celular/móvil → teléfono              ordenador → computadora
//   pileta/alberca → piscina              torta/tarta → pastel
//   cacahuete → maní                      anteojos/lentes → gafas
//   media → calcetín                      heladera/refrigerador → fuera
//   lavarropas/lavadora → fuera           mamadera → biberón
//   pochoclo/palomitas → fuera            papa/patata → fuera
//   zumo/jugo → fuera                     manteca/mantequilla → fuera
//   pileta/piscina → piscina              bombita/foco/bombilla → luz
//   sillón → sofá                         tina → bañera
//   vaquita de San Antonio/catarina → fuera
//
// El inglés va en **americano** y consistente (cookie, candy, truck, pants,
// soccer→ball, eraser, gray): es el inglés que el bebé va a escuchar en los
// dibujitos y el que dice la voz de Piper en inglés.

export type CardCategory =
  | "animales" | "comida" | "casa" | "cuerpo" | "formas"
  | "numeros" | "vehiculos" | "naturaleza" | "ropa" | "juguetes";

export type Card = { id: string; cat: CardCategory; es: string; en: string; icon: string };

export const CARDS: Card[] = [
  // ── animales ──
  { id: "ani-perro",             cat: "animales",   es: "perro",             en: "dog",           icon: "emoji:1f436" },
  { id: "ani-gato",              cat: "animales",   es: "gato",              en: "cat",           icon: "emoji:1f431" },
  { id: "ani-conejo",            cat: "animales",   es: "conejo",            en: "rabbit",        icon: "emoji:1f430" },
  { id: "ani-raton",             cat: "animales",   es: "ratón",             en: "mouse",         icon: "emoji:1f42d" },
  { id: "ani-oso",               cat: "animales",   es: "oso",               en: "bear",          icon: "emoji:1f43b" },
  { id: "ani-panda",             cat: "animales",   es: "panda",             en: "panda",         icon: "emoji:1f43c" },
  { id: "ani-leon",              cat: "animales",   es: "león",              en: "lion",          icon: "emoji:1f981" },
  { id: "ani-tigre",             cat: "animales",   es: "tigre",             en: "tiger",         icon: "emoji:1f42f" },
  { id: "ani-mono",              cat: "animales",   es: "mono",              en: "monkey",        icon: "emoji:1f435" },
  { id: "ani-elefante",          cat: "animales",   es: "elefante",          en: "elephant",      icon: "emoji:1f418" },
  { id: "ani-jirafa",            cat: "animales",   es: "jirafa",            en: "giraffe",       icon: "emoji:1f992" },
  { id: "ani-cebra",             cat: "animales",   es: "cebra",             en: "zebra",         icon: "emoji:1f993" },
  { id: "ani-vaca",              cat: "animales",   es: "vaca",              en: "cow",           icon: "emoji:1f42e" },
  { id: "ani-caballo",           cat: "animales",   es: "caballo",           en: "horse",         icon: "emoji:1f434" },
  { id: "ani-cerdo",             cat: "animales",   es: "cerdo",             en: "pig",           icon: "emoji:1f437" },
  { id: "ani-oveja",             cat: "animales",   es: "oveja",             en: "sheep",         icon: "emoji:1f411" },
  { id: "ani-cabra",             cat: "animales",   es: "cabra",             en: "goat",          icon: "emoji:1f410" },
  { id: "ani-gallina",           cat: "animales",   es: "gallina",           en: "hen",           icon: "emoji:1f414" },
  { id: "ani-pollito",           cat: "animales",   es: "pollito",           en: "chick",         icon: "emoji:1f424" },
  { id: "ani-pato",              cat: "animales",   es: "pato",              en: "duck",          icon: "emoji:1f986" },
  { id: "ani-pajaro",            cat: "animales",   es: "pájaro",            en: "bird",          icon: "emoji:1f426" },
  { id: "ani-loro",              cat: "animales",   es: "loro",              en: "parrot",        icon: "emoji:1f99c" },
  { id: "ani-buho",              cat: "animales",   es: "búho",              en: "owl",           icon: "emoji:1f989" },
  { id: "ani-pinguino",          cat: "animales",   es: "pingüino",          en: "penguin",       icon: "emoji:1f427" },
  { id: "ani-rana",              cat: "animales",   es: "rana",              en: "frog",          icon: "emoji:1f438" },
  { id: "ani-tortuga",           cat: "animales",   es: "tortuga",           en: "turtle",        icon: "emoji:1f422" },
  { id: "ani-serpiente",         cat: "animales",   es: "serpiente",         en: "snake",         icon: "emoji:1f40d" },
  { id: "ani-caracol",           cat: "animales",   es: "caracol",           en: "snail",         icon: "emoji:1f40c" },
  { id: "ani-mariposa",          cat: "animales",   es: "mariposa",          en: "butterfly",     icon: "emoji:1f98b" },
  { id: "ani-abeja",             cat: "animales",   es: "abeja",             en: "bee",           icon: "emoji:1f41d" },
  { id: "ani-hormiga",           cat: "animales",   es: "hormiga",           en: "ant",           icon: "emoji:1f41c" },
  { id: "ani-pez",               cat: "animales",   es: "pez",               en: "fish",          icon: "emoji:1f41f" },
  { id: "ani-ballena",           cat: "animales",   es: "ballena",           en: "whale",         icon: "emoji:1f433" },
  { id: "ani-delfin",            cat: "animales",   es: "delfín",            en: "dolphin",       icon: "emoji:1f42c" },
  { id: "ani-pulpo",             cat: "animales",   es: "pulpo",             en: "octopus",       icon: "emoji:1f419" },
  { id: "ani-cangrejo",          cat: "animales",   es: "cangrejo",          en: "crab",          icon: "emoji:1f980" },
  { id: "ani-ardilla",           cat: "animales",   es: "ardilla",           en: "squirrel",      icon: "emoji:1f43f" },
  { id: "ani-zorro",             cat: "animales",   es: "zorro",             en: "fox",           icon: "emoji:1f98a" },
  { id: "ani-erizo",             cat: "animales",   es: "erizo",             en: "hedgehog",      icon: "emoji:1f994" },
  { id: "ani-koala",             cat: "animales",   es: "koala",             en: "koala",         icon: "emoji:1f428" },
  { id: "ani-canguro",           cat: "animales",   es: "canguro",           en: "kangaroo",      icon: "emoji:1f998" },
  { id: "ani-camello",           cat: "animales",   es: "camello",           en: "camel",         icon: "emoji:1f42b" },
  { id: "ani-murcielago",        cat: "animales",   es: "murciélago",        en: "bat",           icon: "emoji:1f987" },
  { id: "ani-dinosaurio",        cat: "animales",   es: "dinosaurio",        en: "dinosaur",      icon: "emoji:1f995" },
  { id: "ani-unicornio",         cat: "animales",   es: "unicornio",         en: "unicorn",       icon: "emoji:1f984" },
  { id: "ani-huella",            cat: "animales",   es: "huella",            en: "paw print",     icon: "emoji:1f43e" },
  // ── comida ──
  { id: "com-manzana",           cat: "comida",     es: "manzana",           en: "apple",         icon: "emoji:1f34e" },
  { id: "com-banana",            cat: "comida",     es: "banana",            en: "banana",        icon: "emoji:1f34c" },
  { id: "com-naranja",           cat: "comida",     es: "naranja",           en: "orange",        icon: "emoji:1f34a" },
  { id: "com-uvas",              cat: "comida",     es: "uvas",              en: "grapes",        icon: "emoji:1f347" },
  { id: "com-fresa",             cat: "comida",     es: "fresa",             en: "strawberry",    icon: "emoji:1f353" },
  { id: "com-sandia",            cat: "comida",     es: "sandía",            en: "watermelon",    icon: "emoji:1f349" },
  { id: "com-pina",              cat: "comida",     es: "piña",              en: "pineapple",     icon: "emoji:1f34d" },
  { id: "com-limon",             cat: "comida",     es: "limón",             en: "lemon",         icon: "emoji:1f34b" },
  { id: "com-pera",              cat: "comida",     es: "pera",              en: "pear",          icon: "emoji:1f350" },
  { id: "com-cereza",            cat: "comida",     es: "cereza",            en: "cherry",        icon: "emoji:1f352" },
  { id: "com-aguacate",          cat: "comida",     es: "aguacate",          en: "avocado",       icon: "emoji:1f951" },
  { id: "com-zanahoria",         cat: "comida",     es: "zanahoria",         en: "carrot",        icon: "emoji:1f955" },
  { id: "com-tomate",            cat: "comida",     es: "tomate",            en: "tomato",        icon: "emoji:1f345" },
  { id: "com-maiz",              cat: "comida",     es: "maíz",              en: "corn",          icon: "emoji:1f33d" },
  { id: "com-brocoli",           cat: "comida",     es: "brócoli",           en: "broccoli",      icon: "emoji:1f966" },
  { id: "com-pepino",            cat: "comida",     es: "pepino",            en: "cucumber",      icon: "emoji:1f952" },
  { id: "com-cebolla",           cat: "comida",     es: "cebolla",           en: "onion",         icon: "emoji:1f9c5" },
  { id: "com-ajo",               cat: "comida",     es: "ajo",               en: "garlic",        icon: "emoji:1f9c4" },
  { id: "com-mani",              cat: "comida",     es: "maní",              en: "peanut",        icon: "emoji:1f95c" },
  { id: "com-pan",               cat: "comida",     es: "pan",               en: "bread",         icon: "emoji:1f35e" },
  { id: "com-queso",             cat: "comida",     es: "queso",             en: "cheese",        icon: "emoji:1f9c0" },
  { id: "com-huevo",             cat: "comida",     es: "huevo",             en: "egg",           icon: "emoji:1f95a" },
  { id: "com-leche",             cat: "comida",     es: "leche",             en: "milk",          icon: "emoji:1f95b" },
  { id: "com-biberon",           cat: "comida",     es: "biberón",           en: "baby bottle",   icon: "emoji:1f37c" },
  { id: "com-pizza",             cat: "comida",     es: "pizza",             en: "pizza",         icon: "emoji:1f355" },
  { id: "com-hamburguesa",       cat: "comida",     es: "hamburguesa",       en: "hamburger",     icon: "emoji:1f354" },
  { id: "com-sandwich",          cat: "comida",     es: "sándwich",          en: "sandwich",      icon: "emoji:1f96a" },
  { id: "com-pollo",             cat: "comida",     es: "pollo",             en: "chicken",       icon: "emoji:1f357" },
  { id: "com-arroz",             cat: "comida",     es: "arroz",             en: "rice",          icon: "emoji:1f35a" },
  { id: "com-sopa",              cat: "comida",     es: "sopa",              en: "soup",          icon: "emoji:1f372" },
  { id: "com-sal",               cat: "comida",     es: "sal",               en: "salt",          icon: "emoji:1f9c2" },
  { id: "com-helado",            cat: "comida",     es: "helado",            en: "ice cream",     icon: "emoji:1f366" },
  { id: "com-pastel",            cat: "comida",     es: "pastel",            en: "cake",          icon: "emoji:1f382" },
  { id: "com-galleta",           cat: "comida",     es: "galleta",           en: "cookie",        icon: "emoji:1f36a" },
  { id: "com-chocolate",         cat: "comida",     es: "chocolate",         en: "chocolate",     icon: "emoji:1f36b" },
  { id: "com-caramelo",          cat: "comida",     es: "caramelo",          en: "candy",         icon: "emoji:1f36c" },
  { id: "com-miel",              cat: "comida",     es: "miel",              en: "honey",         icon: "emoji:1f36f" },
  { id: "com-cafe",              cat: "comida",     es: "café",              en: "coffee",        icon: "emoji:2615" },
  // ── casa ──
  { id: "cas-casa",              cat: "casa",       es: "casa",              en: "house",         icon: "emoji:1f3e0" },
  { id: "cas-puerta",            cat: "casa",       es: "puerta",            en: "door",          icon: "emoji:1f6aa" },
  { id: "cas-ventana",           cat: "casa",       es: "ventana",           en: "window",        icon: "emoji:1fa9f" },
  { id: "cas-cama",              cat: "casa",       es: "cama",              en: "bed",           icon: "emoji:1f6cf" },
  { id: "cas-silla",             cat: "casa",       es: "silla",             en: "chair",         icon: "emoji:1fa91" },
  { id: "cas-sofa",              cat: "casa",       es: "sofá",              en: "sofa",          icon: "emoji:1f6cb" },
  { id: "cas-espejo",            cat: "casa",       es: "espejo",            en: "mirror",        icon: "emoji:1fa9e" },
  { id: "cas-luz",               cat: "casa",       es: "luz",               en: "light",         icon: "emoji:1f4a1" },
  { id: "cas-vela",              cat: "casa",       es: "vela",              en: "candle",        icon: "emoji:1f56f" },
  { id: "cas-reloj",             cat: "casa",       es: "reloj",             en: "watch",         icon: "emoji:231a" },
  { id: "cas-despertador",       cat: "casa",       es: "despertador",       en: "alarm clock",   icon: "emoji:23f0" },
  { id: "cas-llave",             cat: "casa",       es: "llave",             en: "key",           icon: "emoji:1f511" },
  { id: "cas-telefono",          cat: "casa",       es: "teléfono",          en: "phone",         icon: "emoji:1f4f1" },
  { id: "cas-television",        cat: "casa",       es: "televisión",        en: "television",    icon: "emoji:1f4fa" },
  { id: "cas-computadora",       cat: "casa",       es: "computadora",       en: "computer",      icon: "emoji:1f4bb" },
  { id: "cas-camara",            cat: "casa",       es: "cámara",            en: "camera",        icon: "emoji:1f4f7" },
  { id: "cas-libro",             cat: "casa",       es: "libro",             en: "book",          icon: "emoji:1f4d6" },
  { id: "cas-lapiz",             cat: "casa",       es: "lápiz",             en: "pencil",        icon: "emoji:270f" },
  { id: "cas-tijeras",           cat: "casa",       es: "tijeras",           en: "scissors",      icon: "emoji:2702" },
  { id: "cas-martillo",          cat: "casa",       es: "martillo",          en: "hammer",        icon: "emoji:1f528" },
  { id: "cas-escoba",            cat: "casa",       es: "escoba",            en: "broom",         icon: "emoji:1f9f9" },
  { id: "cas-jabon",             cat: "casa",       es: "jabón",             en: "soap",          icon: "emoji:1f9fc" },
  { id: "cas-cepillo-de-dientes",cat: "casa",       es: "cepillo de dientes",en: "toothbrush",    icon: "emoji:1faa5" },
  { id: "cas-inodoro",           cat: "casa",       es: "inodoro",           en: "toilet",        icon: "emoji:1f6bd" },
  { id: "cas-ducha",             cat: "casa",       es: "ducha",             en: "shower",        icon: "emoji:1f6bf" },
  { id: "cas-banera",            cat: "casa",       es: "bañera",            en: "bathtub",       icon: "emoji:1f6c1" },
  { id: "cas-cuchara",           cat: "casa",       es: "cuchara",           en: "spoon",         icon: "emoji:1f944" },
  { id: "cas-plato",             cat: "casa",       es: "plato",             en: "plate",         icon: "emoji:1f37d" },
  { id: "cas-taza",              cat: "casa",       es: "taza",              en: "cup",           icon: "emoji:1f375" },
  { id: "cas-basura",            cat: "casa",       es: "basura",            en: "trash can",     icon: "emoji:1f5d1" },
  { id: "cas-caja",              cat: "casa",       es: "caja",              en: "box",           icon: "emoji:1f4e6" },
  // ── cuerpo ──
  { id: "cue-mano",              cat: "cuerpo",     es: "mano",              en: "hand",          icon: "emoji:270b" },
  { id: "cue-pie",               cat: "cuerpo",     es: "pie",               en: "foot",          icon: "emoji:1f9b6" },
  { id: "cue-pierna",            cat: "cuerpo",     es: "pierna",            en: "leg",           icon: "emoji:1f9b5" },
  { id: "cue-brazo",             cat: "cuerpo",     es: "brazo",             en: "arm",           icon: "emoji:1f4aa" },
  { id: "cue-ojo",               cat: "cuerpo",     es: "ojo",               en: "eye",           icon: "emoji:1f441" },
  { id: "cue-oreja",             cat: "cuerpo",     es: "oreja",             en: "ear",           icon: "emoji:1f442" },
  { id: "cue-nariz",             cat: "cuerpo",     es: "nariz",             en: "nose",          icon: "emoji:1f443" },
  { id: "cue-boca",              cat: "cuerpo",     es: "boca",              en: "mouth",         icon: "emoji:1f444" },
  { id: "cue-diente",            cat: "cuerpo",     es: "diente",            en: "tooth",         icon: "emoji:1f9b7" },
  { id: "cue-cara",              cat: "cuerpo",     es: "cara",              en: "face",          icon: "emoji:1f600" },
  { id: "cue-hola",              cat: "cuerpo",     es: "hola",              en: "hello",         icon: "emoji:1f44b" },
  { id: "cue-bebe",              cat: "cuerpo",     es: "bebé",              en: "baby",          icon: "emoji:1f476" },
  { id: "cue-mama",              cat: "cuerpo",     es: "mamá",              en: "mom",           icon: "emoji:1f469" },
  { id: "cue-papa",              cat: "cuerpo",     es: "papá",              en: "dad",           icon: "emoji:1f468" },
  { id: "cue-abuela",            cat: "cuerpo",     es: "abuela",            en: "grandma",       icon: "emoji:1f475" },
  { id: "cue-abuelo",            cat: "cuerpo",     es: "abuelo",            en: "grandpa",       icon: "emoji:1f474" },
  // ── formas ── (dibujadas acá: la forma llena en negro, que es lo que más se
  // lee en tinta electrónica; los emoji de formas dependen del color)
  { id: "for-circulo",           cat: "formas",     es: "círculo",           en: "circle",        icon: "shape:circulo" },
  { id: "for-cuadrado",          cat: "formas",     es: "cuadrado",          en: "square",        icon: "shape:cuadrado" },
  { id: "for-rectangulo",        cat: "formas",     es: "rectángulo",        en: "rectangle",     icon: "shape:rectangulo" },
  { id: "for-triangulo",         cat: "formas",     es: "triángulo",         en: "triangle",      icon: "shape:triangulo" },
  { id: "for-ovalo",             cat: "formas",     es: "óvalo",             en: "oval",          icon: "shape:ovalo" },
  { id: "for-rombo",             cat: "formas",     es: "rombo",             en: "diamond",       icon: "shape:rombo" },
  { id: "for-estrella",          cat: "formas",     es: "estrella",          en: "star",          icon: "shape:estrella" },
  { id: "for-corazon",           cat: "formas",     es: "corazón",           en: "heart",         icon: "shape:corazon" },
  { id: "for-cruz",              cat: "formas",     es: "cruz",              en: "cross",         icon: "shape:cruz" },
  // ── numeros ── (n puntos para contar: a esta edad sirve más que el dígito)
  { id: "num-uno",               cat: "numeros",    es: "uno",               en: "one",           icon: "num:1" },
  { id: "num-dos",               cat: "numeros",    es: "dos",               en: "two",           icon: "num:2" },
  { id: "num-tres",              cat: "numeros",    es: "tres",              en: "three",         icon: "num:3" },
  { id: "num-cuatro",            cat: "numeros",    es: "cuatro",            en: "four",          icon: "num:4" },
  { id: "num-cinco",             cat: "numeros",    es: "cinco",             en: "five",          icon: "num:5" },
  { id: "num-seis",              cat: "numeros",    es: "seis",              en: "six",           icon: "num:6" },
  { id: "num-siete",             cat: "numeros",    es: "siete",             en: "seven",         icon: "num:7" },
  { id: "num-ocho",              cat: "numeros",    es: "ocho",              en: "eight",         icon: "num:8" },
  { id: "num-nueve",             cat: "numeros",    es: "nueve",             en: "nine",          icon: "num:9" },
  { id: "num-diez",              cat: "numeros",    es: "diez",              en: "ten",           icon: "num:10" },
  // ── vehiculos ──
  { id: "veh-auto",              cat: "vehiculos",  es: "auto",              en: "car",           icon: "emoji:1f697" },
  { id: "veh-autobus",           cat: "vehiculos",  es: "autobús",           en: "bus",           icon: "emoji:1f68c" },
  { id: "veh-camion",            cat: "vehiculos",  es: "camión",            en: "truck",         icon: "emoji:1f69a" },
  { id: "veh-taxi",              cat: "vehiculos",  es: "taxi",              en: "taxi",          icon: "emoji:1f695" },
  { id: "veh-ambulancia",        cat: "vehiculos",  es: "ambulancia",        en: "ambulance",     icon: "emoji:1f691" },
  { id: "veh-bomberos",          cat: "vehiculos",  es: "camión de bomberos",en: "fire truck",    icon: "emoji:1f692" },
  { id: "veh-tractor",           cat: "vehiculos",  es: "tractor",           en: "tractor",       icon: "emoji:1f69c" },
  { id: "veh-bicicleta",         cat: "vehiculos",  es: "bicicleta",         en: "bicycle",       icon: "emoji:1f6b2" },
  { id: "veh-motocicleta",       cat: "vehiculos",  es: "motocicleta",       en: "motorcycle",    icon: "emoji:1f3cd" },
  { id: "veh-tren",              cat: "vehiculos",  es: "tren",              en: "train",         icon: "emoji:1f682" },
  { id: "veh-avion",             cat: "vehiculos",  es: "avión",             en: "airplane",      icon: "emoji:2708" },
  { id: "veh-helicoptero",       cat: "vehiculos",  es: "helicóptero",       en: "helicopter",    icon: "emoji:1f681" },
  { id: "veh-cohete",            cat: "vehiculos",  es: "cohete",            en: "rocket",        icon: "emoji:1f680" },
  { id: "veh-barco",             cat: "vehiculos",  es: "barco",             en: "ship",          icon: "emoji:1f6a2" },
  { id: "veh-velero",            cat: "vehiculos",  es: "velero",            en: "sailboat",      icon: "emoji:26f5" },
  { id: "veh-canoa",             cat: "vehiculos",  es: "canoa",             en: "canoe",         icon: "emoji:1f6f6" },
  { id: "veh-trineo",            cat: "vehiculos",  es: "trineo",            en: "sled",          icon: "emoji:1f6f7" },
  { id: "veh-semaforo",          cat: "vehiculos",  es: "semáforo",          en: "traffic light", icon: "emoji:1f6a6" },
  { id: "veh-rueda",             cat: "vehiculos",  es: "rueda",             en: "wheel",         icon: "emoji:1f6de" },
  { id: "veh-ancla",             cat: "vehiculos",  es: "ancla",             en: "anchor",        icon: "emoji:2693" },
  // ── naturaleza ──
  { id: "nat-sol",               cat: "naturaleza", es: "sol",               en: "sun",           icon: "emoji:2600" },
  { id: "nat-luna",              cat: "naturaleza", es: "luna",              en: "moon",          icon: "emoji:1f319" },
  { id: "nat-nube",              cat: "naturaleza", es: "nube",              en: "cloud",         icon: "emoji:2601" },
  { id: "nat-lluvia",            cat: "naturaleza", es: "lluvia",            en: "rain",          icon: "emoji:1f327" },
  { id: "nat-nieve",             cat: "naturaleza", es: "nieve",             en: "snowflake",     icon: "emoji:2744" },
  { id: "nat-muneco-de-nieve",   cat: "naturaleza", es: "muñeco de nieve",   en: "snowman",       icon: "emoji:26c4" },
  { id: "nat-arcoiris",          cat: "naturaleza", es: "arcoíris",          en: "rainbow",       icon: "emoji:1f308" },
  { id: "nat-rayo",              cat: "naturaleza", es: "rayo",              en: "lightning",     icon: "emoji:26a1" },
  { id: "nat-tornado",           cat: "naturaleza", es: "tornado",           en: "tornado",       icon: "emoji:1f32a" },
  { id: "nat-arbol",             cat: "naturaleza", es: "árbol",             en: "tree",          icon: "emoji:1f333" },
  { id: "nat-pino",              cat: "naturaleza", es: "pino",              en: "pine tree",     icon: "emoji:1f332" },
  { id: "nat-palmera",           cat: "naturaleza", es: "palmera",           en: "palm tree",     icon: "emoji:1f334" },
  { id: "nat-cactus",            cat: "naturaleza", es: "cactus",            en: "cactus",        icon: "emoji:1f335" },
  { id: "nat-flor",              cat: "naturaleza", es: "flor",              en: "flower",        icon: "emoji:1f338" },
  { id: "nat-girasol",           cat: "naturaleza", es: "girasol",           en: "sunflower",     icon: "emoji:1f33b" },
  { id: "nat-hoja",              cat: "naturaleza", es: "hoja",              en: "leaf",          icon: "emoji:1f343" },
  { id: "nat-trebol",            cat: "naturaleza", es: "trébol",            en: "clover",        icon: "emoji:1f340" },
  { id: "nat-planta",            cat: "naturaleza", es: "planta",            en: "plant",         icon: "emoji:1f331" },
  { id: "nat-hongo",             cat: "naturaleza", es: "hongo",             en: "mushroom",      icon: "emoji:1f344" },
  { id: "nat-montana",           cat: "naturaleza", es: "montaña",           en: "mountain",      icon: "emoji:26f0" },
  { id: "nat-volcan",            cat: "naturaleza", es: "volcán",            en: "volcano",       icon: "emoji:1f30b" },
  { id: "nat-piedra",            cat: "naturaleza", es: "piedra",            en: "rock",          icon: "emoji:1faa8" },
  { id: "nat-fuego",             cat: "naturaleza", es: "fuego",             en: "fire",          icon: "emoji:1f525" },
  { id: "nat-gota",              cat: "naturaleza", es: "gota",              en: "drop",          icon: "emoji:1f4a7" },
  { id: "nat-hielo",             cat: "naturaleza", es: "hielo",             en: "ice",           icon: "emoji:1f9ca" },
  { id: "nat-ola",               cat: "naturaleza", es: "ola",               en: "wave",          icon: "emoji:1f30a" },
  { id: "nat-mundo",             cat: "naturaleza", es: "mundo",             en: "world",         icon: "emoji:1f30d" },
  { id: "nat-paraguas",          cat: "naturaleza", es: "paraguas",          en: "umbrella",      icon: "emoji:2614" },
  { id: "nat-pluma",             cat: "naturaleza", es: "pluma",             en: "feather",       icon: "emoji:1fab6" },
  // ── ropa ──
  { id: "rop-camiseta",          cat: "ropa",       es: "camiseta",          en: "shirt",         icon: "emoji:1f455" },
  { id: "rop-pantalon",          cat: "ropa",       es: "pantalón",          en: "pants",         icon: "emoji:1f456" },
  { id: "rop-vestido",           cat: "ropa",       es: "vestido",           en: "dress",         icon: "emoji:1f457" },
  { id: "rop-abrigo",            cat: "ropa",       es: "abrigo",            en: "coat",          icon: "emoji:1f9e5" },
  { id: "rop-bufanda",           cat: "ropa",       es: "bufanda",           en: "scarf",         icon: "emoji:1f9e3" },
  { id: "rop-guante",            cat: "ropa",       es: "guante",            en: "glove",         icon: "emoji:1f9e4" },
  { id: "rop-calcetin",          cat: "ropa",       es: "calcetín",          en: "sock",          icon: "emoji:1f9e6" },
  { id: "rop-zapato",            cat: "ropa",       es: "zapato",            en: "shoe",          icon: "emoji:1f45e" },
  { id: "rop-bota",              cat: "ropa",       es: "bota",              en: "boot",          icon: "emoji:1f462" },
  { id: "rop-gorra",             cat: "ropa",       es: "gorra",             en: "cap",           icon: "emoji:1f9e2" },
  { id: "rop-sombrero",          cat: "ropa",       es: "sombrero",          en: "hat",           icon: "emoji:1f452" },
  { id: "rop-corona",            cat: "ropa",       es: "corona",            en: "crown",         icon: "emoji:1f451" },
  { id: "rop-gafas",             cat: "ropa",       es: "gafas",             en: "glasses",       icon: "emoji:1f453" },
  { id: "rop-mochila",           cat: "ropa",       es: "mochila",           en: "backpack",      icon: "emoji:1f392" },
  { id: "rop-anillo",            cat: "ropa",       es: "anillo",            en: "ring",          icon: "emoji:1f48d" },
  // ── juguetes ──
  { id: "jug-pelota",            cat: "juguetes",   es: "pelota",            en: "ball",          icon: "emoji:26bd" },
  { id: "jug-globo",             cat: "juguetes",   es: "globo",             en: "balloon",       icon: "emoji:1f388" },
  { id: "jug-oso-de-peluche",    cat: "juguetes",   es: "oso de peluche",    en: "teddy bear",    icon: "emoji:1f9f8" },
  { id: "jug-rompecabezas",      cat: "juguetes",   es: "rompecabezas",      en: "puzzle",        icon: "emoji:1f9e9" },
  { id: "jug-dado",              cat: "juguetes",   es: "dado",              en: "dice",          icon: "emoji:1f3b2" },
  { id: "jug-yoyo",              cat: "juguetes",   es: "yoyo",              en: "yo-yo",         icon: "emoji:1fa80" },
  { id: "jug-burbujas",          cat: "juguetes",   es: "burbujas",          en: "bubbles",       icon: "emoji:1fae7" },
  { id: "jug-tambor",            cat: "juguetes",   es: "tambor",            en: "drum",          icon: "emoji:1f941" },
  { id: "jug-maracas",           cat: "juguetes",   es: "maracas",           en: "maracas",       icon: "emoji:1fa87" },
  { id: "jug-guitarra",          cat: "juguetes",   es: "guitarra",          en: "guitar",        icon: "emoji:1f3b8" },
  { id: "jug-piano",             cat: "juguetes",   es: "piano",             en: "piano",         icon: "emoji:1f3b9" },
  { id: "jug-trompeta",          cat: "juguetes",   es: "trompeta",          en: "trumpet",       icon: "emoji:1f3ba" },
  { id: "jug-saxofon",           cat: "juguetes",   es: "saxofón",           en: "saxophone",     icon: "emoji:1f3b7" },
  { id: "jug-flauta",            cat: "juguetes",   es: "flauta",            en: "flute",         icon: "emoji:1fa88" },
  { id: "jug-violin",            cat: "juguetes",   es: "violín",            en: "violin",        icon: "emoji:1f3bb" },
  { id: "jug-campana",           cat: "juguetes",   es: "campana",           en: "bell",          icon: "emoji:1f514" },
  { id: "jug-musica",            cat: "juguetes",   es: "música",            en: "music",         icon: "emoji:1f3b5" },
  { id: "jug-robot",             cat: "juguetes",   es: "robot",             en: "robot",         icon: "emoji:1f916" },
  { id: "jug-videojuego",        cat: "juguetes",   es: "videojuego",        en: "video game",    icon: "emoji:1f3ae" },
  { id: "jug-pincel",            cat: "juguetes",   es: "pincel",            en: "paintbrush",    icon: "emoji:1f58c" },
  { id: "jug-regalo",            cat: "juguetes",   es: "regalo",            en: "gift",          icon: "emoji:1f381" },
  { id: "jug-trofeo",            cat: "juguetes",   es: "trofeo",            en: "trophy",        icon: "emoji:1f3c6" },
  { id: "jug-medalla",           cat: "juguetes",   es: "medalla",           en: "medal",         icon: "emoji:1f3c5" },
  { id: "jug-bandera",           cat: "juguetes",   es: "bandera",           en: "flag",          icon: "emoji:1f6a9" },
  { id: "jug-carrusel",          cat: "juguetes",   es: "carrusel",          en: "carousel",      icon: "emoji:1f3a0" },
  { id: "jug-castillo",          cat: "juguetes",   es: "castillo",          en: "castle",        icon: "emoji:1f3f0" },
];

export const CARD_CATEGORIES: CardCategory[] = Array.from(new Set(CARDS.map((c) => c.cat))) as CardCategory[];

export function cardById(id: string): Card | undefined {
  return CARDS.find((c) => c.id === id);
}
