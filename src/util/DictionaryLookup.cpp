#include "DictionaryLookup.h"

#include "CrossPointSettings.h"

namespace dictlookup {

bool available() { return SETTINGS.dictionaryName[0] != '\0'; }

void Session::ensureOpen() {
  if (attempted) return;
  attempted = true;
  openOk = dict.open(SETTINGS.dictionaryName);
  needsIndex = openOk && dict.needsIndex();
}

StrId Session::busyMessage() {
  ensureOpen();
  return needsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
}

Hit Session::lookup(const char* word, void (*yieldFn)(void*), void* ctx) {
  ensureOpen();
  Hit out;

  bool ok = openOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && needsIndex) {
    ok = dict.buildIndex(yieldFn, ctx, &indexResult);
    needsIndex = !ok;  // una pasada buena deja el sidecar fresco; una fallida reintenta
  }

  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  if (ok && dict.lookup(word, out.definition, out.headword, &result)) {
    out.found = true;
    out.html = dict.definitionsAreHtml();
    return out;
  }

  // Nombrar la falla: que la palabra no esté es lo normal; que estuviera y no
  // se pudiera leer es un error de verdad, y se distingue la descompresión de
  // la falta de memoria y del error de lectura.
  out.error = true;
  if (!ok) {
    // Armar el índice pide un buffer de barrido, así que falla igual que una
    // consulta con el heap fragmentado: se dice eso y no un error genérico.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        out.message = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        out.message = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        out.message = StrId::STR_DICT_ERROR;  // falló dict.open(), no el índice
        break;
    }
    return out;
  }
  switch (result) {
    case Dictionary::LookupResult::Decompress:
      out.message = StrId::STR_DICT_DECOMPRESS_ERROR;
      break;
    case Dictionary::LookupResult::LowMemory:
      out.message = StrId::STR_DICT_LOW_MEMORY;
      break;
    case Dictionary::LookupResult::ReadError:
      out.message = StrId::STR_DICT_READ_FAILED;
      break;
    case Dictionary::LookupResult::NotFound:
    default:
      out.error = false;
      out.message = StrId::STR_DICT_NOT_FOUND;
      break;
  }
  return out;
}

}  // namespace dictlookup
