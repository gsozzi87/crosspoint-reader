# Mascota: la perrita salchicha de bolsillo (`examples/Apps/mascota.lua`)

Un tamagotchi básico como app de Lua (`docs/ws397/APPS_LUA.md`). El pedido del dueño fue,
textual: *"hazme una pequeña mascota virtual, un tamagotchi básico con imágenes bonitas, no las
generes tú, busca de por ahí alguna que aparezca en la web"*, y después: *"pero quiero una perrita
salchicha"*. Así que **ningún dibujo es nuestro**: todos salen de dos conjuntos abiertos que están
en la web, y esta página deja escrito de dónde, de quién y con qué licencia.

## Los dibujos: de dónde salen

| Qué | Conjunto | Autor | Licencia | Enlace |
| --- | --- | --- | --- | --- |
| La perrita (8 cuadros: quieta ×2, contenta ×2, triste ×2, dormida, se fue) | **openclaw-tamagotchi**, la mascota `dachshund` ("Alegra"): `pets/dachshund/sprites/{idle,walk,sleep}.png` | **Artem Katolikov** (`katolikov`) | **MIT** (Copyright (c) 2026 Artem Katolikov; el `LICENSE` del repo cubre todo lo que trae, y el README dice que el arte de las mascotas es propio, no sacado de OpenClaw) | https://github.com/katolikov/openclaw-tamagotchi (commit `4b3afac`, 2026-05-04) |
| Los iconos de la botonera (comida, pelota, balde, info), la caca y el huevo (2 cuadros) | **OpenCritter** (`gfx/bitmaps/icons`, `gfx/bitmaps/critters/egg_*.bmp`) | **SuperMechaCow** | **MIT** (Copyright (c) 2017 SuperMechaCow) | https://github.com/SuperMechaCow/OpenCritter |

MIT exige conservar el aviso de copyright, que va en el comentario de cabecera de la app y acá.
No restringe el uso comercial.

### Qué cuadro se usa para qué

El original es una perra salchicha negra de perfil (silueta con el ojo y las patas de otro
color), en celdas de 32 × 32 con tres animaciones: **idle** (8 cuadros, mueve la cola), **walk**
(6) y **sleep** (4, echada). No hay pose triste ni de muerta, así que se mapea:

| Estado en la app | Cuadros del original |
| --- | --- |
| Quieta (`idle1`, `idle2`) | `idle` 0 y 2 (la cola arriba y abajo) |
| Contenta (`feliz1`, `feliz2`) | `walk` 0 y 3 |
| Triste / con hambre / sucia / agotada (`triste1`, `triste2`) | `sleep` 0 y 2 (echada, sin la "z Z") |
| Dormida (`duerme`) | `sleep` 1 más una "z Z" en texto |
| Se fue (`sefue`) | `idle` 0 **dado vuelta** (patas para arriba): es una transformación del cuadro, no un dibujo nuevo |
| Comiendo / limpiando / recién jugó | el cuadro que toque más el icono de OpenCritter al lado |
| El huevo de la pantalla del nombre | `egg_idle.bmp` y `egg_main.bmp` de OpenCritter, a escala 6 |

### Cómo se convirtieron

`convert.py` (Pillow), guardado junto con los originales en el scratchpad de la sesión
(`scratchpad/mascota/`, con el clon del repo en `orig/openclaw-tamagotchi`): de cada celda de
32 × 32 se recortan las filas 8 a 23 (la perra vive ahí) → **32 × 16**, y se pasa a 1 bit **por
paleta, no por umbral**: el cuerpo `(35,28,25)` y las patas `(175,100,45)` son tinta y el ojo
`(10,8,6)` queda blanco — por luminancia el ojo es más oscuro que el cuerpo y desaparecía. Son 4
bytes por fila, **64 bytes por cuadro**, 1856 caracteres hexa en total; el archivo entero pesa
**24 KB** (el tope es 64). Los iconos son BMP de 1 bit y van tal cual (16 × 16 y 32 × 32). Todo
empaquetado MSB primero, 1 = tinta, como pide `cp.image()`, y decodificado una vez con `gsub`
sobre una tabla de 256 entradas, que corre en C.

**En pantalla la perra se dibuja a escala 7: 224 × 112 px** en el panel de 480 × 800 (unos 24 × 12 mm
a 235 ppp), con la pinta de píxel grande que le corresponde. Los iconos de la botonera a escala 2
(32 px) y los de al lado de la perra a escala 3 (48 px).

### Lo que se miró y se descartó

- **"Dachshund" de zwonky, OpenGameArt, CC0** (16 × 16, sit/stand/run): licencia perfecta, pero a
  1 bit un perro de 16 px con tres colores queda en dos manchas; no se reconoce. Está bajado en
  `orig/zwonky-dachshund.png` por si el dueño prefiere ese estilo a color en otra pantalla.
- **Tiny Kitten de Segel (CC0)**: era la versión anterior de esta app (un gato); se cambió a pedido.
- **picotamachibi** (Kevin McAleer): sprites de 1 bit perfectos para tamagotchi, **sin licencia**.
- **Matagotchi / Dragotchi** (Flipper): GPL-3. **Pet Mobile de ToffeeCraft**: sólo uso personal.
  **Pixel Dogs de Benvictus**: licencia informal ("créditos si no donas") y sin salchicha declarado.
- No hay salchicha en los packs de animales de Kenney (Animal Pack Redux trae "dog" genérico).

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
perra en pantalla con los bytes que corresponden (`fake.images`), alimentar, el juego entero,
dormir y despertar, limpiar, seis horas de reloj, recargar desde `cp.save`, el tope de 12 h con
30 h fuera, sin reloj, descuido largo hasta la despedida, huevo nuevo, el recorte del nombre, los
dos gestos (prestándole un `cp.motion` al harness) y cincuenta ticks quietos sin repintar.

Instrucciones por llamada, medidas con una sonda sobre `armStepLimit` (tope 400 000):
`on_open` ≈ 50 (sin estado) / 230 (con estado y 27 h fuera), `on_draw` ≈ 3 900 (casa, incluida la
decodificación única de los sprites), `on_tick` ≈ 160, `on_key` ≈ 50.

## Instalar

Copiar `examples/Apps/mascota.lua` a `/Apps` de la tarjeta (modo memoria USB) y abrirla en
**Juegos → Mascota**. Pendiente de hardware: verla en el vidrio (la escala 7 es una decisión a ojo:
`ESCALA` al principio de la parte de juego, de 1 a 8), el micrófono para el nombre y los dos gestos.
