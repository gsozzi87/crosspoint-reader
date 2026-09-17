# Pendiente de verificar en el aparato

Todo lo que se publicó sin poder probarlo en hardware, con **qué mirar exactamente** y **dónde**. Se va
tachando a medida que se confirma. Lo que falle vuelve como corrección puntual.

Versión más nueva publicada: **1.5.92**. Sin probar desde **1.5.71**.

---

## Lo primero, porque bloquea a lo demás

### 1. El reloj del sistema (1.5.79) — bloquea el TLS

`/board/log`, al arrancar. Tiene que aparecer:

```
[CLK] reloj del sistema en hora desde el RTC (epoch …)
```

- **Si aparece** → el terreno está firme y se puede seguir con la verificación del certificado (la otra mitad
  de F15).
- **Si dice** `el RTC no tiene una hora creíble` → hay que resolver eso ANTES de tocar el TLS. Con el reloj en
  1970 todo certificado parece "todavía no válido", y si encima no hay red no se puede pedir la hora por NTP:
  ese es el escenario que deja el aparato incomunicado.

### 2. El reposo, que es lo que decide la autonomía (1.5.72 / 73)

En `/board/log` **ya NO** tiene que estar esto en bucle:

```
[REST] a reposar tras 45000 ms quieto (ciclo 2000 ms)
[REST] despertó por movimiento tras 2000 ms
```

Lo que sí tiene que verse: `a reposar tras 30000 ms quieto (hasta que toquen un botón)` y la línea siguiente
**recién cuando alguien apretó algo**, minutos u horas después.

Y estos cinco casos, que son los defectos que encontró la revisión adversarial:

- [ ] Dejarlo quieto **más de 10 minutos** y confirmar que llega al deep sleep (antes se quedaba en reposo para
      siempre y nunca bajaba).
- [ ] Poner un recordatorio para **dentro de varias horas**, dejarlo dormir y confirmar que suena.
- [ ] **Pausar una canción** y dejarlo: tiene que dormirse igual (antes quedaba despierto para siempre).
- [ ] Una alarma que **nadie atiende**: a los 60 s deja de sonar, **se posterga sola** y el aparato se duerme
      **sin pasar por el hub** (en el log: `nadie contesto en 60 s` y `nadie la atendio ...: a dormir`).
- [ ] Con un recordatorio **vencido** y el lector abierto, que no entre y salga del reposo sin parar.

---

## Lo que se ve

### 3. El fondo de pantalla (1.5.74)

- [ ] **Suspender** (PWR, soltar con la barrita): pantalla que dice **SUSPENDIDO**, el sello de la hora, la
      próxima alarma en dígitos grandes, clima, libro y titulares. Abajo: "OK para volver".
- [ ] **Apagar** (PWR 3 s): **APAGADO** y "PWR 1 s para encender". Antes acá quedaba congelada la barrita.
- [ ] Que **NO** aparezca la pantalla de sueño del SDK antes del fondo: es una sola pintura, no dos.
- [ ] Sin haber entrado nunca a Noticias no hay titulares: la pantalla tiene que quedar igual de bien.

### 4. La barrita del PWR (1.5.71)

- [ ] Se llena **pareja** de vacía a llena entre 1,2 s y 3 s, sin los dos escalones de antes.
- [ ] Soltar a mitad de camino suspende.

### 5. Interfaz unificada: Lyra y nada más (1.5.91)

- [ ] `Ajustes → Pantalla` no muestra un selector de interfaz en la ws397.
- [ ] En el log, `[UI] Using Lyra theme`.
- [ ] Hub, listas, agenda, ajustes, noticias, Biblia, viajes y lector: **todas con la misma cara**, sin mezclar
      tipografías ni formas entre pantallas. Es lo único que hay que mirar: que no quede una pantalla distinta.
- [ ] Un aparato que venía con Diario o Bento guardados en la tarjeta tiene que pasar a Lyra solo, sin migrar
      nada y sin quedar en un tema que ya no existe.

### 5 bis. La alarma y la batería (1.5.92)

