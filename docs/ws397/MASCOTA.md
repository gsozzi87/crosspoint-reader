# Mascota: la perrita salchicha de bolsillo (`examples/Apps/mascota.lua`)

Un tamagotchi básico como app de Lua (`docs/ws397/APPS_LUA.md`). El pedido del dueño fue,
textual: *"hazme una pequeña mascota virtual, un tamagotchi básico con imágenes bonitas, no las
generes tú, busca de por ahí alguna que aparezca en la web"*, y después: *"pero quiero una perrita
salchicha"*. Así que **ningún dibujo es nuestro**: todos salen de fuentes abiertas que están en la
web, y esta página deja escrito de dónde, de quién y con qué licencia.

## La tercera versión: caricatura CON CONTORNO, no una mancha

La segunda versión eran **siluetas negras macizas** y el dueño las rechazó con todas las letras:
*"la mascota salchicha se ve pésimo, busca una caricatura de salchicha, algo más bonito, no esa
mancha negra, no quiero mancha quiero que esté dibujado el contorno tipo caricatura"*. Es la
tercera vez que rechaza el dibujo, así que la regla ahora está escrita: **la perra va dibujada con
línea de contorno y el interior blanco** — ojo, hocico, oreja, patas y cola que se lean —, nunca un
pictograma relleno.

**Todos los cuadros salen de UN SOLO dibujo**, y eso importa tanto como el contorno: en la primera
versión de esta tanda la dormida venía de otro autor y parecía otro perro (hocico en punta, orejas
paradas). La mascota tiene que ser **el mismo personaje** en los cinco cuadros, así que la dormida
también sale del mismo dibujo.

### Por qué el dibujo de línea obliga a trabajar cerca de la resolución final

Una silueta maciza se puede reducir todo lo que uno quiera: sigue siendo una mancha con la forma
correcta. Un trazo de contorno no: si al reducir queda de medio píxel, el umbral lo rompe y la
línea aparece cortada. Por eso el cuerpo se pinta a **escala 2** desde un bitmap de 187 px (374 px
en el vidrio) y no a escala 6 desde uno de 32, y por eso el trazo del SVG se **engorda** antes de
reducir (`STROKE = 1.75` contra el 1.417 del original) hasta que en el bitmap final mide 2 px, que
en el vidrio son 4. Se probaron anchos de 150 a 256 con trazos de 1,4 a 3,1 y se miraron todos.

## Los dibujos: de dónde salen

| Qué | Autor | Licencia | Enlace |
| --- | --- | --- | --- |
| **La perra entera**, los cinco cuadros (`cuerpo` + las seis colas, `duerme`, `sefue`) | **oksmith** (subido de publicdomainq.net) | **CC0 1.0** (todo openclipart es dominio público) | https://openclipart.org/detail/315296/dachshund |
| Los iconos de al lado de la perra (comida, pelota, balde, caca) y el huevo (2 cuadros) | **SuperMechaCow**, proyecto **OpenCritter** (`gfx/bitmaps`) | **MIT** (Copyright (c) 2017 SuperMechaCow) | https://github.com/SuperMechaCow/OpenCritter |

CC0 no exige nada y **permite modificar** (que es lo que hacemos: doblar la cola, cerrar los ojos,
espejar); MIT exige conservar el aviso de copyright, que va en el comentario de cabecera de la app,
en la pantalla de Info y acá. Ninguna restringe el uso comercial.

**Por qué ese dibujo y no otro.** El SVG de oksmith trae, por cada forma, un camino **relleno** y
otro **sólo de trazo** (`fill-opacity="0" stroke="#1A1919"`). Poniendo los rellenos en **blanco** y
dejando los trazos negros queda exactamente el dibujo de línea, y los rellenos blancos siguen
tapando lo que va detrás, así que las patas del otro lado no se transparentan. Los dos únicos
rellenos que quedan negros son los puntos de los ojos.

