# Apps de fábrica

Estas cinco son **las apps del producto**. Van en la tarjeta del aparato que se
vende (`docs/ws397/TARJETA_DE_FABRICA.md`) y el servidor las sirve en el paquete
de contenido (`server/src/assets.ts`, `FACTORY_APPS`), así que el aparato se las
baja solo cuando se actualiza. Para probarlas a mano, copia el archivo a `/Apps`
de la tarjeta (Ajustes → Sistema → Modo memoria USB) y ábrelo en Juegos.

| Archivo | Se llama | Qué es |
| --- | --- | --- |
| `ahorcado.lua` | Ahorcado | El juego de siempre, con abecedario en pantalla: palanca, OK y Atrás |
| `librito.lua` | Escritor | Dicta un tema y el servidor te escribe un libro entero en EPUB |
| `libros.lua` | Biblioteca | Di un título o un autor y te lo baja por el bot de Telegram |
| `mascota.lua` | Mascota | Un tamagotchi: una perrita salchicha que hay que cuidar |
| `sudoku.lua` | Sudoku | Sudoku con tres botones y nada de voz |

Decisión del dueño: **estas cinco y nada más.** Las otras cinco que había
(`reloj`, `tresenraya`, `contador`, `dados` y `viajes`) se borraron. El aparato
limpia de `/Apps` lo que el paquete le instaló alguna vez y ya no figura en el
manifiesto, así que sacar una de acá la saca también de las tarjetas que ya la
tienen; lo que el usuario copió a mano no se toca.

Las cinco se prueban de escritorio, sin aparato: **`./test/lua_sandbox/run.sh`**
las carga, les pega a todos los callbacks con el reloj puesto y sin el reloj
puesto, y juega partidas enteras (que es donde aparecen los índices fuera de
rango y los cursores que se cuelgan con el tablero lleno).

El contrato entero está en `docs/ws397/APPS_LUA.md`.