Esto es lo que se acaba de arreglar y lo que más importa mirar, porque es lo que vació la batería.

- [ ] **Dejarlo apoyado boca abajo con un recordatorio programado.** Tiene que sonar y **no** posponerse solo.
      En el log NO puede aparecer `[MOTION] boca abajo` seguido de `[REMIND] gesto: se pospone` a los ~1,1 s de
      cada arranque. Lo que sí tiene que aparecer una vez por arranque es `posicion inicial: boca abajo`.
- [ ] **El gesto sigue andando a mano**: con el aparato boca arriba y la alarma sonando, darlo vuelta la
      posterga. (Levantarlo primero y después darlo vuelta también.)
- [ ] **El tope**: un recordatorio que nadie atiende suena **cuatro veces en media hora** (a los 0, 10, 20 y
      30 minutos) y después se descarta. En el log: tres `snooze N de 3 (sin atender)` y un
      `descartado ... tras 3 postergaciones`.
- [ ] Si el recordatorio **repite**, al descartarse queda armada la ocurrencia siguiente (mañana), no
      desaparece. Si **no repite**, se borra.
- [ ] **Entre repique y repique el aparato NO tiene que despertarse cada cinco segundos.** El log tiene que
      mostrar diez minutos limpios entre un `arranque por temporizador` y el siguiente.
- [ ] Una alarma que suena **desde Notas o la agenda**: al atenderla vuelve a esa pantalla, no al hub.
- [ ] Y el número que cierra todo: **dejarlo una noche entera y mirar el porcentaje a la mañana**
      (`Ajustes → Sistema → Memoria` da el %/h).

### 5 ter. El log (1.5.92)

- [ ] `/board/log` tiene que mostrar **una sola** copia de cada cosa, en orden, y **nada de más de un día**.
      Si aparecen dos veces las mismas líneas, la marca de subida no se está guardando.
- [ ] Sincronizar dos veces seguidas sin hacer nada en el medio: la segunda **no** sube nada
      (`log upload` no aparece, o aparece con muy pocos bytes).

---

## Lo que necesita servidor

### 6. Noticias masticadas (1.5.75)

Cargar dos o tres feeds en `/board` → Ajustes → Noticias, esperar un minuto y entrar a Noticias en el aparato.

- [ ] Las notas se ven **reescritas** para pantalla chica, no el texto crudo del diario.
- [ ] Se abren **sin WiFi**.
- [ ] Los titulares aparecen en el fondo de pantalla al suspender.

### 7. La sincronización oportunista (1.5.76)

- [ ] Abrir Hablar (o Noticias, o la Biblia), dejar la pantalla quieta unos segundos y ver en el log:
      `[SYNC] red arriba en …: N subidos, N noticias, hub …`.
- [ ] Con la **agenda** en frente tiene que decir `hub se deja para después` (es la guardia que evita leer fuera
      de rango).

### 8. La web (1.5.78)

- [ ] Seis pestañas: Hoy · Agenda · Listas · Notas · Viajes · Ajustes.
- [ ] Ajustes es una pantalla con secciones, no un cajón de filas iguales.
- [ ] Una dirección vieja guardada en el teléfono (`#mas/…`) sigue llevando a algún lado.

---

## Lo que no se puede verificar mirando: hay que medir

### 9. Autonomía (Ola 2, paso 8)

Con `Ajustes → Sistema → Memoria`, que ya dice "%/h · quedan N h (N días)".

- [ ] **Reposo puro**: cargado al 100 %, quieto, sin WiFi, 48 h.
- [ ] **Lectura**: leer sin parar hasta que se apague.
- [ ] **Uso normal**: 20 dictados/día durante 3 días.
- [ ] **Suspendido una semana** (nunca se midió).

Sin estos cuatro números no hay nada que poner en la caja.

### 10. Velocidad de lectura (Ola 8, paso 29)

Hoy 2,5 a 4,8 s por página con `display=2227ms` adentro. **Esto ahora se mide sin cable**: el coordinador
cronometra cada refresco y `Ajustes → Sistema → Memoria` tiene una sección **Panel** con el promedio, la cantidad
y el máximo de cada forma (FAST, HALF, FULL). La línea del log también lleva los milisegundos
(`refresh FAST hint=page 612ms …`).

