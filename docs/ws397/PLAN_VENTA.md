# Plan para que la ws397 sea un producto vendible

Estado de partida: **1.5.70**. Todo lo de acá sale de lo hablado hasta esta versión, de las tres
fotos y el log que mandaste, y de la revisión externa. Nada de esto está implementado todavía.

Criterio de salida (lo que quiere decir "vendible"):

1. Un desconocido lo saca de la caja y lo deja andando **sin cable, sin teclado y sin que le
   expliquemos nada**.
2. Se puede decir **un número de autonomía** y que sea cierto.
3. El canal contra el servidor está **autenticado** (hoy no lo está).
4. El `.bin` que se vende se puede **volver a construir** mañana desde cero.
5. Nada de lo que tiene el aparato dice "Próximamente", falla en silencio ni miente.

Hoy fallan los cinco.

---

## Bloque A — Lo que impide vender

### A1. El aparato no autentica al servidor (F15)

`setInsecure()` sigue puesto: se cifra pero no se verifica ni el certificado ni el nombre del host.
Cualquiera en el medio se hace pasar por el servidor, se queda con el token del aparato, con lo que
se dicta por voz y con la descarga de OTA (o sea, le mete el firmware que quiera).

No alcanza con sacar el `setInsecure`. Hay tres cosas:

