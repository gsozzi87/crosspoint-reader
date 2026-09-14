# Apps de ejemplo y de fábrica

Copia estos archivos a `/Apps` de la tarjeta (Ajustes → Sistema → Modo memoria
USB) y ábrelos en Juegos.

Las **tres de fábrica** van en la tarjeta del aparato que se vende
(`docs/ws397/TARJETA_DE_FABRICA.md`); las dos de ejemplo están para leerlas.

| Archivo | De fábrica | Qué muestra |
| --- | --- | --- |
| `reloj.lua` | sí | `cp.time()`, dígitos de segmentos dibujados a mano y **cuándo** repintar en tinta |
| `ahorcado.lua` | sí | Texto y medidas, `cp.selection()`, estado guardado con `cp.save`/`cp.load` |
| `tresenraya.lua` | sí | `cp.selection()` como cursor, una IA chica, tres botones para todo |
| `contador.lua` | no | Lo mínimo: los cuatro callbacks, `cp.save`/`cp.load` |
| `dados.lua` | no | Dibujo con `cp.rect`, gestos con `cp.motion()`, `on_tick` |

Las cinco se prueban de escritorio, sin aparato: **`./test/lua_sandbox/run.sh`**
las carga, les pega a todos los callbacks con el reloj puesto y sin el reloj
puesto, y juega partidas enteras de ahorcado y de tres en raya (que es donde
aparecen los índices fuera de rango y los cursores que se cuelgan con el tablero
lleno).

El contrato entero está en `docs/ws397/APPS_LUA.md`.
