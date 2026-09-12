# Auditoría de producto WS397 — 12 de septiembre de 2026

## Resultado y alcance

**No recomiendo publicar esta versión como producto terminado todavía.** La rama contiene correcciones concretas, pero quedan bloqueos de seguridad, coherencia sin conexión y validación física.

- Repositorio: gsozzi87/crosspoint-reader.
- Base inmóvil de la auditoría: `e779955fbfa458d8bb135580c5a93d5159de67f9`, versión 1.5.70, rama ws397. Cambios posteriores del otro agente no forman parte de esta revisión.
- Rama de trabajo: `codex/product-audit-2026-09-12`. No se hizo merge, despliegue ni publicación OTA.
- Cambios de código revisados: `b0f831af6892556db31b9ad0d9b0b51bb03d7ea2`.
- Dependencia recuperada: freeink-sdk `5c11243770d061808e7519b4b06c09aa3f40a2a9`, en una rama auxiliar del mismo nombre. Debe conservarse accesible: el firmware la referencia.
- Trabajo remoto mediante GitHub. No se clonó ni descargó una copia local del repositorio. La prueba web sirvió tres recursos en memoria con respuestas ficticias.

Se inventariaron **1.025 archivos del repositorio principal**. Se recuperaron para inspección 389 archivos seleccionados de firmware, servidor, interfaz y configuración, y después ocho archivos adicionales del lector. La profundidad fue mayor en persistencia, cuentas, sincronización, red, audio, alarmas y formularios. Inventariar o recuperar un archivo no equivale a certificar cada línea. Las tablas generadas, imágenes, fuentes y todas las combinaciones de contenido no recibieron una inspección exhaustiva individual. Esta auditoría tampoco sustituye una prueba sobre el WS397 físico.

Distribución del inventario: lib 402, src 343, test 82, docs 76, server 48, scripts 27, .github 11; el resto son configuración, instrucciones y ejemplos.

## Correcciones incluidas

| ID | Error observado en la base | Cambio en la rama |
|---|---|---|
| C01 | El submódulo apuntaba a un commit no publicado: una instalación limpia no podía obtenerlo. | Reconstruidos los tres parches exportados de audio/IMU sobre el SDK publicado; verificados sus hashes y publicado un commit accesible. |
| C02 | El servidor podía devolver una copia antigua de notas/listas desde la memoria de otra réplica; también exponer objetos modificables sin guardar. | Lecturas desde el almacenamiento y modificaciones dentro de la transacción existente, sin esa caché compartida mutable. |
| C03 | La cola interna de escrituras por archivo conservaba entradas después de terminar. | Eliminación de entradas terminadas sin borrar una escritura sucesora. |
| C04 | Dos cuentas podían competir por el mismo código de vinculación; un código viejo podía intentar sobrescribir una credencial nueva. | Consumo del código y reasignación en una transacción; comprobación de la credencial actual al actualizar. |
| C05 | El registro público concedía administrador al correo configurado, sin demostrar propiedad del correo; también tenía una condición de primera cuenta. | Todo registro público crea una cuenta normal. La creación del administrador queda en el arranque administrado. |
| C06 | Comprobar consumo y sumarlo después permitía superar límites con peticiones simultáneas. El tamaño declarado del audio no era suficiente. | Reserva atómica antes de atender, medida del cuerpo recibido y devolución contra el mes reservado si la respuesta falla. |
| C07 | Las sugerencias no respetaban el límite mensual antes de generar. | Reserva previa; respuesta almacenada cuando corresponde; devolución si falla la generación. |
| C08 | El código de vinculación aparecía en el registro. | Mensaje de diagnóstico sin el código. |
| C09 | Los JSON del aparato se leían con un límite de 50 KB aunque la cola y las notas podían superarlo. | Lectura JSON por flujo desde el archivo. No elimina la necesidad de límites de memoria. |
| C10 | Si faltaba el archivo definitivo, el código salía antes de recuperar el temporal de una escritura interrumpida. | Validación del JSON temporal y promoción únicamente si es válido. |
| C11 | Al llegar a 50 operaciones pendientes se eliminaba la más antigua; un archivo de cola dañado podía parecer vacío. | Rechazo de la nueva operación sin borrar las anteriores; una cola dañada bloquea la sustitución de la copia local y no se sobrescribe al agregar. |
| C12 | Peticiones normales y la pantalla de diagnóstico podían vaciar pendientes antes de comprobar a qué cuenta pertenecían. | Eliminada la descarga automática de pendientes en esos caminos; comprobación explícita en sincronización. Al detectar otro dueño, los pendientes se conservan en cuarentena. **El aislamiento completo sigue abierto en P08.** |
| C13 | Se podía bajar una instantánea del servidor mientras quedaban modificaciones locales sin enviar. | La sincronización no reemplaza el estado local mientras existe cola pendiente; el límite de vaciado cubre las 50 entradas. |
| C14 | Un efecto sonoro ya inicializado podía disputar el audio con música/grabación. | Comprobación del uso del puerto y de música también en el trabajador; asignación que permite detectar falta de memoria. |
| C15 | Cancelar Wi-Fi al abrir una noticia podía descartar un texto de rescate ya disponible. | Muestra el texto disponible antes de declarar fracaso. |
| C16 | Una versión OTA mal formada dejaba enteros sin inicializar para la comparación. | Inicialización y comprobación de los tres componentes antes de comparar. Esto no resuelve la autenticidad de OTA. |
| C17 | Agregar a una lista borraba el texto antes de saber si se había guardado. Dos envíos podían repetirse. | Conservación del borrador en error, bloqueo mientras guarda y desbloqueo para reintentar. |
| C18 | Agenda/día podían seguir mostrando datos anteriores después de cambiar algo o actualizar. | Invalidación de ambas cachés al modificar o refrescar. |
| C19 | El cierre retardado de un editor podía vaciar el siguiente editor recién abierto. | Cancelación del cierre anterior al abrir otro. |
| C20 | Algunas acciones de texto trataban errores HTTP como éxito; una imagen en carga podía usar “loading” como dirección. | Comprobación HTTP y exclusión del marcador de carga al construir la imagen. |
| C21 | En ZIP sin compresión y XTC por flujo, un error de lectura negativo se convertía en un tamaño enorme. ZIP podía desbordar al reservar el terminador de un tamaño máximo. | Comprobación del resultado firmado antes de consumir datos, rechazo de bloques vacíos y protección del tamaño de reserva. |
| C22 | La CI habitual no incluía el producto ws397 ni estas regresiones del servidor. | Flujo específico de compilación WS397, pruebas existentes del lector y pruebas aisladas de servidor/formularios. Sin despliegue. |