- Embeber las raíces (las de Railway y las de Let's Encrypt alcanzan; ~4 KB de flash).
- Llamar a `wolfSSL_check_domain_name` en `SecureClient` — hoy no se llama en ningún lado.
- **Poner el reloj en hora ANTES del primer TLS.** Hoy nadie llama a `settimeofday`, y un
  certificado se valida contra la fecha. Sin esto el arreglo deja al aparato sin red.
  Fuente de hora en orden: RTC PCF85063 → SNTP → el header `Date` de una respuesta HTTP simple.

Salida: el aparato se conecta con verificación completa, y si el reloj está en 1970 hace un
único paso de SNTP antes de abrir el primer TLS.

### A2. El registro público puede dar admin

Reportado por la revisión externa, sin verificar todavía contra `server/src/accounts.ts` y
`tenant.ts`. Primer paso es reproducirlo: crear una cuenta nueva por el formulario público en una
instancia limpia y ver qué `role` le queda. Si el primer usuario se hace admin por diseño, hay que
decidirlo explícitamente (variable de entorno `BOOTSTRAP_ADMIN_EMAIL`) en vez de que salga solo.

Mientras tanto: `/board/log` tiene los nombres de las redes WiFi y **todo lo que se dicta por voz**.
Confirmar que exige token siempre, y agregar un botón "borrar el log" en la web.

### A3. La batería: el reposo no está funcionando

En el log que mandaste esto se repite sin parar:

```
[REST] a reposar tras 45000 ms quieto (ciclo 2000 ms)
[REST] despertó por movimiento tras 2000 ms
```

O sea: **el aparato entra al reposo y sale a los 2 segundos, siempre.** El reposo de tres etapas,
que es todo el argumento de autonomía, hoy no existe: el aparato está a ~40 mA todo el tiempo.

Causa a confirmar: `IdleSleep::WAKE_DELTA_G = 0.06f` (60 mg) contra la suma de los tres ejes del
QMI8658. El ruido del propio sensor más la cuantización pueden dar eso con el aparato quieto sobre
la mesa. Y `motionMoved()` también devuelve true con **cualquier gesto latcheado** del chip, que
incluye los que el motor de golpes levanta solo.

Trabajo:

1. Medir el delta real con el aparato quieto durante 10 minutos y loguear el máximo (no hay que
   adivinar el umbral: hay que verlo).
2. Subir el umbral a lo que se midió × 3, y exigir **dos muestras seguidas** por encima antes de
   salir del reposo (un pico suelto no es que lo levantaron).
3. Separar "gesto latcheado" de "movimiento": el doble golpe sí tiene que despertar, la deriva del
   sensor no.
4. Bajar el sondeo: con el umbral bien puesto, el ciclo de 2 s puede ser de 4-5 s sin que se note.

Sin esto, cualquier número de autonomía que digamos es mentira.

### A4. No hay un número de autonomía

`BatteryLog` y `analyze()` ya están y ya se prueban de escritorio (`test/battery_drain/run.sh`).
Lo que falta es **correr las pruebas** una vez que A3 esté arreglado:

| Prueba | Cómo | Qué queremos saber |
|---|---|---|
| Reposo puro | cargado al 100 %, quieto, sin WiFi, 48 h | días en reposo |
| Lectura | leer sin parar hasta que se apague | horas de lectura |
| Voz + hub | 20 dictados/día durante 3 días | días de uso normal |
| Deep sleep | suspendido una semana | %/semana suspendido (**nunca se midió**) |

Salida: tres números en la caja y en la web. Si el de reposo no da al menos una semana, hay que
revisar qué queda prendido.

### A5. Recuperación de archivos incompleta

De la revisión externa, y es real: al arrancar no se mira el `.tmp` que dejó una escritura
interrumpida, y las lecturas se cortan a 50 KB. Un corte en medio de guardar el progreso del libro
o la caché del hub pierde el archivo. `SDCardManager::writeFile` ya escribe a `.tmp` y verifica
(F16), pero el rescate es sólo si el destino no está, y no hay barrido al arrancar.

Trabajo: barrido de `.tmp` en `setup()`, y sacar el tope de 50 KB o subirlo a lo que realmente
necesita el archivo más grande que leemos.

### A6. El firmware que se vende no se puede reconstruir

El submódulo `freeink-sdk/` apunta a un commit que GitHub no encuentra. Hoy no hay forma de volver
a construir el `.bin` desde cero. Esto es bloqueante de producción, no de funcionalidad: no se
puede vender un aparato cuyo firmware no se puede regenerar.

Trabajo:
- Publicar el commit del SDK en una rama del fork propio y reapuntar el submódulo.
- Regenerar **todos** los `.patch` de `docs/ws397/` con `git format-patch` (faltan tres).
- Un `build.sh` que clone limpio, aplique parches y compile, y correrlo una vez en frío.

### A7. La tarjeta que sale de fábrica está vacía

En el log:

```
[SDREG] Fonts directory not found
[DREG] No /dictionaries directory
```

El aparato que se vende tiene que salir con la tarjeta armada: `/fonts`, `/dictionaries`
(diccionario español), `/Apps` con los ejemplos de Lua, `/Music` y `/Books` vacíos pero creados, y
un libro de muestra. Hoy eso no existe ni como imagen ni como script.

Trabajo: `tools/tarjeta/` que arme la imagen, y que el aparato **cree las carpetas que faltan** en
el primer arranque en vez de loguear que no están.

### A8. PWR: errático, y la barrita

Dos cosas distintas, las dos con causa concreta.

**La barrita no carga: se dibuja exactamente dos veces.** En `handlePowerHold()` (main.cpp) hay
`bannerStage` 0 → 1 → 2 y nada más. El primer dibujo es a los 1200 ms (barra al 40 %), el segundo a
los 2300 ms (al 77 %) y después ya no se repinta hasta que se apaga a los 3000 ms. Nunca llega al
100 % y salta en dos escalones. El comentario del código dice por qué: *"second and last repaint:
ink is expensive"*. La solución no es repintar más seguido con un refresco completo: es dibujar
**sólo el rectángulo de la barra** con un parcial chico (la caja y el texto ya están en pantalla),
cada ~200 ms, que son 9 parciales de una región de 300×16 px. Eso sí se puede pagar.

**PWR errático: la hipótesis fuerte es que después de suspender PWR no enciende.** GPIO38 (la IRQ
del PMIC) no es RTC GPIO, así que del deep sleep despierta **OK**, no PWR. Pero después de un
apagado (3 s) PWR sí enciende, porque ahí actúa el PressOn del propio PMIC. Desde afuera: el mismo
botón a veces prende y a veces no. Eso es exactamente "errático".

Dos salidas posibles, hay que elegir con una medición:

- **(a) Suspender = light sleep indefinido** en vez de deep sleep. En light sleep GPIO38 sí
  despierta (es lo que ya destraba el reposo), no hay reset, el estado sigue vivo y vuelve
  instantáneo. Si el consumo en light sleep es el que creemos (~240 µA), 1500 mAh dan meses. Esto
  arreglaría el botón **y** haría que volver de suspendido sea inmediato en vez de un arranque.
  **Hay que medirlo antes de decidir.**
- **(b)** Dejar deep sleep y que la barrita diga "OK para volver a encender". Es honesto pero feo.

Además, del log hay que confirmar que no aparezca `press edge changes from … to …`: si la polaridad
se re-aprende en caliente, eso también se ve como errático. Para eso necesito el log de tres
pulsaciones seguidas: un toque, una de 1,5 s y una de 3,5 s.

---

## Bloque B — Lo que hace que se sienta terminado

### B1. Fotos afuera, el fondo de pantalla pasa a ser la pantalla de información

Decidido: **las fotos se van del sistema.**

- Se saca `PhotosActivity` y el mosaico Fotos (el hub baja de 13 a 12 mosaicos y la grilla queda
  más holgada, que es mejor).
- En el servidor se saca `photos.ts` completo (`/api/photos`, `/api/photos/file`,
  `/api/photos/preview`, `toDeviceBmp`, `bmpToPng`) y la pantalla Fotos de `/board`.
- **El fondo de pantalla pasa a ser la pantalla de suspendido con información**, la que maquetamos:
  hora del último repintado (chiquita, como sello, no como reloj vivo), **la próxima alarma en
  dígitos grandes** (que es el dato que no miente mientras duerme), el clima, el próximo
  recordatorio y el libro abierto. Se pinta una sola vez, en `paintWallpaperForSleep()`, justo
  antes de dormir — que es donde ya se pinta el fondo hoy.
- Se borra el ajuste de fondo de pantalla que elegía una imagen.

Recordatorio de por qué el reloj va chico: el panel es biestable y el sistema está muerto mientras
duerme. Un reloj grande que se ve vivo estaría mintiendo. Un reloj por minuto costaría ~63 mAh/día
y 60 refrescos completos.

### B2. La web: sacar todo de abajo de "Más"

Hoy son 5 pestañas y la quinta se comió Fotos, Noticias, Viajes, Memoria, Ajustes, Aparatos, IA,
Contenido, Log y la cuenta. Con Fotos afuera quedan nueve.

Propuesta: seis pestañas abajo — **Hoy · Agenda · Listas · Notas · Viajes · Ajustes**. Viajes sale
al primer nivel porque es lo único con contenido propio que se usa seguido. Y **Ajustes deja de ser
un cajón**: pantalla con secciones a la vista (Aparato, Voz y sonido, Clima y lugar, Noticias,
Memoria, Cuenta, y abajo Avanzado con IA / Contenido / Log para admin). Nada de un "Más" con diez
filas iguales.

Se mantiene la regla que ya está: **un solo estado**, se vuelve a pedir entero y se repinta después
de cada cambio.

### B3. Viajes reconoce un viaje que ya no existe

Encontré tres cosas que lo causan, y probablemente sean las tres a la vez:

1. **`getTrip(acc, id)` con id vacío devuelve `trips[0]`** (`server/src/trips.ts:96`). Si el
   aparato pregunta sin id porque se le limpió el suyo, el servidor le contesta con **otro viaje**
   como si fuera el que pidió. Nunca ve el 404.
2. **Las sugerencias no se purgan al borrar el viaje.** Quedan en `/data/suggest.json` con clave
   `trip:<id>:…` para siempre. `/api/trip/delete` borra el viaje, los eventos del calendario y los
   adjuntos, pero no las sugerencias.
3. **La caché del aparato** (`/.crosspoint/trips.json`) se muestra primero a propósito (para el
   aeropuerto sin red). `fetchTrips()` refresca la lista pero nunca compara `tripId` contra la
   lista nueva, así que el id viejo sobrevive hasta que alguien entra y se come el 404.

Arreglo: `getTrip` no inventa un viaje cuando le pidieron uno concreto (hoy ya devuelve null con id
explícito, pero `suggest.ts` llama con el id que venga y `/api/suggest/trip` cae en el fallback);
`/api/trip/delete` purga las entradas de `suggest.json` de ese viaje; y `fetchTrips()` limpia
`tripId` y `suggestTripId` si dejaron de estar en la lista.

### B4. La lectura va lenta

Del log: páginas de 2,5 a 4,8 s, con `display=2227ms` adentro. Más de la mitad es el panel. Hay que
medir qué forma de refresco está eligiendo el coordinador para una vuelta de página: si está
mandando HALF o FULL donde alcanzaba un parcial, ahí están los 2 s. `PAGE_TURN_CEILING = 24` y
`FAST_BEFORE_CLEAN = 12` hay que verificarlos contra lo que de verdad pasa al leer seguido.

Objetivo: menos de 1 s por página en el caso normal.

### B5. El OTA repinta cada 2 %

Una descarga de 5,7 MB con un parcial cada 2 % son 50 refrescos. Cambiar a cada 10 % (10
refrescos) o a barra por tiempo (uno cada 3 s). Es media hora de trabajo y se nota.

### B6. Primer arranque

Hoy no hay. El aparato arranca y muestra el hub sin WiFi, sin cuenta, sin lugar de clima y sin
idioma elegido. Para vender hace falta un asistente que corra una sola vez:

1. Idioma (los seis, con la palanca).
2. WiFi — **por el teléfono**, que ya está hecho en 1.5.70: levanta su punto de acceso y la clave
   se carga desde el navegador.
3. Vincular con la cuenta (el código de seis dígitos, ya está).
4. Lugar del clima por voz (ya está).
5. Una pantalla que enseñe los tres gestos que no se adivinan: **doble Atrás = hablar**, **Atrás
   mantenido = sincronizar**, **PWR mantenido = suspender**.

Sin el paso 5 nadie descubre la mitad del producto.

### B7. Documentación que miente

- `CLAUDE.md` describe el modo de energía como tres opciones con nombre (Ahorro / Normal / Siempre
  encendido). **Eso se sacó de la ws397 en 1.5.61**: hoy está forzado a 5 minutos y escondido.
  Consecuencia real: la red de seguridad de 30 minutos en `main.cpp` pregunta por
  `sleepTimeoutMs == 0`, y en la ws397 eso ya no puede pasar, así que **esa protección está
  muerta**. Hay que decidir si vuelve el selector o si la red de seguridad se reescribe.
- `docs/ws397/FUNCIONES.md` todavía lista los doce juegos compilados (se fueron en 1.5.63) y los
  gestos del IMU como pendientes (están hechos).

---

## Bloque C — Decisiones que faltan (tuyas)

1. **Tema visual.** Están maquetadas Lyra v2 y Diario v2, completas. Hay que elegir una, porque todo
   lo visual de abajo cuelga de eso. Si ninguna convence, es otra vuelta de maquetas antes de tocar
   código.
2. **Suspender: light sleep o deep sleep** (A8). Se decide con la medición, pero el criterio es
   tuyo: ¿preferís que PWR encienda siempre y volver sea instantáneo, aunque consuma un poco más?
3. **Selector de modo de energía** (B7): ¿vuelve a la ws397 o se queda forzado?
4. **Qué apps de Lua salen de fábrica.** Hoy `/Apps` va vacío. Con tres o cuatro buenas el mosaico
   Juegos deja de ser una promesa.

---

## Orden de trabajo

Las olas están ordenadas por lo que bloquea a lo que, no por lo que es más divertido.

| Ola | Versión | Qué entra |
|---|---|---|
| 1 | **1.5.71** | B3 viajes fantasma · B5 OTA · A8 barrita (el repintado de la barra) · B7 docs |
| 2 | **1.5.72** | A3 el reposo que no reposa + medición del umbral · A8 la decisión de suspender |
| 3 | **1.5.73** | A5 recuperación de archivos · A6 submódulo y parches · A7 imagen de tarjeta |
| 4 | **1.5.74** | A1 TLS de verdad · A2 permisos · borrar el log desde la web |
| 5 | **1.5.75** | B1 fotos afuera + fondo de pantalla con información |
| 6 | **1.5.76** | B2 la web sin "Más" |
| 7 | **1.5.77** | C1 el tema visual elegido, aplicado a todas las pantallas |
| 8 | **1.5.78** | B6 primer arranque · B4 velocidad de lectura |
| 9 | **1.6.0** | A4 las cuatro mediciones de batería · candidata de venta |

Las olas 1 a 4 son las que no se pueden saltear. De la 5 en adelante es lo que hace que se sienta
un producto y no un proyecto.

---

## Checklist de hardware antes de decir que está listo

Cada punto se prueba en el aparato, no en el log de la nube.

- [ ] PWR: toque, 1,5 s, 3,5 s — y la barrita llenándose parejo
- [ ] Suspender y volver, diez veces seguidas, con el botón que corresponda
- [ ] 48 h quieto sin perder más de lo medido
- [ ] Recordatorio que suena con el aparato suspendido
- [ ] Temporizador que sobrevive a salir, dormir y reiniciar
- [ ] Dictar en los seis idiomas y que vuelva bien
- [ ] Leer 200 páginas seguidas sin fantasma acumulado
- [ ] Sacar la tarjeta en caliente y volver a ponerla
- [ ] Quedarse sin batería leyendo y que al cargar abra donde estaba
- [ ] OTA de punta a punta
- [ ] Primer arranque completo con la tarjeta de fábrica, cronometrado

---

## Lo que NO entra en la versión de venta

Para que quede escrito y no vuelva a discutirse:

- Fotos (decidido acá).
- Pizarra de mensajes (se sacó en 1.5.44).
- Juegos compilados y tarjetas (se fueron en 1.5.63/65; si vuelven, vuelven en Lua).
- Teclado en pantalla, en cualquier forma.
- Radio por streaming, Spotify, lectura en voz alta de libros, chino, auto-rotación por IMU.
- Subida de archivos por la web (se carga por el modo memoria USB).
