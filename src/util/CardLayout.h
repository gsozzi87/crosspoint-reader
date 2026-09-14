#pragma once

// La tarjeta sale armada de fábrica, pero nadie garantiza que siga así: el
// usuario la formatea, la copia a otra, o borra una carpeta desde el modo
// memoria USB. En vez de loguear que falta (`Fonts directory not found`,
// `No /dictionaries directory`) y dejar la función muda, el aparato crea al
// arrancar las carpetas que espera encontrar.
//
// Sólo carpetas vacías: nada de contenido. Una carpeta vacía cuesta un cluster
// y hace visible desde el modo memoria USB DÓNDE va cada cosa, que es el
// problema real que tiene el que acaba de comprar el aparato.
namespace cardlayout {

// Crea las carpetas que falten. Se llama una vez, en el arranque, con la
// tarjeta ya montada. Loguea sólo lo que crea.
void ensure();

}  // namespace cardlayout
