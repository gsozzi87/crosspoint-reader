# Mascota: la mascota de bolsillo (`examples/Apps/mascota.lua`)

Un tamagotchi básico como app de Lua (`docs/ws397/APPS_LUA.md`). El pedido del dueño fue,
textual: *"hazme una pequeña mascota virtual, un tamagotchi básico con imágenes bonitas, no las
generes tú, busca de por ahí alguna que aparezca en la web"*. Así que **ningún dibujo es nuestro**:
todos salen de dos conjuntos abiertos que están en la web, y esta página deja escrito de dónde,
de quién y con qué licencia.

## Los dibujos: de dónde salen

| Qué | Conjunto | Autor | Licencia | Enlace |
| --- | --- | --- | --- | --- |
| El gato (8 cuadros: quieto ×2, contento ×2, triste ×2, dormida, se fue) | **Tiny Kitten Game Sprite** | **Segel** | **CC0** (dominio público) | https://opengameart.org/content/tiny-kitten-game-sprite (archivo `tiny_cat_sprite.zip`, 1,2 MB) |
| Los iconos de la botonera (comida, pelota, balde, info), la caca y el huevo (2 cuadros) | **OpenCritter** (`gfx/bitmaps/icons`, `gfx/bitmaps/critters/egg_*.bmp`) | **SuperMechaCow** | **MIT** (Copyright (c) 2017 SuperMechaCow) | https://github.com/SuperMechaCow/OpenCritter |

CC0 no exige atribución; MIT exige conservar el aviso de copyright, que va en el comentario de
cabecera de la app y acá. Ninguno de los dos restringe el uso comercial.

### Qué cuadro se usa para qué

El original de Segel es un gato de plataformas (Idle, Run, Jump, Hurt, Dead), no un tamagotchi,
así que los estados se mapean:

| Estado en la app | Cuadros del original |
| --- | --- |
| Quieta (`idle1`, `idle2`) | `01_Idle/__Cat_Idle_000` y `_006` |
| Contenta (`feliz1`, `feliz2`) | `03_Jump/01_Up/__Cat_JumpUp_002` y `02_Run/__Cat_Run_004` |
| Triste / con hambre / sucia / agotada (`triste1`, `triste2`) | `04_Hurt/__Cat_Hurt_001` y `_004` (los ojos `> <`) |
| Dormida (`duerme`) | `05_Dead/__Cat_Dead_001` (de pie, ojos cerrados) más una "z Z" en texto |
| Se fue (`sefue`) | `05_Dead/__Cat_Dead_007` (acostada) |
| Comiendo / limpiando / recién jugó | el cuadro que toque más el icono de OpenCritter al lado |
| El huevo de la pantalla del nombre | `egg_idle.bmp` y `egg_main.bmp` de OpenCritter, a escala 6 |

### Cómo se convirtieron

`convert.py` (Pillow), guardado junto con los originales en el scratchpad de la sesión
(`scratchpad/mascota/`): cada cuadro PNG (489 × 461, con transparencia) se compone sobre blanco,
se escala **por el alto del gato de pie a 120 px** (la misma escala para todos, así el que salta o
la acostada guardan la proporción), se **umbraliza en 70 sobre 255**: por debajo es tinta. El
gris del cuerpo queda blanco y sobrevive sólo el contorno negro, que es lo que se lee bien en
tinta electrónica (un tramado del gris se veía sucio). Se recorta al contenido, ancho múltiplo de
8, lienzo de 120 px alineado abajo. Resultado: **88 × 120** (la acostada, 128 × 120), 11 bytes por
fila, 1320 bytes por cuadro. Los iconos son BMP de 1 bit y van tal cual (16 × 16 y 32 × 32).

Todo se empaqueta MSB primero, 1 = tinta, como pide `cp.image()`, y va dentro del `.lua` como
strings hexa (`SPRITES` al principio del archivo): 23 KB de hexa, 45 KB el archivo entero (el tope
es 64). Se decodifican una vez con `gsub` sobre una tabla de 256 entradas, que corre en C.

**En pantalla el gato se dibuja a escala 2: 176 × 240 px** en el panel de 480 × 800 (unos 18 × 25 mm
a 235 ppp). Los iconos de la botonera a escala 2 (32 px) y los de al lado del gato a escala 3 (48 px).

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
  los días que vivió y **OK trae un huevo nuevo**.
- **Botonera** (palanca elige, OK hace): **Alimentar** (+30 de hambre, −3 de higiene; llena no
  come), **Jugar** (mayor o menor: sale un dígito grande y hay que adivinar con ARRIBA/ABAJO si el
  próximo será mayor o menor, tres rondas; paga +8 de ánimo más +8 por acierto, cuesta 12 de energía
  y 5 de hambre; agotada o con mucha hambre no juega), **Dormir / Despertar** (despertarla con menos
  de 50 de energía la pone de mal humor), **Limpiar** (higiene a 100), **Info** (números, edad,
  cómo cuidarla y los créditos).
- **Dormida no come, no juega ni se limpia**: hay que despertarla.
- **Gestos** (`cp.motion()`): **sacudir** la despierta de golpe (−10 de ánimo) o, despierta, la
  molesta (−5); **boca abajo** la duerme. Los demás gestos se ignoran.
- **Nombre por voz** la primera vez y con cada huevo nuevo: `cp.listen(6, "¿Cómo se llama?")`;
  se toma la primera palabra de lo transcripto (sacando un "se llama" delante), con mayúscula y
  hasta 12 letras. Si se cancela con Atrás, falla el micrófono o no se entiende: **Pipo**.
- **Atrás sale** y guarda. Además guarda con cada acción y cada 5 minutos.
- **Cuándo repinta**: `on_tick` compara una firma de lo que se ve (cuadro, estado, aviso, barras
  redondeadas, día) y devuelve `true` sólo si cambió. La animación alterna cuadro **cada 4 s**;
  los avisos duran 4 s. Con el aparato quieto, cincuenta ticks seguidos no repintan.

Estado guardado con `cp.save` (una línea, `;` como separador):
`mascota1;<nombre>;<hambre>;<ánimo>;<energía>;<higiene>;<duerme>;<descuido s>;<edad s>;<epoch>;<viva>`.

## Probar sin aparato

`./test/lua_sandbox/run.sh` corre `test/lua_sandbox/scenarios/mascota.lua`: nombre por voz, el
gato en pantalla con los bytes que corresponden (`fake.images`), alimentar, el juego entero,
dormir y despertar, limpiar, seis horas de reloj, recargar desde `cp.save`, el tope de 12 h con
30 h fuera, sin reloj, descuido largo hasta la despedida, huevo nuevo, el recorte del nombre, los
dos gestos (prestándole un `cp.motion` al harness) y cincuenta ticks quietos sin repintar.

Instrucciones por llamada, medidas con una sonda sobre `armStepLimit` (tope 400 000):
`on_open` ≈ 50 (sin estado) / 230 (con estado y 27 h fuera), `on_draw` ≈ 3 900 (casa, incluida la
decodificación única de los sprites), `on_tick` ≈ 160, `on_key` ≈ 50.

## Instalar

Copiar `examples/Apps/mascota.lua` a `/Apps` de la tarjeta (modo memoria USB) y abrirla en
**Juegos → Mascota**. Pendiente de hardware: verla en el vidrio (el tamaño a escala 2 es una
decisión a ojo; si queda chica, `dibujo(g, px, py, 2)` en `dibujarCasa` admite escala 3 y el
`py` baja), el micrófono para el nombre y los dos gestos.
