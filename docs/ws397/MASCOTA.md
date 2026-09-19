# Mascota: la perrita salchicha de bolsillo (`examples/Apps/mascota.lua`)

Un tamagotchi básico como app de Lua (`docs/ws397/APPS_LUA.md`). El pedido del dueño fue,
textual: *"hazme una pequeña mascota virtual, un tamagotchi básico con imágenes bonitas, no las
generes tú, busca de por ahí alguna que aparezca en la web"*, y después: *"pero quiero una perrita
salchicha"*. Así que **ningún dibujo es nuestro**: todos salen de fuentes abiertas que están en la
web, y esta página deja escrito de dónde, de quién y con qué licencia.

## La segunda versión (1.5.114): siluetas, nombre en pantalla y terminación

La primera versión (1.5.113) fue con sprites de 32 × 16 píxeles escalados ×7, y en el vidrio el
dueño la vio así: *"mascota se ve horrible, no parece ni parecido un salchicha, busca una imagen
mejor, pésima terminación, todo mal, encima se llama pipo, ni cambiarle el nombre, malísimo"*.
Tres cosas, y las tres se cambiaron:

1. **Dibujos**: en tinta de 1 bit un sprite de 32 píxeles a escala 7 son manchas. Ahora son
   **siluetas vectoriales** (CC0 de openclipart.org) reducidas a 150 px de ancho y pintadas a
   escala 2: **300 px de ancho en el vidrio**, con la forma entera del perro (cuerpo largo, patas
   cortas, oreja caída, cola).
2. **Nombre**: el bautizo por voz fallaba y la perra quedaba "Pipo" sin remedio. Ahora la primera
   vez hay una **pantalla** que pregunta ("¿Cómo se llama tu perrita?") con dos filas: *Decir el
   nombre* (micrófono) y *Se llama Pipo* (sin hablar). Una escucha que no se entiende **vuelve a
   esa pantalla** con "No entendí, intenta otra vez"; nunca bautiza sola. Y en **Info** hay una
   fila *Cambiar el nombre* que escucha otra vez.
3. **Terminación**: la pantalla principal sigue el sistema visual (`docs/ws397/DISENO.md`): margen
   de 24 px, grilla de 8, cabezal UI_14 con regla de 1 px (nombre · día), la perra centrada en una
   zona blanca de 208 px, las cuatro barras con marco de 1 px y relleno macizo (nunca trama bajo
   texto), un renglón de estado UI_12 y las acciones como **lista** con el resalte del sistema
   (`cp.selection`), no cajas sueltas.

## Los dibujos: de dónde salen

| Qué | Archivo | Autor | Licencia | Enlace |
| --- | --- | --- | --- | --- |
| La perrita parada (`quieta1`, `quieta2`, `feliz`, `triste`) y "se fue" (`sefue`) | **"Dachshund"** (openclipart 340936): silueta negra de perfil con collar | **eevee93** | **CC0 1.0** (todo openclipart es dominio público; la página del clip enlaza `creativecommons.org/publicdomain/zero/1.0/`) | https://openclipart.org/detail/340936/dachshund |
| La perrita dormida (`duerme`) | **"Resting/sleeping dog"** (openclipart 163699): perro enroscado durmiendo, silueta con las líneas interiores en blanco | **f_featherbrain** | **CC0 1.0** | https://openclipart.org/detail/163699/restingsleeping-dog |
| Los iconos de al lado de la perra (comida, pelota, balde, caca) y el huevo (2 cuadros) | **OpenCritter** (`gfx/bitmaps/icons`, `gfx/bitmaps/critters/egg_*.bmp`) | **SuperMechaCow** | **MIT** (Copyright (c) 2017 SuperMechaCow) | https://github.com/SuperMechaCow/OpenCritter |

CC0 no exige nada; MIT exige conservar el aviso de copyright, que va en el comentario de cabecera
de la app y acá. Ninguna restringe el uso comercial.

### Qué cuadro se usa para qué, y qué es transformación

De la silueta parada hay UN solo dibujo. Los cuadros son **transformaciones del original**, no
dibujos nuevos, y conviene decirlo: la cola —todo lo que queda a la izquierda de su raíz, en
x = 248 de los 1008 px del render— se **gira alrededor de la raíz** (pivote 248, 135):

