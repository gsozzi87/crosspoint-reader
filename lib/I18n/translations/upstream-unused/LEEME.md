# Idiomas de upstream que este fork no usa

Los idiomas del producto ws397 son siete: español, inglés, francés, alemán,
portugués (Brasil y Portugal) y ruso. Los demás vienen de CrossPoint upstream y
acá no se usan.

Estaban costando **440 KB de flash** (las cadenas de los 33 idiomas van al
binario) en un aparato que ya iba por el 87,5 % de flash usada.

`scripts/gen_i18n.py` recorre `*.yaml` de la carpeta de arriba y NO entra en
subcarpetas, así que con moverlos acá alcanza: siguen versionados y se pueden
recuperar moviéndolos de vuelta.
