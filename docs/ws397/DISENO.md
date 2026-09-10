# Sistema visual de la ws397

Este documento es el resultado del panel de diseño de 1.5.48: tres propuestas completas con
maquetas renderizadas a 480x800 en 1 bit, tres jueces independientes que las puntuaron en seis
ejes (legibilidad, coherencia, tinta electrónica, navegabilidad, belleza, costo) y los injertos
que los tres coincidieron en pedirle a la ganadora.

| Propuesta | Juez 1 | Juez 2 | Juez 3 | Total |
|---|---:|---:|---:|---:|
| **editorial** | 48 | 46 | 48 | **142** |
| tarjetas | 42 | 48 | 45 | 135 |
| instrumento | 43 | 41 | 41 | 125 |

Gana **editorial**: dos primeros puestos de tres y el único que no queda último en ningún eje.
Las maquetas de las tres están en el scratch de la sesión (`design/{editorial,instrumento,tarjetas}/*.png`).

## 1. Los cinco principios

1. **Nunca hay letras sobre trama.** Ni en la fila elegida, ni en un mosaico, ni en una celda de
   calendario, ni en un tablero. Si hace falta texto encima de algo tramado, va un plato blanco
   debajo (`drawTextPlate`).
2. **La jerarquía la hace la tipografía, no las cajas.** Una regla de 1 px separa mejor que un
   marco, y cuesta la centésima parte de tinta.
3. **Nada de negro macizo** salvo el texto, la pestaña del resalte y algún ícono chico. El negro
   grande deja fantasma en el refresco parcial siguiente.
4. **Un solo margen lateral de 24 px** y todo cae en una grilla de 8 px.
5. **Cada repintado se justifica.** Un parcial son 30 ms de panel y cada doce hay que hacer uno
   limpio. Una pantalla con números en vivo repinta como mucho dos veces por segundo, y solo si
   el número cambió lo suficiente para verse.

## 2. Escala tipográfica

| Rol | Fuente | Línea / mayúscula | Dónde |
|---|---|---|---|
| Display | `SevenSegment.h` | dígitos dibujados | temporizador, reproductor, magnitud del IMU, conversor |
| Título | **UI_14** (Ubuntu 14 bold) | 34 / 20 px | encabezados, hora del hub, marcadores, título de diálogo |
| Cuerpo | UI_12 (Ubuntu 12) | 29 / 17 px | filas de lista, opciones de diálogo |
| Etiqueta | UI_10 (Ubuntu 10) | 24 / 15 px | valores, metadatos, ayudas |
| Detalle | SMALL (NotoSans 8, **ahora con negrita**) | 23 / 12 px | pies, chips, números de versículo |
| Lectura | la cara que eligió el usuario en el lector | paso de renglón **40 px** | visores de texto largo |

UI_14 y la negrita de SMALL son nuevas en 1.5.48. Cuestan 122 KB de flash (85,5 % → 87,4 %) y se
generan con el mismo `convert-builtin-fonts.sh` que las demás: la regeneración de `ubuntu_12_bold`
con ese pipeline sale **byte a byte idéntica** a la commiteada, que es la prueba de que las nuevas
salieron del mismo molde. La negrita de SMALL tiene exactamente las mismas métricas que la regular
(23/18/-5), así que entra en los mismos renglones sin tocar ningún layout, y de paso arregla los
cuatro lugares que ya pedían SMALL + BOLD y pintaban regular sin avisar.

No se agrega UI_16: con UI_14 ya hay un escalón claro sobre UI_12 y son 90 KB más.

## 3. El resalte (`src/components/Selection.h`)

El defecto central que los tres jueces marcaron en la ganadora: la trama tapaba **toda** la fila,
o sea que el renglón sobre el que uno va a apretar OK era el menos legible de la pantalla.

El resalte tiene ahora tres partes y ninguna cae sobre el texto:

1. **Pestaña** negra de 5 px pegada al borde izquierdo. Es el único aviso que se sigue viendo
   cuando la trama se lava después de diez parciales, y a un metro se ve sin buscarlo.
2. **Marco** fino alrededor de toda la fila.
3. **Franjas** tramadas de 16 px en los márgenes, donde no hay letras.

El centro queda blanco y el texto, negro. Hay dos estilos: `Row` (listas) y `Tile` (mosaicos del
hub y cuadrículas), que no lleva pestaña y trama el campo del ícono en vez de los márgenes, con la
etiqueta sobre blanco.

Esto sigue cumpliendo lo que pidió el usuario en 1.5.43 —"no quiero que sea negro, mejor un gris o
algo no tan contrastante"— y arregla lo que ese cambio dejó a medias.

## 4. Componentes

- **Fila de lista**: dos renglones (título UI_12, detalle UI_10), casilla de 18x18 px a la
  izquierda cuando OK tilda, metadato alineado a la derecha en su columna. Margen inferior
  `buttonHintsHeight + verticalSpacing`, nunca solo el alto de los hints.
- **Diálogo de opciones**: título UI_14, opciones UI_12, ícono de 24 px por opción cuando existe, y
  un renglón de ayuda en UI_10 debajo de la última ("Atrás cierra sin hacer nada"). Sin ese
  renglón el diálogo es mudo: no dice cómo salir.
- **Paginador**: "Página 2 de 5" en UI_10 más una barra que se llena (carril de 1 px + tramo negro
  de 3 px). La palabra "Página" es lo que explica la barra; el folio "2 / 5" en una esquina no.
- **Ajuste de sí/no**: interruptor dibujado de 36x20 px, que un usuario mayor reconoce del
  teléfono, en vez de la palabra "Activado".
- **Medidor bipolar** (pantalla de Movimiento): 18 px de alto con marco de 2 px y marca en el cero.
  A 8 px es un hilo que no se ve.
- **Barra de botones**: cuatro huecos angostos. Ninguna etiqueta puede pasar de 70 px en UI_10; si
  se pasa, se acorta el string, no se achica la fuente.

## 5. Lo que se descartó de las otras dos propuestas

- **De instrumento, las versalitas** (SMALL en mayúsculas con tracking) como sistema de etiquetas:
  a un metro son textura, no texto. Es lo que le costó el último puesto.
- **De instrumento, el reloj en siete segmentos** en la barra de estado: el "1" de siete segmentos
  es una barra suelta y el reloj se lee "09:4 I". Los dígitos dibujados quedan para los números
  grandes que no son la hora.
- **De tarjetas, la sombra tramada** como señal de "esto está elegido": es la superficie que más
  contraste cambia de golpe al abrir y cerrar, y encima se mueve con cada golpe de palanca. La
  sombra queda solo para el diálogo, de 4 a 6 px.
- **De tarjetas, todo dentro de una caja**: ocho tarjetas en Notas y filas en cajas dentro de una
  caja en Música dejan el 60 % de la pantalla vacío.

## 6. Reglas de hardware que el diseño no puede ignorar

- **OK largo no existe.** En esta placa mantener OK apagaba el aparato hasta 1.5.47 y ahora el
  encendido es un botón aparte, pero `wasLongPressed(Confirm)` sigue sin llegar nunca. Ninguna
  función puede colgar de ahí.
- **ARRIBA y ABAJO son una palanca**: arriba XOR abajo, nunca las dos.
- **No hay teclado, nunca.** Todo texto que el usuario tenga que ingresar entra por voz.
- **El movimiento es una entrada más**: inclinar, sacudir, girar, boca abajo y doble golpe, con la
  salvedad de que el panel no aguanta movimiento continuo — un gesto es una jugada y un repintado.