| Cuadro | Qué es | Tamaño (px de bitmap → en el vidrio) |
| --- | --- | --- |
| `quieta1` | la silueta tal cual | 150 × 56 → 300 × 112 |
| `quieta2` | la cola 12° más arriba (el segundo cuadro de la animación) | 150 × 56 |
| `feliz` | la cola 38° arriba, casi horizontal | 150 × 56 |
| `triste` | la cola 14° más abajo, entre las patas | 150 × 56 |
| `duerme` | la silueta enroscada de f_featherbrain, con las líneas interiores engordadas antes de reducir (`MaxFilter(13)` sobre el render de 1200 px) para que sobrevivan a 120 px; la app le pone la "z Z" al lado | 120 × 81 → 240 × 162 |
| `sefue` | la silueta parada **espejada** y a 100 px: se va caminando para el otro lado | 100 × 38 → 200 × 76 |
| el huevo | `egg_idle.bmp` y `egg_main.bmp` de OpenCritter, a escala 6 | 32 × 32 → 192 × 192 |

No hay silueta de salchicha sentada ni echada con licencia libre en Wikimedia Commons, openclipart,
freesvg ni publicdomainvectors (se buscó en los cuatro; svgrepo devolvió 429 desde el sandbox).
Los dibujos lineales antiguos que sí hay (liftarn: "Tired dog", "Dog with bow"; j4p4n: "Dog
dreams") se descartaron: a 280 px de ancho el trazo se rompe y traen accesorios (un grifo, un
moño, una cama con huesos). El grabado de Pearson Scott Foresman (`Dachshund (PSF).png`, Commons,
dominio público) es precioso pero su tramado se vuelve ruido a 1 bit.

### Cómo se convirtieron

`convert2.py` (resvg-py + Pillow), guardado junto con los SVG en el scratchpad de la sesión
(`scratchpad/mascota/`, con los candidatos en `cand/`): render del SVG a 1200 px de ancho, recorte
al contenido, giro de la cola, reducción LANCZOS al ancho del bitmap, umbral a 128, recorte de las
filas blancas y empaquetado MSB primero, 1 = tinta, `ceil(w/8)` bytes por fila, como pide
`cp.image()`. Los cuatro cuadros parados son 1064 bytes cada uno, el dormido 1215, el ido 494:
**12,6 KB de hexa** en total y el archivo entero pesa **38,5 KB** (el tope es 64). Los iconos son
BMP de 1 bit y van tal cual (16 × 16 y 32 × 32). Todo se decodifica una vez con `gsub` sobre una
tabla de 256 entradas, que corre en C.

Vista previa de todas las pantallas (compuesta con un `cp` falso que anota las llamadas de dibujo y
Pillow con DejaVu en lugar de Ubuntu): `scratchpad/mascota/preview.png` (la casa) y
`scratchpad/mascota/preview/preview_*.png` (nombre, dormida, triste, Info, se fue).

## La pantalla principal

    y  40  cabezal: nombre (UI_14 negrita) · "Día N" a la derecha (UI_12)
    y  78  regla de 1 px, de margen a margen (24 → 456)
    y  96  zona de la perra, 208 px de alto; la silueta va centrada en los dos ejes
    y 312  estado (UI_12 negrita, centrado): "Está feliz", "Tiene hambre", "Duerme"…
    y 340  aviso de 4 s (UI_10, centrado): "Ñam, ñam", "Se durmió", "Te esperó 5 h"…
    y 376  cuatro barras cada 36 px: etiqueta UI_10 en x = 24, marco de 256 × 16 en x = 128
           con relleno macizo, número alineado a la derecha en x = 456
    y 536  cinco filas de 40 px: Alimentar · Jugar · Dormir/Despertar · Limpiar · Info,
           título UI_12 a 24 px del borde de la fila y metadato UI_10 a la derecha;
           la elegida lleva `cp.selection(24, y, 432, 40)`
    y 760  ayuda (UI_10): "Palanca: elegir · OK: hacer · Atrás: salir"

Las pantallas sin barras (el nombre, "se fue") suben su lista a y = 400. Todas las coordenadas son
enteras (`//`, nunca `/`): el `cp` del aparato rechaza un float con fracción, y el harness también.

## Cómo se juega

- **Cuatro necesidades** de 0 a 100: hambre (lleno = 100), ánimo, energía e higiene. Nace con 80.
- **Bajan con el tiempo real**: despierta, 8 / 6 / 5 / 4 puntos por hora; dormida, la energía
  **sube** 20 por hora y lo demás baja despacio (3 / 1 / 2), y se despierta sola al llegar a 100.
