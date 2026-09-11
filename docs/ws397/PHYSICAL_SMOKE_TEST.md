# Prueba física de cada release (ws397)

Lista de verificación **en el aparato**, para correr después de cada release o de cualquier cambio transversal
(refresco, energía, audio, IMU, tema). En la nube no hay hardware: esta lista es lo que el usuario prueba y lo que
reporta. Marcá lo que falle con el número de paso y, si hay, la línea del log de `/board/log`.

Convención de botones: **ARRIBA/ABAJO** = palanca, **OK** = confirmar, **ATRÁS** = BOOT, **PWR** = botón de encendido
(el del PMIC). "Mantener" = ≥ 1 s salvo que se diga otra cosa.

## 1. Arranque y actualización
1. Ajustes → Buscar actualizaciones instala la versión nueva por OTA y el aparato reinicia solo.
2. El arranque llega al hub sin reinicios en bucle; la versión en Ajustes → Sistema es la esperada.
3. El primer pintado del hub es **un solo destello** (refresco limpio), sin fantasma del logo.
4. `/board/log` muestra el arranque con la razón de encendido (`encendido`, `boton`, `temporizador`) y una sola
   línea de memoria cada 10 s (`[MEM] loop: interna libre …`), sin ráfagas de `pixeles fuera de pantalla`.

## 2. Botones y energía
1. **PWR corto**: la pantalla hace un refresco limpio completo (se ve el destello) y vuelve donde estaba.
2. **PWR mantenido**: a los 0,6 s aparece la barra de apagado; a los 3 s el aparato duerme. Soltar antes de los 3 s
   cancela y la pantalla vuelve a repintarse sola.
3. Mientras la barra está en pantalla, **la música sigue sonando** (no se congela).
4. **OK mantenido** ya NO apaga: en una lista, mantener OK no hace nada raro (sólo confirma al soltar).
5. Dormido, **OK corto** despierta; ARRIBA/ABAJO/ATRÁS no despiertan. Un roce de OK de menos de 10 ms no despierta.
6. Con un recordatorio o temporizador pendiente, dormir y esperar: el aparato despierta solo, suena y muestra el alerta.
7. En `/board/log` al arrancar hay una línea con los registros del PMIC (`PWRON 0x20`, `PWROFF 0x21`, `0x22`, `0x27`).
8. Autonomía: anotar el % de batería antes de dormir y a las 12 h.

## 3. Pantalla y refresco
1. Navegar 20 veces seguidas entre hub → Recordatorios → hub → Notas → hub: en algún momento (cada ~12 parciales)
   aparece **un** refresco limpio solo, y nunca se acumula fantasma gris de fondo.
2. Abrir un menú de opciones (OptionPopup) y mover el cursor 10 veces: el fondo NO se repinta entero (no hay destello
   del fondo) y al cerrar el fondo queda intacto.
3. En el hub, esperar a que cambie el minuto: el reloj se actualiza sin que cambie nada más; si no cambió nada, no hay
   ningún refresco (en el log: `refresh salto por sombra`).
4. "Próximamente" en un mosaico: un solo refresco, no dos.
5. Con Suavizado de texto activado, una respuesta de Hablar se ve en 4 grises nítidos; con el siguiente parcial no
   queda fantasma del texto.
6. Ajustes → Pantalla → fadingFix activado: el primer pintado tras dormir sigue siendo limpio (no fantasmea).

## 4. Voz y audio
1. Hablar (doble ATRÁS): decir una frase **arrancando a hablar en el mismo instante en que suena el pitido**: la
   transcripción tiene la primera palabra completa.
2. Apretar y soltar sin hablar: "no se escuchó nada", sin consumir una llamada al servidor.
3. Notas → Nueva nota de voz: grabar, se reproduce sola al terminar; ATRÁS descarta (no queda archivo), OK guarda, ARRIBA
   la vuelve a reproducir. La nota guardada se reproduce desde la lista.
4. Notas → Nueva nota (dictada): la pantalla muestra `0:07 / 1:30`; una nota de 60 s se transcribe entera.
5. Volumen desde Ajustes → Sistema → Volumen cambia la voz, los pitidos y la música por igual.
6. Música: reproducir, salir al hub (sigue sonando, se ve en la barra), entrar a Hablar (la música se corta antes de
   grabar y no sale "Falló la captura del micrófono").
