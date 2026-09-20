#pragma once

#include <string>

// REV-017: de quién es una entrada de la cola offline, y si se puede mandar.
//
// La cola guarda POSTs con ids del STORE, y el servidor numera desde 1 en CADA
// cuenta. Reproducir la cola de la cuenta A contra la B no duplica nada: le
// tilda, le corre la fecha o le BORRA a B el objeto que casualmente tenga ese
// número. Desde `/board` se puede mudar un aparato de cuenta sin tocarle el
// token, así que el aparato no puede dar por hecho de quién es lo que tiene
// guardado.
//
// La decisión vive acá, sola y sin nada del aparato adentro, por dos motivos:
// se prueba de escritorio (./test/queue_account/run.sh) y es UNA regla en UN
// lugar — la versión anterior la tenía escrita como un `if` adentro del bucle
// de `flushQueue()` y la premisa del comentario era falsa.
namespace queueacct {

// Qué se sabe de una entrada a la hora de decidir.
struct Entry {
  bool sealed = false;  // ¿trae el campo `acct`?
  std::string account;  // su valor (vacío = servidor de una sola cuenta)
};

// Qué se sabe de la sesión de red.
struct Session {
  std::string account;              // de qué cuenta se confirmó que es el aparato
  bool changedThisSession = false;  // ¿se descubrió un cambio de cuenta recién?
};

// ¿Sale esta entrada?
//
// Tres reglas, y la tercera es la que faltaba:
//
//  1. Sellada con la cuenta de ahora: sale.
//  2. Sellada con OTRA cuenta: no sale. Cubre el caso en que la cola no se
//     pudo vaciar (tarjeta dañada).
//  3. SIN sellar (la escribió un firmware anterior a REV-017): sale sólo si en
//     esta sesión NO se descubrió un cambio de cuenta. Antes se daba por hecho
//     que un cambio ya habría vaciado la cola, y eso es falso dos veces: el
//     vaciado puede fallar por tarjeta, y —lo que encontró el revisor— dentro
//     de la misma llamada `flushQueue()` ya tenía la lista LEÍDA EN MEMORIA, así
//     que vaciar el archivo no le sacaba nada de encima. Una entrada vieja
//     podía salir UNA vez contra la cuenta nueva.
//
// EL SELLO VACÍO ES UN SELLO (REV-055). `""` es la cuenta del servidor de un
// solo usuario: una identidad conocida, no una ausente. `enqueue()` escribe el
// campo SIEMPRE, así que "no tiene campo `acct`" significa una sola cosa —lo
// escribió un firmware anterior a REV-017— y una entrada nueva del modo de un
// solo usuario sale por la regla 1, aunque en la sesión haya habido un cambio.
// Con la versión anterior esa entrada era indistinguible de una vieja y se
// perdía: el usuario creaba una nota sin red y no llegaba nunca.
inline bool allowed(const Entry& e, const Session& s) {
  if (e.sealed) return e.account == s.account;
  return !s.changedThisSession;
}

// Por qué no salió, para el log. Devuelve nullptr cuando sí sale.
inline const char* refusal(const Entry& e, const Session& s) {
  if (allowed(e, s)) return nullptr;
  if (e.sealed) return "es de otra cuenta";
  return "es de un firmware viejo y el aparato acaba de cambiar de cuenta";
}

}  // namespace queueacct