Se miraron y se descartaron: 317830 y 325158 (dachshunds de openclipart sin trazos: son formas
oscuras apiladas, el "contorno" es el borde de la de abajo y a 1 bit quedan manchas); 194259,
276116 y 311178 (color plano sin línea); 32473 y `Dachshund (PSF)` de Commons (grabados: el tramado
se vuelve ruido); 337983, 337986 y 334378 (línea hecha a mano, con moños, grifos y birretes);
**329276 "Dog dreams" de liftarn** (CC0, una salchicha dormida en su camita, que llegó a estar en
una versión de esta tanda) — se cayó porque **es otro perro**: al lado de la de oksmith parece un
zorro y rompe lo único que la mascota no puede romper, que es ser siempre la misma.

### Qué cuadro se usa para qué, y qué es transformación

De la perra hay **un solo dibujo**. Todos los cuadros son **transformaciones** de ese original:

| Cuadro | Qué es | Tamaño (bitmap → en el vidrio) |
| --- | --- | --- |
| `cuerpo` | el dibujo tal cual, con el hueco de la cola en blanco | 187 × 103 → 374 × 206 |
| `cola_quieta1` / `cola_quieta2` | la cola a 0° y a −12°: la animación tranquila | 27 × 32 (parche) |
| `cola_feliz` / `cola_feliz2` | la cola a −46° y −30°: **las dos arriba** | 27 × 32 |
| `cola_triste` / `cola_triste2` | la cola a +22° y +11°: **las dos abajo** | 27 × 32 |
| `duerme` | la misma perra con los **ojos cerrados** (los dos puntos aplastados a una rayita: `scale(1.9, 0.40)` alrededor de su centro) y la cola caída; la app le pone la "z Z" al lado | 187 × 103 → 374 × 206 |
| `sefue` | la misma **espejada** y más chica, yéndose; se rinde con el trazo más grueso (2.2) para que al achicarla no adelgace | 140 × 78 → 280 × 156 |
| el huevo | `egg_idle.bmp` y `egg_main.bmp` de OpenCritter, a escala 6 | 32 × 32 → 192 × 192 |

**La cola se DOBLA, no se recorta y se pega.** Un recorte girado deja una juntura en la base, que a
este tamaño se ve como un error de dibujo. Acá el giro **crece con la distancia a la raíz**: es 0
hasta 46 px (del render de 900) y entero a partir de 145, así que los píxeles de la base **no se
mueven ni uno** y la unión con el lomo es continua; lo que se ve es la cola doblándose, que es lo
que hace una cola. El polígono que la selecciona está medido a mano sobre el render de 900 px
(`cand3/tail_zoom2.png`) y excluye la línea del muslo, que pasa a 12 px de la cola.

**Por qué los dos cuadros de cada humor van para el mismo lado**: si uno de los dos fuera el neutro,
la mitad del tiempo la perra contenta se vería idéntica a la tranquila, y el juego distingue esos
dos estados. Contenta mueve la cola entre −46° y −30°; triste, entre +22° y +11°.

**El cuerpo se guarda una sola vez.** Los seis cuadros de pie son el mismo cuerpo con otra cola, así
que se guarda el cuerpo (con el hueco de la cola en blanco) y encima se pinta el **parche** de la
cola que toca, en `COLA_X, COLA_Y`. `cp.image` no pinta los ceros, así que uno no borra al otro. Las
seis colas juntas pesan 768 bytes, contra los ~15 KB que costarían seis cuerpos enteros.

### Cómo se convirtieron

`convert3.py` (las fuentes y las transformaciones) y `gen_final.py` (arma la tabla `SPRITES` y la
hoja de contacto), guardados con los SVG en el scratchpad de la sesión (`scratchpad/mascota/`, los
candidatos en `cand/`): render del SVG a 1800 px de ancho con los rellenos en blanco, doblado de la
cola, recorte **común a todos los cuadros** (para que la perra no salte al cambiar de humor),
reducción `BOX` al ancho final, umbral en 190 y empaquetado MSB primero, 1 = tinta, `ceil(w/8)`
bytes por fila, como pide `cp.image()`. El cuerpo y la dormida son 2472 bytes cada uno, cada cola
128 y la que se va 1404: **15,0 KB de hexa** en total, y el archivo entero pesa **42,2 KB** (el tope
del cargador es 64). Los iconos son BMP de 1 bit y van tal cual. Todo se decodifica una vez con
`gsub` sobre una tabla de 256 entradas, que corre en C: `on_open` entra con 2.000 instrucciones y
`on_draw` con 4.000, contra el tope de 400.000.