7. Ajustes → Prueba de audio: nivel pico > 30 % hablando a 15 cm.

## 5. Movimiento (IMU)
1. Ajustes → Sistema → Movimiento: **primero "Calibrar ejes"** (tres pasos: apoyado boca arriba, inclinado a la
   derecha, inclinado hacia uno). Sin eso, "inclinar a la derecha" puede ser cualquier eje. Anotar qué muestra la
   línea de montaje al terminar (por ejemplo `n=+Z x=-X y=+Y`).
2. En la misma pantalla: sacudir dos veces da "sacudir"; inclinar 30° da "inclinar" con la dirección correcta;
   apoyar plano da "horizontal"; dos golpecitos con el dedo en la tapa dan "doble golpe" (si no aparece, anotar si la
   pantalla dice que el motor de golpes responde o no).
3. Con un recordatorio sonando, dar vuelta el aparato boca abajo: se pospone y se calla.
4. Grabando en Hablar, sacudir: se cancela la grabación.
5. En el hub, dos golpecitos: se abre Hablar.
6. Juegos → Laberinto: cada inclinación hace rodar la bolita hasta la próxima bifurcación (es por turnos, no se mueve
   sola); Juegos → 2048: una inclinación firme empuja las fichas. Los dos se juegan igual con la palanca y OK.
7. Dormido, mover el aparato no lo despierta ni gasta batería (§2.8).
8. Hub: el widget del clima muestra "Interior NN° · NN %" aunque no haya lugar cargado.

## 6. Visual
1. En ninguna pantalla hay bloques negros macizos con texto blanco (salvo la barra de título de Música).
2. Lo elegido se ve con **pestaña negra a la izquierda, marco, y las franjas grises SOLO en los márgenes**: el texto
   de la fila elegida queda sobre blanco y se lee igual de bien que el resto. Vale para hub, Recordatorios, Notas,
   Calendario, Juegos, Ajustes, Explorador de archivos y los menús de opciones. **En ninguna pantalla puede haber
   letras encima de la trama.**
6. Los títulos y encabezados se ven un escalón más grandes que el cuerpo de las listas (fuente UI_14 nueva); los pies
   en negrita se distinguen de los pies normales.
3. Márgenes y encabezados iguales en todas las pantallas nuestras; Ajustes se ve del mismo estilo.
4. Textos largos (ruso, alemán) se truncan con "…" sin salirse del ancho; nada escribe fuera de la pantalla (log sin
   `pixeles fuera de pantalla`).
5. Hub: iconos de 64 px, etiquetas siempre en negrita, "Recordatorios" entero.

## 7. Funciones (regresión rápida)
1. Recordatorios: tildar, mover a Compras/Tareas, borrar; Notas: crear, abrir, borrar.
2. Mi día: dictar "a las 8 gimnasio, a las 9 reunión": aparecen las dos; editar la hora; borrar una.
3. Biblia: abrir Juan 4; ATRÁS mantenido → Preguntar: "no entendí del 8 al 12".
4. Noticias: abrir un artículo; Leer en voz alta lo lee entero, ARRIBA/ABAJO saltan de trozo.
5. Traductor: una frase de ida y otra de vuelta.
6. Clima: pantalla detallada con los seis días y los iconos.
7. Vincular con mi cuenta: muestra el código de seis dígitos (o "falta DATABASE_URL" si el servidor no tiene base).
8. Modo memoria USB: con el cable, la tarjeta aparece en la computadora; al salir, el aparato sigue funcionando.
9. Conversor de unidades: 10 km → millas.

## 8. Lector (upstream)
1. Abrir un EPUB, pasar 20 páginas: refresco limpio según la cadencia de Ajustes; volver al hub y "Continuar leyendo"
   abre en la misma página.
2. Preguntarle al libro: pregunta por voz y respuesta paginada.

## 9. Reposo, alarma del RTC y menú de PWR (1.5.48, lo más nuevo)

**Reposo (light sleep).** Es lo más difícil de ver, porque cuando funciona no se nota nada.

