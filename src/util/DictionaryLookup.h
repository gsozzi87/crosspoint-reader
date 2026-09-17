#pragma once

#include <I18n.h>

#include <string>

#include "util/Dictionary.h"

// El diccionario local, en un solo lugar.
//
// Abrir el diccionario, armarle el índice la primera vez y traducir cada modo
// de falla al cartel que le corresponde eran cuarenta líneas metidas adentro de
// `DictionaryWordSelectActivity`. Con la Biblia buscando palabras también
// (1.5.93) esa lógica pasaba a estar escrita DOS veces, y dos copias de una
// regla se separan — ya pasó con las rutas protegidas en 1.5.91. Vive acá y la
// usan las dos pantallas.
namespace dictlookup {

// ¿Hay un diccionario elegido? Buscar significa diccionario local y nunca se
// convierte en una consulta a la IA sin avisar: si no hay, se dice.
bool available();

struct Hit {
  bool found = false;
  std::string headword;
  std::string definition;
  bool html = false;  // la definición es HTML y se puede componer con el motor del lector
  // Qué decir cuando no hay definición. `error` separa la falla real (no se
  // pudo abrir, no entró en memoria, el .dict.dz está roto) del caso normal,
  // que es que la palabra no esté.
  StrId message = StrId::STR_DICT_NOT_FOUND;
  bool error = false;
};

// Una sesión por pantalla: el diccionario se abre una vez y se queda abierto
// entre consultas. `needsIndex()` abre y valida el .qidx, así que preguntarlo
// una vez por apertura y no una vez por palabra es la diferencia entre una
// pasada por la SD y veinte.
class Session {
 public:
  // Qué cartel mostrar MIENTRAS busca. Hay que llamarlo antes de `lookup()`:
  // la primera vez puede tener que armar el índice, que es la pasada lenta, y
  // el usuario tiene que ver por qué el aparato se quedó pensando.
  StrId busyMessage();
  Hit lookup(const char* word, void (*yieldFn)(void*) = nullptr, void* ctx = nullptr);

 private:
  void ensureOpen();

  Dictionary dict;
  bool attempted = false;
  bool openOk = false;
  bool needsIndex = false;
};

}  // namespace dictlookup
