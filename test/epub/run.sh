#!/usr/bin/env bash
# El EPUB que arma el servidor para el Librito (server/src/epub.ts), revisado
# de escritorio: el ZIP es válido, `mimetype` es la PRIMERA entrada y va sin
# comprimir (stored), y cada XHTML parsea como XML. Sin red y sin modelo.
set -e
cd "$(dirname "$0")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
EPUB="$TMP/muestra.epub"

(cd ../../server && bun run ../test/epub/make.ts "$EPUB")

# 1. El ZIP entero está sano.
unzip -tqq "$EPUB"

# 2. `mimetype` es la primera entrada y está stored, con el contenido exacto.
FIRST="$(unzip -Z1 "$EPUB" | head -n 1)"
[ "$FIRST" = "mimetype" ] || { echo "la primera entrada es '$FIRST', no 'mimetype'"; exit 1; }
unzip -v "$EPUB" | grep -E '^\s*[0-9]+\s+Stored\b.*\smimetype$' >/dev/null || { echo "mimetype no está Stored"; unzip -v "$EPUB"; exit 1; }
[ "$(unzip -p "$EPUB" mimetype)" = "application/epub+zip" ] || { echo "mimetype con contenido equivocado"; exit 1; }

# 3. Todo lo que es XML parsea como XML (el lector no perdona un & suelto).
unzip -q "$EPUB" -d "$TMP/x"
for f in "$TMP"/x/META-INF/container.xml "$TMP"/x/OEBPS/content.opf "$TMP"/x/OEBPS/toc.ncx "$TMP"/x/OEBPS/*.xhtml; do
  python3 -c "import xml.dom.minidom,sys; xml.dom.minidom.parse(sys.argv[1])" "$f" || { echo "no parsea: $f"; exit 1; }
done

# 4. Lo que tiene que estar: portada, tres capítulos, cierre, en ese orden en el spine.
python3 - "$TMP/x/OEBPS/content.opf" "$TMP/x/OEBPS/toc.ncx" "$TMP/x/OEBPS/ch1.xhtml" <<'PY'
import sys, xml.dom.minidom as m
opf = m.parse(sys.argv[1]); ncx = m.parse(sys.argv[2]); ch1 = m.parse(sys.argv[3])
spine = [e.getAttribute("idref") for e in opf.getElementsByTagName("itemref")]
assert spine == ["title", "ch1", "ch2", "ch3", "closing"], spine
hrefs = {e.getAttribute("href") for e in opf.getElementsByTagName("item")}
for need in ("title.xhtml", "ch1.xhtml", "ch3.xhtml", "closing.xhtml", "toc.ncx", "style.css"): assert need in hrefs, need
assert opf.getElementsByTagName("dc:title")[0].firstChild.data.startswith("La peste negra")
assert opf.getElementsByTagName("dc:language")[0].firstChild.data == "es"
assert "urn:uuid:" in opf.getElementsByTagName("dc:identifier")[0].firstChild.data
assert len(ncx.getElementsByTagName("navPoint")) == 5
ps = [p.firstChild.data for p in ch1.getElementsByTagName("p")]
assert ps[0] == "Europa en 1340 era un continente lleno & apretado.", ps
assert ps[1] == 'Segundo párrafo con <etiquetas> y "comillas".', ps
assert ch1.getElementsByTagName("h2")[0].firstChild.data == "Antes de la peste"
PY
# Los párrafos vacíos del capítulo 2 no dejan <p></p>.
[ "$(grep -c '<p>' "$TMP/x/OEBPS/ch2.xhtml")" = "2" ] || { echo "ch2 tiene que tener 2 párrafos"; cat "$TMP/x/OEBPS/ch2.xhtml"; exit 1; }

echo "epub: OK"
