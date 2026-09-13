#pragma once

#include <string>
#include <vector>

// El paquete de noticias que masticó el servidor.
//
// El aparato ya no limpia artículos ni espera con el WiFi arriba por cada nota:
// el servidor recorre los feeds solo, cada hora, se mete en cada noticia y la
// deja legible. Acá se baja ese trabajo de un tirón, cuando el aparato se
// conecta por CUALQUIER motivo, y después se lee sin red.
//
// Se baja en dos pasos a propósito, y no como un JSON grande:
//   1. el manifiesto (`GET /api/news/pack`), unos 4 KB, que dice qué hay y con
//      qué sha; lo que ya está y no cambió no se vuelve a bajar;
//   2. una nota por vez (`GET /api/news/item?id=`), cada una a su archivo.
// `ServerClient` no tiene streaming y copia el cuerpo dos veces, así que un
// JSON de 100 KB serían 200 KB de heap interno sobre los ~230 KB que hay.
namespace newspack {

struct Item {
  std::string id;
  std::string feed;
  std::string title;
  std::string when;
  std::string sha;
  bool chewed = false;
  bool local = false;  // el cuerpo ya está en la tarjeta
};

// Lo que hay guardado en la tarjeta. No toca la red.
std::vector<Item> cached();

// El cuerpo de una nota ("" si no está en la tarjeta).
std::string body(const std::string& id);

// Baja lo que falte. Necesita WiFi arriba y es SÍNCRONA: va desde una Activity
// de red o desde la sincronización, nunca desde el render. `budget` es cuántas
// notas como mucho baja en esta pasada (0 = todas las que falten).
// Devuelve cuántas bajó, o -1 si no se pudo ni traer el manifiesto.
int sync(int budget = 0);

// Los titulares más nuevos, para el fondo de pantalla. Sin red.
std::vector<std::string> headlines(int max);

}  // namespace newspack
