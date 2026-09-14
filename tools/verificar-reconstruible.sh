#!/usr/bin/env bash
# ¿El firmware que se vende se puede volver a construir desde cero?
#
# La respuesta era NO y nadie lo sabía: el repo padre apuntaba a un commit del
# submódulo que nunca se había subido, así que un `git clone --recursive` desde
# GitHub fallaba al traer el SDK. Se compilaba sólo en la máquina donde ese
# commit existía.
#
# Esto lo comprueba de la única forma que vale: preguntándole al remoto si tiene
# el commit que el repo padre dice necesitar. Sin clonar 200 MB.
set -u
cd "$(dirname "$0")/.."

fallas=0
decir() { printf '%-58s %s\n' "$1" "$2"; }

# 1) El commit del submódulo que el repo padre tiene anotado.
# OJO con `tr -d '+-U'`: en tr eso es un RANGO de '+' (0x2B) a 'U' (0x55), que
# se come TODOS los dígitos del hash. El prefijo (+ = distinto del anotado,
# - = sin inicializar, U = con conflictos) se saca con sed.
esperado=$(git submodule status freeink-sdk | awk '{print $1}' | sed 's/^[-+U]//')
url=$(git config -f .gitmodules submodule.freeink-sdk.url)
rama=$(git config -f .gitmodules submodule.freeink-sdk.branch)
decir "submódulo anotado" "$esperado"
decir "remoto" "$url ($rama)"

# 2) ¿El remoto lo tiene? `ls-remote` lista las puntas; para un commit que no
#    sea la punta hace falta preguntar por el objeto.
if git ls-remote "$url" | awk '{print $1}' | grep -q "^$esperado$"; then
  decir "el remoto tiene ese commit" "sí (es punta de una rama o tag)"
else
  # Puede estar en la historia sin ser punta: se comprueba con un fetch al aire.
  if git -C freeink-sdk fetch -q "$url" "$esperado" 2>/dev/null; then
    decir "el remoto tiene ese commit" "sí (está en la historia)"
  else
    decir "el remoto tiene ese commit" "NO -> el firmware NO se puede reconstruir"
    echo "    Arreglo: cd freeink-sdk && git push origin HEAD:$rama"
    fallas=$((fallas + 1))
  fi
fi

# 3) Que no haya cambios sin commitear en el submódulo: compilarían acá y no allá.
if [ -n "$(git -C freeink-sdk status --porcelain)" ]; then
  decir "submódulo limpio" "NO: hay cambios sin commitear"
  fallas=$((fallas + 1))
else
  decir "submódulo limpio" "sí"
fi

# 4) Que cada commit ws397 del SDK tenga su .patch exportado (convención del
#    proyecto: si el submódulo se pierde, los parches lo reconstruyen).
comits=$(git -C freeink-sdk log --oneline --grep='^ws397:' | wc -l | tr -d ' ')
parches=$(ls docs/ws397/*.patch 2>/dev/null | wc -l | tr -d ' ')
if [ "$parches" -ge "$comits" ]; then
  decir "parches exportados ($parches) vs commits ws397 ($comits)" "sí"
else
  decir "parches exportados ($parches) vs commits ws397 ($comits)" "FALTAN $((comits - parches))"
  echo "    Arreglo: cd freeink-sdk && git format-patch -<N> -o ../docs/ws397/"
  fallas=$((fallas + 1))
fi

echo
if [ "$fallas" -eq 0 ]; then
  echo "Reconstruible: un clon limpio puede compilar este firmware."
else
  echo "NO reconstruible: $fallas problema(s). Ver arriba."
fi
exit "$fallas"
