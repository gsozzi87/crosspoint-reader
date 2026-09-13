#pragma once

// Barrido de los `.tmp` que dejó una escritura interrumpida.
//
// `SDCardManager::writeFile` escribe a `<archivo>.tmp`, verifica el largo y
// recién ahí reemplaza el destino (F16). El rescate existía pero era PEREZOSO:
// vivía adentro de `readFile()` y sólo actuaba sobre el archivo que alguien
// estuviera leyendo justo en ese momento. Los que se leen con
// `openFileForRead()` directo — la caché del hub, la de viajes, la de
// noticias, el fondo de pantalla — nunca pasaban por ahí, así que un corte en
// medio de una escritura les dejaba el `.tmp` tirado para siempre.
//
// Esto lo hace de entrada, una vez por arranque:
//   - destino que NO está  -> el `.tmp` ES el archivo: se renombra (rescate).
//   - destino que SÍ está  -> la escritura no llegó a confirmarse: el `.tmp` se
//     borra y queda el contenido viejo, que es el último estado bueno.
namespace tempsweep {
// Devuelve cuántos rescató y cuántos borró.
void run();
}  // namespace tempsweep
