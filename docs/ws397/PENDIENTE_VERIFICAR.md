# Pendiente de verificar en el aparato

Todo lo que se publicó sin poder probarlo en hardware, con **qué mirar exactamente** y **dónde**. Se va
tachando a medida que se confirma. Lo que falle vuelve como corrección puntual.

Versión más nueva publicada: **1.5.80**. Sin probar desde **1.5.71**.

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
- [ ] Una alarma que **nadie atiende**: a los 60 s deja de sonar y el aparato se duerme (antes se comía la
      batería hasta la mañana).
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

### 5. Los temas (1.5.77 / 78)

- [ ] `Ajustes → Pantalla → Interfaz` ofrece **sólo Diario, Bento y Lyra**.
- [ ] Cambiar entre ellos se ve **en el acto**, sin reiniciar.
- [ ] En el log, `[UI] Using Diario theme`.
- [ ] Que el cambio se note en el **hub y la agenda**, no sólo en el lector (ése era todo el punto).

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

Hoy 2,5 a 4,8 s por página con `display=2227ms` adentro. Hay que ver qué forma de refresco elige el coordinador al
pasar de página. Objetivo: menos de 1 s.

---

## Lo que NO se hizo y por qué

- **La verificación del certificado TLS** (la otra mitad de F15). El reloj, que era el requisito que lo hacía
  imposible, ya está. Falta embeber las raíces y chequear el nombre del host. **No se toca sin hardware**: si
  queda mal, el aparato se queda sin red y hay que flashearlo por cable.
- **Riel y Estación** siguen siendo maquetas (`docs/ws397/maquetas/`), no temas.
- **Apps de Lua de fábrica** y `cp.time()`: Ola 9, el usuario las puso al final.