- [ ] Abrir un libro y pasar **20 páginas seguidas** sin tocar nada más.
- [ ] Ir a `Ajustes → Sistema → Memoria` y anotar los tres renglones de **Panel**.

Lo que dicen esos números:

| Lo que se ve | Qué significa | Qué se hace |
|---|---|---|
| FAST ≈ 2 s, n grande | La onda parcial del panel ES así de lenta | Hay que cargar una **LUT propia** (registro 0x32), que es trabajo con hardware delante |
| FAST ≈ 0,5 s pero hay muchos HALF/FULL | Se están colando limpiezas donde alcanzaba un parcial | Es política: se arregla en `PanelRefreshCoordinator` y en `refreshFrequency` |
| FAST rápido y pocos HALF | El panel no es el problema | Lo que sobra está en el render (AA, prewarm), y ahí sí se puede recortar |

Objetivo: menos de 1 s.

### 11. Las carpetas de la tarjeta (Ola 8, paso 27)

- [ ] Con una tarjeta vacía (o después de borrar `/fonts`), arrancar y mirar `/board/log`: tiene que decir
      `[CARD] Carpeta creada: …` por cada una y **no** volver a decir `Fonts directory not found`.
- [ ] Entrar por **modo memoria USB** y ver `/Books`, `/Music`, `/Apps`, `/fonts` y `/dictionaries`.

### 12. El asistente de primer arranque (Ola 8, paso 28)

Sólo sale en un aparato que se ve **nuevo**: sin `setupDone`, sin redes WiFi cargadas y sin haber sincronizado
nunca. En el aparato del usuario **no va a salir** al actualizar, que es lo correcto.

- [ ] Para probarlo a propósito: borrar `/.crosspoint/hub.json` y `/.crosspoint/wifi.json` desde el modo memoria
      USB y arrancar.
- [ ] Los cinco pasos: idioma → WiFi por el teléfono → vincular → lugar del clima → los tres gestos.
- [ ] Saltar los tres del medio con ABAJO y llegar igual a la pantalla de gestos.
- [ ] **El reinicio silencioso**: si al salir de Vincular o del Clima el aparato se reinicia solo, tiene que
      **volver al asistente en el paso siguiente**, no al hub y no al principio (eso es lo que guarda
      `setupStep` en `hub.json`).

### 13. Las apps de Lua de fábrica (Ola 9, paso 30)

Las tres están probadas de escritorio (`./test/lua_sandbox/run.sh`, partidas enteras incluidas), pero el dibujo
nunca se vio en el vidrio: las medidas están escritas contra 480 × 800 y nadie las miró.

- [ ] Copiar `reloj.lua`, `ahorcado.lua` y `tresenraya.lua` a `/Apps` por el modo memoria USB.
- [ ] **Reloj**: la hora grande entra y no se sale de ancho; la fecha de abajo tampoco. Y sobre todo:
      **repinta al cambiar el minuto y no más seguido** (si parpadea cada pocos segundos, algo quedó mal).
- [ ] **Ahorcado**: las dos filas del abecedario entran a lo ancho y el resalte se lee (nunca letras sobre trama).
- [ ] **Tres en raya**: las tres columnas entran centradas y el cursor salta las casillas ocupadas.
- [ ] Las tres tienen que aguantar salir y volver a entrar con el marcador guardado.

---

## Lo que NO se hizo y por qué

- **La verificación del certificado TLS** (la otra mitad de F15). El reloj, que era el requisito que lo hacía
  imposible, ya está. Falta embeber las raíces y chequear el nombre del host. **No se toca sin hardware**: si
  queda mal, el aparato se queda sin red y hay que flashearlo por cable.
- **Riel y Estación** siguen siendo maquetas (`docs/ws397/maquetas/`), no temas.
- **Apps de Lua de fábrica** y `cp.time()`: Ola 9, el usuario las puso al final.
