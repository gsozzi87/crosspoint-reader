# La tarjeta de fábrica

Qué tiene que haber en la microSD del aparato que se vende, y por qué. El firmware **crea las carpetas vacías
solo** (`src/util/CardLayout`), así que lo de acá es el **contenido**: lo que hay que copiar antes de cerrar la caja.

## Estructura

```
/Books/            un libro de muestra en EPUB
/Music/            vacía (el usuario copia sus MP3 por el modo memoria USB)
/Apps/             las apps de Lua de fábrica (Ola 9)
/fonts/            las tipografías extra para el lector
/dictionaries/     un diccionario StarDict en español
```

Las cinco las crea el aparato al arrancar si faltan, así que una tarjeta formateada **no rompe nada**: lo que se
pierde es el contenido, no el funcionamiento.

## Qué va en cada una

### `/Books` — el libro de muestra

Uno solo, en español, de dominio público y liviano (menos de 1 MB). Tiene que abrirse rápido y verse bien con la
tipografía de fábrica: es la primera página que el comprador va a leer y decide si el aparato "se siente" bien.

### `/dictionaries` — el diccionario

Un StarDict español: `/dictionaries/<carpeta>/<nombre>.idx` más `<nombre>.dict` o `<nombre>.dict.dz`. Sin `.dict`
al lado del `.idx`, `DictionaryRegistry` lo descarta a propósito (un diccionario que aparece en la lista y después
falla al buscar es peor que no tenerlo).

El idioma del diccionario **no** sigue al idioma de la interfaz: son dos cosas distintas y se elige en
Ajustes → Lectura.

### `/fonts` — las tipografías

Sólo las que no vienen en el firmware. Las de la interfaz (Ubuntu, NotoSans, NotoSerif) están compiladas adentro y
no van en la tarjeta.

### `/Apps` — las apps de Lua

Ola 9. El contrato está en `APPS_LUA.md` y los ejemplos en `examples/Apps/`.

## Lo que NO va

- **Nada dentro de `/.crosspoint/`**. Esa carpeta es del aparato: el token, las credenciales de WiFi, la caché del
  hub, el log. Si se clona una tarjeta con `/.crosspoint/server.json` adentro, **todos los aparatos salen con el
  mismo token** y con la cuenta del que armó la imagen. El token se genera solo en el primer arranque
  (`ServerCredentialStore::ensureToken()`); hay que dejarlo hacer su trabajo.
- **Ninguna credencial de WiFi.**
- Archivos de macOS (`._*`, `.Spotlight-V100`, `.Trashes`). El registro de diccionarios ya saltea los `._*`, pero
  ensucian el navegador de archivos.

## Cómo se arma

1. Formatear en **FAT32** (exFAT no).
2. Copiar el contenido de arriba.
3. Poner la tarjeta en un aparato **sin flashear la identidad**, encenderlo y comprobar que:
   - el log dice `[CARD] Carpeta creada: …` sólo para las que falten;
   - el asistente de primer arranque sale (`SetupActivity::pending()`);
   - el libro de muestra abre.
4. Sacar la tarjeta y clonarla **antes** de vincularla con ninguna cuenta.
