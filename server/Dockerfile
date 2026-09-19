# Railway detecta este Dockerfile (Root Directory = server). Bun + Piper (voz
# local, sin tokens) con una voz por idioma. Las voces pesan ~60 MB cada una.
FROM oven/bun:1-debian@sha256:4f6e31d1a54d6a3dd312daef655fc998101b5043d52e12592ac293ef04b9bc73

RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates && rm -rf /var/lib/apt/lists/*

# Piper 2023.11.14-2 (binario x86_64 con espeak-ng y onnxruntime incluidos)
RUN mkdir -p /opt/piper/voices && cd /opt \
    && curl -sSL -o piper.tar.gz https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz \
    && echo "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992  piper.tar.gz" | sha256sum -c - \
    && tar xzf piper.tar.gz && rm piper.tar.gz

# Apps Lua de fábrica (server/src/assets.ts, FACTORY_APPS). Viven en
# examples/Apps del repo, que NO entra en el contexto de Docker: Railway compila
# con Root Directory = server, así que un COPY no las alcanza y hay que bajarlas.
#
# ESTABAN FIJADAS A UN COMMIT Y A SU sha256, y eso las congeló: el paquete de
# contenido sirvió durante meses el ahorcado de ese commit por más que el repo
# cambiara, porque nadie se acuerda de actualizar tres hashes a mano. Ahora se
# bajan de la RAMA que Railway despliega. El `ADD` de arriba es el reloj de la
# caché: su respuesta cambia con cada commit de la rama, así que la capa de las
# apps se rehace sola en cada despliegue. Sin él, Docker reusaría la capa vieja
# y volveríamos al mismo congelamiento por otro camino.
#
# La verificación que reemplaza al sha256: curl -f falla con 404 (una app que se
# renombró y no se cambió acá NO pasa desapercibida) y después se exige que
# estén las cinco y que ninguna venga vacía o recortada.
ARG APPS_REF=ws397
ADD https://api.github.com/repos/gsozzi87/crosspoint-reader/commits/${APPS_REF} /tmp/apps-head.json
RUN mkdir -p /opt/apps && cd /opt/apps \
    && for a in ahorcado librito libros mascota sudoku ; do \
         curl -fsSL --retry 3 -o $a.lua \
           https://raw.githubusercontent.com/gsozzi87/crosspoint-reader/${APPS_REF}/examples/Apps/$a.lua \
         || exit 1 ; \
       done \
    && n=$(ls -1 /opt/apps/*.lua | wc -l) && [ "$n" = 5 ] || { echo "faltan apps: $n de 5"; exit 1; } \
    && for f in /opt/apps/*.lua ; do \
         [ -s "$f" ] || { echo "$f vino vacío"; exit 1; } ; \
         [ "$(wc -c < "$f")" -ge 2000 ] || { echo "$f vino recortado"; exit 1; } ; \
       done \
    && ls -l /opt/apps

# Voces (rhasspy/piper-voices en Hugging Face): es, en, fr, de, pt, ru
ARG HF=https://huggingface.co/rhasspy/piper-voices/resolve/1162a9173d0ce503555aed757976b7a9912eae4c
RUN cd /opt/piper/voices && for v in \
      es/es_MX/claude/high/es_MX-claude-high \
      en/en_US/lessac/medium/en_US-lessac-medium \
      fr/fr_FR/siwis/medium/fr_FR-siwis-medium \
      de/de_DE/thorsten/medium/de_DE-thorsten-medium \
      pt/pt_BR/faber/medium/pt_BR-faber-medium \
      ru/ru_RU/irina/medium/ru_RU-irina-medium ; do \
      n=$(basename $v); curl -fsSL --retry 3 --retry-delay 2 -o $n.onnx $HF/$v.onnx \
      && curl -fsSL --retry 3 --retry-delay 2 -o $n.onnx.json $HF/$v.onnx.json ; done

# Biblias (thiagobodruk/bible, JSON): es, en, fr, de, pt, ru (~27 MB)
RUN mkdir -p /opt/bible && cd /opt/bible && for f in es_rvr en_kjv fr_apee de_schlachter pt_aa ru_synodal; do \
      curl -fsSL --retry 3 -o $f.json https://raw.githubusercontent.com/thiagobodruk/bible/13225a15fa5e3e3043495b0c82df56c3fdfeb7f4/json/$f.json ; done

WORKDIR /app
COPY package.json bun.lock* ./
RUN bun install --production --frozen-lockfile
COPY src ./src
COPY public ./public

ENV PIPER_BIN=/opt/piper/piper PIPER_VOICES=/opt/piper/voices BIBLE_DIR=/opt/bible FACTORY_APPS_DIR=/opt/apps PORT=3000
EXPOSE 3000
CMD ["bun", "run", "src/index.ts"]