- **Con la app cerrada también pasa el tiempo**: se guarda el `epoch` del reloj (`cp.time()`) y
  al volver se descuenta lo que estuvo cerrada, **con tope de 12 h** (una semana fuera no la mata
  de golpe). Sin reloj (aparato sin hora), dentro de la app cuenta `cp.ms()`; entre aperturas no
  se descuenta nada, porque no hay con qué.
- **Descuido**: con el hambre o la higiene en cero (o ánimo y energía en cero a la vez) se
  acumula; con todo bien, se descuenta. **36 h de descuido y se va**: pantalla de despedida con
  los días que vivió y **OK trae un huevo nuevo** (que vuelve a preguntar el nombre).
- **Lista de acciones** (palanca elige, OK hace): **Alimentar** (+30 de hambre, −3 de higiene; llena
  no come), **Jugar** (mayor o menor: sale un dígito grande y hay que adivinar con ARRIBA/ABAJO si
  el próximo será mayor o menor, tres rondas; paga +8 de ánimo más +8 por acierto, cuesta 12 de
  energía y 5 de hambre; agotada o con mucha hambre no juega), **Dormir / Despertar** (despertarla
  con menos de 50 de energía la pone de mal humor), **Limpiar** (higiene a 100), **Info** (números,
  edad, cómo cuidarla, los créditos y la fila **Cambiar el nombre**).
- **Dormida no come, no juega ni se limpia**: hay que despertarla.
- **Gestos** (`cp.motion()`): **sacudir** la despierta de golpe (−10 de ánimo) o, despierta, la
  molesta (−5); **boca abajo** la duerme. Los demás gestos se ignoran.
- **El nombre**: la pantalla del nombre sale la primera vez y con cada huevo nuevo; *Decir el
  nombre* abre `cp.listen(6, "Di el nombre")` y de lo transcripto se toma la **primera palabra**
  (sacando un "se llama" delante), sin puntuación, con mayúscula y hasta 12 letras. Si no queda
  nada (Atrás durante la escucha, micrófono que falla, silencio), se queda en la pantalla con "No
  entendí, intenta otra vez". *Se llama Pipo* la bautiza sin hablar. En Info, OK escucha otra vez
  y "Ahora se llama Canela"; si no entiende, sigue como estaba.
- **Atrás sale** y guarda (sin mascota, sale sin más y la próxima vez vuelve a preguntar). Además
  guarda con cada acción y cada 5 minutos.
- **Cuándo repinta**: `on_tick` compara una firma de lo que se ve (cuadro, estado, aviso, barras
  redondeadas, día, nombre) y devuelve `true` sólo si cambió. La animación alterna cuadro **cada
  4 s**; los avisos duran 4 s. Con el aparato quieto, cincuenta ticks seguidos no repintan.

Estado guardado con `cp.save` (una línea, `;` como separador):
`mascota1;<nombre>;<hambre>;<ánimo>;<energía>;<higiene>;<duerme>;<descuido s>;<edad s>;<epoch>;<viva>`.
El formato no cambió respecto de 1.5.113: una mascota guardada sigue viva al actualizar.

## Probar sin aparato

`./test/lua_sandbox/run.sh` corre `test/lua_sandbox/scenarios/mascota.lua`: la pantalla del nombre
(una escucha que no entiende se queda; la segunda bautiza "Luna" limpiando "se llama Luna, la
perrita."), la perra en pantalla con el tamaño y los bytes que corresponden (`fake.images`: 150 × 56 a
escala 2, centrada, dentro de su zona; dormida 120 × 81; ida 100 × 38), alimentar, el juego entero,
dormir y despertar, limpiar, seis horas de reloj, recargar desde `cp.save`, el tope de 12 h con
30 h fuera, sin reloj, **cambiar el nombre desde Info** (una escucha vacía no lo toca; "Canela" sí
y se guarda), descuido largo hasta la despedida, huevo nuevo con el nombre de fábrica, Atrás durante
la escucha, el recorte del nombre largo, los dos gestos (prestándole un `cp.motion` al harness) y
cincuenta ticks quietos sin repintar.

## Instalar

Copiar `examples/Apps/mascota.lua` a `/Apps` de la tarjeta (modo memoria USB) y abrirla en
**Juegos → Mascota**. Pendiente de hardware: ver las siluetas en el vidrio (si la parada queda
grande o chica, `ESCALA` al principio de la parte de juego, 1 o 2; a 3 no entra en la zona), el
micrófono para el nombre y los dos gestos.