1. Dejar el aparato quieto en el hub **45 segundos** sin tocarlo. La pantalla no cambia (es lo correcto: el panel
   retiene la imagen). Apretar cualquier botón: tiene que responder **al instante**, sin parpadeo ni arranque.
   Si tarda o si la pantalla se rehace entera, no estaba reposando: estaba durmiendo de verdad.
2. Ajustes → Sistema → **Memoria**, sección Reposo: tiene que decir cuántos ciclos lleva y cuánto tiempo estuvo.
   Si dice **"todavía no entró"** después de un rato quieto, hay algo bloqueándolo siempre — mirar el log
   (`/board/log`, etiqueta `REST`).
3. Con el aparato quieto y reposando, **levantarlo**: se despierta solo. En el log, `despertó por movimiento`.
4. Apretar **PWR** con el aparato reposando: abre el menú de pantalla. (Esto es lo que prueba que la IRQ del PMIC
   despierta: PWR no es un GPIO.)
5. Poner música y dejarlo quieto un minuto: **no** tiene que reposar (la música se cortaría). Igual con el cable
   USB enchufado y con el modo memoria USB.
6. Batería: anotar el porcentaje, dejarlo quieto una hora en el hub y volver a anotar. Contra 1.5.47 la caída
   tiene que ser bastante menor. **Es la medición que más importa de este release.**

**Alarma del RTC.**

7. Al arrancar, el log tiene que decir `alarma del RTC lista`. Si en cambio dice `INT del RTC (GPIO45) en bajo al
   arrancar`, la alarma no se va a usar y hay que avisar: ese pin quedaría rechazando el reposo.
8. Poner un recordatorio para dentro de **10 minutos** y dejar el aparato quieto. El log dice `alarma armada
   para …`. A los 10 minutos tiene que sonar sin que nadie lo toque.
9. Lo mismo con un temporizador de 3 minutos: salir del temporizador (Atrás, que lo deja corriendo), dejarlo
   quieto y esperar.

**Menú de pantalla y bloqueo.**

10. Toque corto de **PWR**: aparece el menú de tres (Limpiar pantalla, Bloquear pantalla, Dormir ahora). La
    palanca mueve, OK elige, Atrás o PWR de nuevo lo cierra. Solo, se cierra a los diez segundos.
11. **Limpiar pantalla**: el próximo pintado sale completo y se va el fantasma. Probarlo **leyendo un EPUB**, que
    es donde más se acumula (el menú tiene que funcionar ahí adentro también).
12. **Bloquear pantalla**: sale el cartel. Ahora la palanca, OK y Atrás no hacen NADA; sacudir tampoco. Sólo PWR
    desbloquea, y al desbloquear vuelve la pantalla que estaba abajo.
13. Bloqueado, poner un recordatorio para dentro de un minuto: cuando vence, **desbloquea solo** y suena.
14. Bloqueado y quieto: tiene que reposar igual (ver Memoria). Apretar botones bloqueado no lo despierta del todo.

## 10. Apps en Lua (1.5.48)

1. Copiar `examples/Apps/contador.lua` y `examples/Apps/dados.lua` a `/Apps` de la tarjeta (Ajustes → Sistema →
   Modo memoria USB). Sin la carpeta `/Apps`, la pantalla tiene que decir que no hay apps, no romperse.
2. Juegos → **Apps de la tarjeta**: aparecen las dos con su ruta debajo.
3. **contador**: la palanca suma y resta, OK vuelve a cero. Salir con Atrás y volver a entrar: se acordó del
   número (`cp.save`/`cp.load`).
4. **dados**: la palanca cambia cuántos dados, OK tira, **sacudir el aparato** también tira. Los dados se dibujan
   con sus puntos.
5. **Atrás mantenido un segundo** sale de cualquier app, siempre. Es la salida de emergencia: probarla.
6. Una app rota: crear `/Apps/mala.lua` con `esto no es lua` adentro. Tiene que abrir la pantalla de error con el
   mensaje del intérprete (archivo y línea), y Atrás vuelve al catálogo. **El aparato no se reinicia.**
7. Una app colgada: crear `/Apps/colgada.lua` con `function on_open() while true do end end`. Después de un rato
   corto tiene que aparecer el error "la app tardó demasiado", no un aparato trabado.