Referencias principales:
- C02–C03: [server/src/store.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/store.ts#L175); [server/src/fsjson.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/fsjson.ts#L27).
- C04–C08: [server/src/accounts.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/accounts.ts#L199); [server/src/accounts.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/accounts.ts#L372); [server/src/usage.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/usage.ts#L96); [server/src/api.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/api.ts#L87).
- C09–C13: [lib/Serialization/PersistableStore.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/Serialization/PersistableStore.cpp#L21); [lib/ServerClient/ServerClient.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/ServerClient/ServerClient.cpp#L175); [src/activities/home/HubSyncActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/home/HubSyncActivity.cpp#L446).
- C17–C20: [server/public/board/app.js](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/public/board/app.js#L1458).
- C21: [lib/ZipFile/ZipFile.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/ZipFile/ZipFile.cpp#L452); [lib/Xtc/Xtc/XtcParser.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/Xtc/Xtc/XtcParser.cpp#L470).

## Errores y riesgos que siguen abiertos

P1 = resolver antes de venta general. P2 = resolver o delimitar expresamente antes de prometer la función. “Confirmado” describe el camino de código; no implica una explotación ni una reproducción física.

### P01 — P1 — TLS sin validación y actualización sin confianza suficiente

Confirmado: ServerClient y el camino wolfSSL de HttpDownloader llaman `setInsecure()`. Cifrar sin validar la identidad permite que un intermediario suplante al servidor. El problema alcanza credenciales y contenido; debe revisarse también el recorrido de OTA. Además HttpDownloader vuelve a agregar Basic Auth en cada salto, incluso si cambia el destino.

**Trabajo pendiente:** confianza de certificados y nombre del servidor, reloj de arranque, rechazo de degradación y política de redirecciones que no envíe credenciales a otro origen; autenticidad del firmware y recuperación ante actualización fallida.
**Aceptación:** certificado válido funciona; certificado equivocado, caducado y servidor impostor fallan sin instalar ni revelar credenciales. Probar renovación de certificados y arranque sin hora.
Referencia: [lib/ServerClient/ServerClient.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/ServerClient/ServerClient.cpp#L69); [src/network/HttpDownloader.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/network/HttpDownloader.cpp#L76).

### P02 — P1 — Configuración Wi-Fi abierta con funciones de administración del aparato

Confirmado: `startPhoneEntry()` crea “CrossPoint-Reader” sin contraseña y arranca CrossPointWebServer completo. Ese servidor registra lectura/escritura de archivos, ajustes y WebDAV. El alta de una red desde el teléfono queda unida a una superficie mucho mayor que la necesaria.

**Trabajo pendiente:** modo exclusivo de aprovisionamiento, autorización efímera vinculada a lo que muestra la pantalla y cierre automático. No basta con ocultar botones: bloquear rutas, WebDAV y WebSocket. Diseñar además acceso autenticado al gestor de archivos normal.
**Aceptación:** un segundo teléfono sin autorización no puede leer SD ni cambiar ajustes; el usuario autorizado puede configurar una red oculta, cancelar y volver a intentarlo.
Referencia: [src/activities/network/WifiSelectionActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/network/WifiSelectionActivity.cpp#L911); [src/network/CrossPointWebServer.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/network/CrossPointWebServer.cpp#L150).

### P03 — P1 — Reintentos sin idempotencia completa

Confirmado: se envía X-Request-Id, pero falta un mecanismo durable que haga cumplir la deduplicación de las operaciones. El identificador de una petición que termina en cola cambia al encolarla. Si el servidor guardó y se perdió la respuesta, el reintento puede repetir la acción. Guardar el vaciado de la cola sólo al final también permite repetir tras un corte.

**Trabajo pendiente:** conservar un identificador desde la primera emisión hasta el acuse, ámbito cuenta/dispositivo y resultado persistente en servidor, dentro de la misma transacción que modifica datos. Resolver también acciones de voz, que pueden hacer varias modificaciones.
**Aceptación:** cortar la conexión después del commit y antes de la respuesta; reiniciar y repetir; debe haber una sola nota/operación.

### P04 — P1 — Repeticiones distintas en servidor y aparato sin conexión

Confirmado: `nextRepeatDue()` calcula varios casos en UTC, trata diariamente como un día fijo y no reproduce toda la semántica de días semanales, intervalos y fin de repetición del servidor. Meses cortos y cambio de horario pueden divergir.

**Trabajo pendiente:** un contrato de repetición completo, zona horaria y ocurrencia confirmada; decidir cómo representa el aparato la próxima ocurrencia sin red.
**Aceptación:** cada dos días; martes/jueves; día 31; 29 de febrero; límite de repetición; cambio horario; varios días apagado. Comparar web y aparato.
Referencia: [src/HubStore.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/HubStore.cpp#L278).

### P05 — P1 — Avisos pospuestos durante lectura

Confirmado: `checkTimeAlarms()` retorna inmediatamente si la actividad es un lector. No se puede prometer “el aviso suena a la hora mientras leo”. Apagado real y suspensión también deben distinguirse en las promesas del producto.

**Trabajo pendiente:** arbitrar la interrupción del lector, persistir/retomar su posición y verificar temporizador y RTC. Mantener la exclusión de almacenamiento cuando USB posee la SD.
**Aceptación:** alarma y temporizador mientras se lee EPUB/TXT/XTC, suspendido, con música y al salir de USB.
Referencia: [src/main.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/main.cpp#L490).

### P06 — P1 — Una nota local borrada puede reaparecer

Confirmado: la nota pendiente tiene identificador negativo; borrarla la quita del HubStore, pero no cancela su POST de creación ya guardado en la cola. Al sincronizar puede volver.

**Trabajo pendiente:** vincular borrador con operación durable y poder cancelar/compensar esa creación.
**Aceptación:** dictar, fallar el envío, borrar sin red, reiniciar y sincronizar: no debe reaparecer.
Referencia: [src/activities/home/NotesActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/home/NotesActivity.cpp#L337).

### P07 — P1 — Nota demasiado grande para la cola

Confirmado: la cola admite cuerpos de hasta 4.096 bytes; hay notas que pueden superar ese tamaño. Guardar temporalmente en la copia del hub no garantiza recuperación: una sincronización posterior puede sustituirla cuando no hay entrada pendiente.

**Trabajo pendiente:** almacén de borradores independiente con estado visible y subida por referencia/archivo, o un límite coherente aplicado antes de confirmar al usuario.
**Aceptación:** nota mayor de 4 KB, texto multibyte, servidor 503, reinicio y sincronización. Debe conservarse o rechazarse de forma explícita.

### P08 — P1 — Cambio de cuenta aún no aísla todos los datos

La rama elimina el vaciado prematuro y conserva pendientes del dueño anterior en cuarentena, pero la identidad local usa el correo y no un identificador de cuenta más servidor. La adopción de una cola antigua sin dueño conocido y las otras cachés requieren migración. También existen operaciones directas fuera del ciclo de sincronización.

**Trabajo pendiente:** guardar propietario/origen en cada cola y caché, bloquear operaciones con identidad incierta y definir transferencia/restablecimiento del aparato.
**Aceptación:** cuentas A/B con IDs de notas coincidentes; cambiar servidor; cola heredada sin propietario; fotos, viajes, audio y registros no deben pasar a B.
Referencia: [src/activities/home/HubSyncActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/home/HubSyncActivity.cpp#L446); [src/HubStore.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/HubStore.cpp#L347).

### P09 — P1 — Protección de solicitudes remotas incompleta

Confirmado: `isPrivateHost()` filtra nombres y algunos rangos literales, pero no resuelve y fija el destino DNS. Una URL pública que resuelva a una red privada y variantes IPv6 requieren protección adicional. Comprobar sólo la cadena no cierra SSRF.

**Trabajo pendiente:** política de destinos y salida de red; validar direcciones resueltas, redirecciones y conexión efectiva evitando cambio entre comprobación y uso.
**Aceptación:** DNS a loopback/red privada, cambio DNS, IPv4 mapeada en IPv6 y redirección a metadatos, sin realizar conexiones internas.
Referencia: [server/src/net.ts](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/server/src/net.ts#L15).

### P10 — P1 — Actualización de contenido puede destruir el archivo anterior

Confirmado: AssetSync verifica lo recibido, pero escribe el archivo final directamente; una escritura corta puede perder la copia anterior. La decisión de omitir descarga compara manifiesto y existencia, sin validar de nuevo el archivo que quedó en SD.

**Trabajo pendiente:** descargar a temporal, comprobar cierre/tamaño/hash, reemplazar con recuperación y confirmar manifiesto después. Verificar el contenido local cuando esté dañado.
**Aceptación:** cortar corriente durante descarga, durante escritura y al actualizar manifiesto; debe quedar una versión utilizable.
Referencia: [src/activities/home/AssetSyncActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/home/AssetSyncActivity.cpp#L336); [src/activities/home/AssetSyncActivity.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/src/activities/home/AssetSyncActivity.cpp#L395).

### P11 — P2 — ZIP/EPUB aún necesitan endurecimiento de formato

La rama corrige dos errores de memoria, pero la lectura del directorio central aún contiene campos leídos sin comprobar todos los retornos. La búsqueda del fin de ZIP se limita a 1 KB: archivos válidos con comentarios largos pueden no abrirse. Falta una campaña de archivos truncados, tamaños imposibles y descompresión excesiva.

**Trabajo pendiente:** límites de formato/memoria, todas las lecturas y offsets comprobados, búsqueda EOCD compatible y pruebas de entradas malformadas.
**Aceptación:** rechazar sin reinicio archivos truncados; abrir ZIP válido con comentario largo; limitar consumo y tiempo.
Referencia: [lib/ZipFile/ZipFile.cpp](https://github.com/gsozzi87/crosspoint-reader/blob/b0f831af6892556db31b9ad0d9b0b51bb03d7ea2/lib/ZipFile/ZipFile.cpp#L225).

### P12 — P1 — Cambiar contraseña no revoca las sesiones existentes

Confirmado por revisión del flujo: cambiar el hash de contraseña no invalida las cookies firmadas existentes.

**Trabajo pendiente:** versión de sesión por cuenta o sesiones revocables, renovación después de cambio y opción “cerrar otras sesiones”.
**Aceptación:** dos navegadores; cambiar clave desde uno; la sesión anterior del otro debe dejar de ser válida.

### P13 — P2 — Arranque administrativo concurrente

`seedFromFiles()` comprueba el número de cuentas y luego crea el administrador; dos réplicas en una base vacía pueden competir. C05 cierra el privilegio del registro público, pero no convierte la migración inicial en una operación transaccional de un solo ejecutor.

**Trabajo pendiente:** bloqueo de arranque y migración idempotente, recuperación segura de administrador sin depender de conservar una contraseña en logs.
**Aceptación:** dos arranques simultáneos, interrupción a mitad de migración y nuevo arranque sin duplicar ni dejar datos a medias.

### P14 — P2 — Los topes no equivalen todavía al costo real de proveedores

C06 evita carreras, pero contar una llamada por ruta no representa necesariamente varias llamadas internas del asistente. La duración se estima por bytes y tipo declarado. Una operación que llamó a un proveedor y luego terminó en error puede haber generado costo aunque se devuelva la reserva.

**Trabajo pendiente:** medir invocaciones reales, formatos de audio admitidos y costos de búsqueda/TTS; definir explícitamente qué cuenta como cuota del cliente.
**Aceptación:** errores después de usar proveedor, herramientas múltiples, audio mal declarado y ejecución simultánea.

### P15 — P2 — Rechazos definitivos de pendientes no tienen recuperación visible

El vaciado elimina operaciones rechazadas definitivamente por 4xx. Puede ser razonable no reintentarlas, pero el usuario no dispone de una bandeja clara para saber qué no llegó y recuperarlo.

**Trabajo pendiente:** conservar rechazadas con motivo, conteo visible y exportación/reintento controlado. Evitar mostrar “sincronizado” como si todo hubiera sido aceptado.
**Aceptación:** operación cuyo objeto se borró desde la web, payload inválido y cola llena: aviso comprensible y dato recuperable.

### P16 — P2 — Accesibilidad e idiomas incompletos en la web

Observado: gran parte de /board está escrita en español; cambiar el idioma del aparato no traduce esa web. Hay filas editables que aparecen como elementos genéricos en la inspección de accesibilidad, no como controles con nombre. Revisar foco y teclado en editores.

**Trabajo pendiente:** controles semánticos, foco visible/retorno de foco, navegación sólo con teclado, etiquetas de errores y traducción coherente.
**Aceptación:** crear/editar/borrar sin ratón, lector de pantalla y revisión de textos en cada idioma anunciado.

## Funcionalidades y pantallas: qué se verificó y qué falta

“Código” significa revisión estática del flujo. “Web” significa navegación con datos ficticios; no prueba comunicación con un aparato real.

| Área | Funciones presentes | Verificación y siguiente prueba |
|---|---|---|
| Inicio/hub | Accesos, estado, clima, próxima actividad, sincronización | Código. Probar primer arranque sin cuenta/reloj/red, navegación y refresco de tinta. |
| Lectura | Biblioteca, recientes, EPUB, TXT, XTC/XTCH; opciones de lectura y diccionario | Código de actividades y partes críticas de parsers. Pruebas existentes en CI; probar corpus real, libros grandes, notas al pie, imágenes y recuperación de posición. |
| Hablar | Captura, transcripción, asistente y acciones | Código. Requiere micrófono, proveedor real, ruido, cancelación y retorno a pantalla anterior. |
| Traductor | Captura y traducción con salida de voz/texto | Código. Probar cada idioma, falta de red y retorno sin perder actividad. |
| Agenda/calendario | Recordatorios, tareas, compras, eventos y repetición | Código y vistas web Hoy/Agenda/Calendario/Listas. Abiertos P03–P05. |
| Temporizador/avisos | Cuenta atrás, pausa/reanudación, avisos | Código. Requiere medir hora real, suspensión y convivencia con lector/música. |
| Notas | Notas transcritas y grabaciones locales | Código y vista web. Abiertos P06–P07; probar SD llena y audio interrumpido. |
| Biblia | Contenido offline, selección y consulta contextual | Código. Probar paquetes por idioma, descarga interrumpida y consulta con cuota agotada. |
| Música | Reproducción, volumen y controles | Código; C14. Requiere salida real, pausa/reanudación, cambio de pista y alarmas. |
| Noticias | Feeds, titulares, artículos y caché | Código; C15. Probar feeds reales, artículos cortos, redirecciones y lectura sin red. |
| Fotos/viajes/documentos | Álbum, itinerarios, archivos adjuntos, preparación | Código. Requiere descarga/renderizado real, formatos grandes y aislamiento de cuentas. |
| Juegos/apps Lua | Lanzador y ejecución de aplicaciones | Código de integración. Probar cada juego distribuido, límites, salir y avisos; no confundir módulos antiguos con accesos actuales. |
| Clima/ubicación | Búsqueda de lugar, previsión y zona horaria | Código y vista de ajustes. Probar ubicación sin resultados, error del servicio y cambio de zona. |
| Wi-Fi/cuentas | Redes guardadas/ocultas, alta por teléfono y vinculación | Código y vista Aparatos; pruebas reales de concurrencia en PostgreSQL. P02 y P08 siguen abiertos. |
| Archivos/USB | Gestor web, WebDAV y almacenamiento USB | Código. Exclusión de SD, expulsión, cable retirado y reconstrucción tras escritura requieren hardware. |
| Actualización/contenido | Consulta e instalación OTA y paquetes SD | Código; C01/C16/C22. P01/P10 pendientes; no se instaló firmware. |
| Web de administración | Hoy, Agenda, Listas, Notas, Más, ajustes, aparatos, contenido y diagnóstico | Vistas principales y secciones de Más abiertas con fixtures; regresiones de formularios y revisión de código. No se probó cada operación de cada pantalla con servidor real. |

## Validación

- Primer commit: **7 pruebas de servidor pasadas, 0 fallos**, PostgreSQL 17 y Bun 1.3.10 en GitHub Actions. Incluyen 12 escrituras concurrentes, reversión, 12 reservas concurrentes y carrera de vinculación.
- Segundo commit: **9 pruebas pasadas, 0 fallos**; añade conservación/reintento del formulario y doble envío. Consultar la ejecución enlazada para los resultados de compilación y pruebas nativas.
- No se ejecutaron la matriz de otras placas ni todas las herramientas de análisis/formato de la CI general.
- Web: caso de guardado con HTTP 503 reproducido y texto conservado; navegación de vistas indicadas con datos ficticios. Comprobación de ancho 360 px en Aparatos sin desbordamiento horizontal del documento.
- Última revisión de código: **WS397 compilado correctamente y 175 pruebas nativas pasadas, 0 fallos**. Validación completa de este flujo finalizada con éxito el 12/09/2026 a las 20:54 UTC. Un resultado de compilación no equivale a funcionamiento físico.
- No se probaron proveedores de IA de pago, el servidor de producción, micrófono, altavoz, RTC, suspensión, batería, SD real, USB ni OTA física. No se consumieron datos de clientes.

Ejecución con las últimas modificaciones de código: [Product audit](https://github.com/gsozzi87/crosspoint-reader/actions/runs/34718086154).

## Mejoras de producto recomendadas

1. **Primer uso guiado:** idioma → red → cuenta → descarga de contenido → prueba de micrófono/altavoz → pantalla inicial, con recuperación en cada paso.
2. **Estado de sincronización comprensible:** “guardado aquí”, “pendiente de enviar”, “sincronizado” y “no se pudo”; fecha de última sincronización real.
3. **Respaldo, restauración y transferencia:** recuperar notas/grabaciones, exportar información y preparar el aparato para otro dueño sin mezclar cuentas.
4. **Diagnóstico para soporte:** versión de firmware y contenido, último fallo explicado, exportación voluntaria con secretos y datos personales filtrados.
5. **Pruebas de duración y fallos:** sesiones de varios días, SD llena/corrupta/ausente, batería baja, reconexiones, cambios de horario y apagado a mitad de operaciones.
6. **Control de versiones entregables:** firmware, SDK y paquete de contenido identificados juntos; matriz de compatibilidad y procedimiento de reversión.
7. **Documentación de funciones reales:** dejar claro qué necesita internet, qué funciona sin cuenta y la diferencia entre suspender y apagar; eliminar instrucciones obsoletas.
8. **Revisión de distribución:** inventario de dependencias, licencias y procedencia de fuentes/contenidos que se incluirán. No se emite aquí una conclusión jurídica sobre su comercialización.

## Orden de cierre antes de vender

Primero P01/P02/P09 y aislamiento P08; después P03–P07 y P10/P12; finalmente compatibilidad de formatos, recuperación y experiencia de usuario. Los cambios de esta rama deben revisarse e instalarse en unidades de prueba antes de decidir el merge. No se puede declarar el producto libre de errores a partir de una auditoría de código.

## Cierre de la validación remota

La ejecución sobre `b0f831af6892556db31b9ad0d9b0b51bb03d7ea2` terminó en **success**. Las cifras de compilación se refieren a ocupación estática, no a memoria libre ni autonomía durante el uso:

```
RAM:   [===       ]  28.0% (used 91772 bytes from 327680 bytes)
Flash: [========= ]  87.2% (used 5711839 bytes from 6553600 bytes)
======================== [SUCCESS] Took 364.80 seconds ========================
ws397          SUCCESS   00:06:04.799
100% tests passed, 0 tests failed out of 175
Total Test time (real) =   0.41 sec
```

El commit posterior del informe sólo añade documentación; el código probado permanece igual.