**La vista previa se mira antes de entregar.** `preview/driver.c` corre la app de verdad con un `cp`
falso que anota cada llamada de dibujo y `preview/render.py` las pinta como un panel de 480 × 800:
`scratchpad/mascota/preview/sheet.png` es la hoja con las ocho pantallas (los dos cuadros de la
animación contenta, tranquila, dormida, comiendo, el nombre, Info y la despedida) y
`preview/preview_*.png` cada una suelta. Si el contorno se rompe o parece una mancha, se ve ahí.

## La pantalla principal

    y  40  cabezal: nombre (UI_14 negrita) · "Día N" a la derecha (UI_12)
    y  78  regla de 1 px, de margen a margen (24 → 456)
    y  96  zona de la perra, 208 px de alto; el dibujo (374 × 206) va centrado en los dos
           ejes y la llena: por eso lo que la acompaña va en las ESQUINAS DE ARRIBA, que es
           donde el dibujo tiene blanco — la "z Z" o el icono de lo que acaba de pasar
           (comida, pelota, balde) a la derecha, la caca a la izquierda
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

La **pantalla del nombre** (desde 1.5.114) sale la primera vez y con cada huevo nuevo: antes el
bautizo por voz fallaba y la perra quedaba "Pipo" sin remedio, así que ahora es una pantalla con
dos filas —*Decir el nombre* y *Se llama Pipo*— y una escucha que no se entiende **vuelve a
preguntar**; nunca bautiza sola. Y en **Info** hay una fila *Cambiar el nombre*.

Todo sigue el sistema visual (`docs/ws397/DISENO.md`): margen de 24, grilla de 8, cabezal UI_14 con
regla de 1 px, barras con marco de 1 px y relleno macizo (nunca trama bajo texto) y las acciones
como lista con el resalte del sistema (`cp.selection`), no cajas sueltas.

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
  4 s** (y lo único que cambia entre los dos es la cola, así que el parcial es chico); los avisos
  duran 4 s. Con el aparato quieto, cincuenta ticks seguidos no repintan.

Estado guardado con `cp.save` (una línea, `;` como separador):
`mascota1;<nombre>;<hambre>;<ánimo>;<energía>;<higiene>;<duerme>;<descuido s>;<edad s>;<epoch>;<viva>`.
El formato no cambió respecto de 1.5.113: una mascota guardada sigue viva al actualizar.

## Probar sin aparato

`./test/lua_sandbox/run.sh` corre `test/lua_sandbox/scenarios/mascota.lua`: la pantalla del nombre
(una escucha que no entiende se queda; la segunda bautiza "Luna" limpiando "se llama Luna, la
perrita."), la perra en pantalla con el tamaño y los bytes que corresponden (`fake.images`: el
cuerpo 187 × 103 a escala 2, centrado, dentro de su zona, **con el parche de la cola encima** en
`COLA_X, COLA_Y`; dormida 187 × 103 y sin parche; ida 140 × 78), alimentar, el juego entero,
dormir y despertar, limpiar, seis horas de reloj, recargar desde `cp.save`, el tope de 12 h con
30 h fuera, sin reloj, **cambiar el nombre desde Info** (una escucha vacía no lo toca; "Canela" sí
y se guarda), descuido largo hasta la despedida, huevo nuevo con el nombre de fábrica, Atrás durante
la escucha, el recorte del nombre largo, los dos gestos (prestándole un `cp.motion` al harness) y
cincuenta ticks quietos sin repintar.

## Instalar

Copiar `examples/Apps/mascota.lua` a `/Apps` de la tarjeta (modo memoria USB) y abrirla en
**Juegos → Mascota**. Pendiente de hardware: ver el contorno en el vidrio —si el trazo de 4 px
quedara fino o grueso hay que volver a generar los bitmaps con otro `STROKE`, no cambiar `ESCALA`,
que a 1 la perra queda diminuta y a 3 no entra en la zona—, el micrófono para el nombre y los dos
gestos.
