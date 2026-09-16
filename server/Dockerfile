# Railway detecta este Dockerfile (Root Directory = server). Bun + Piper (voz
# local, sin tokens) con una voz por idioma. Las voces pesan ~60 MB cada una.
FROM oven/bun:1-debian@sha256:4f6e31d1a54d6a3dd312daef655fc998101b5043d52e12592ac293ef04b9bc73

RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates && rm -rf /var/lib/apt/lists/*

# Piper 2023.11.14-2 (binario x86_64 con espeak-ng y onnxruntime incluidos)
RUN mkdir -p /opt/piper/voices && cd /opt \
    && curl -sSL -o piper.tar.gz https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz \
    && echo "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992  piper.tar.gz" | sha256sum -c - \
    && tar xzf piper.tar.gz && rm piper.tar.gz

# Apps Lua de fábrica. Se fijan al commit de la release y se verifican antes de
# incorporarlas al paquete que el lector descarga a /Apps.
RUN mkdir -p /opt/apps && cd /opt/apps \
    && curl -fsSL --retry 3 -o reloj.lua https://raw.githubusercontent.com/gsozzi87/crosspoint-reader/09d15206f9c7bbb0d991c26bdf466cfbf1740a72/examples/Apps/reloj.lua \
    && curl -fsSL --retry 3 -o ahorcado.lua https://raw.githubusercontent.com/gsozzi87/crosspoint-reader/09d15206f9c7bbb0d991c26bdf466cfbf1740a72/examples/Apps/ahorcado.lua \
    && curl -fsSL --retry 3 -o tresenraya.lua https://raw.githubusercontent.com/gsozzi87/crosspoint-reader/09d15206f9c7bbb0d991c26bdf466cfbf1740a72/examples/Apps/tresenraya.lua \
    && printf '%s  %s\n' \
      df3764a6128dc7890d8b4a469386b25aea4e81d999d81c20696e480859c6ae73 reloj.lua \
      1d7d38d61467bd4ecc3a92a04ffeeaae888deb78ec5623556859ba69cd6417f0 ahorcado.lua \
      c8b29ea6d6755ccbfcb94ad264606f1348871b5faeed8fa8aeaa7763c72662de tresenraya.lua \
      | sha256sum -c -

# Apps agregadas en esta release. Van dentro del contexto de Docker para que el
# despliegue no dependa de que la rama de GitHub ya se haya actualizado.
COPY factory-apps/viajes.lua factory-apps/investigar_epub.lua /opt/apps/

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
