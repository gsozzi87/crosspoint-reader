# Railway detecta este Dockerfile (Root Directory = server). Bun + Piper (voz
# local, sin tokens) con una voz por idioma. Las voces pesan ~60 MB cada una.
FROM oven/bun:1-debian

RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates && rm -rf /var/lib/apt/lists/*

# Piper 2023.11.14-2 (binario x86_64 con espeak-ng y onnxruntime incluidos)
RUN mkdir -p /opt/piper/voices && cd /opt \
    && curl -sSL -o piper.tar.gz https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz \
    && tar xzf piper.tar.gz && rm piper.tar.gz

# Voces (rhasspy/piper-voices en Hugging Face): es, en, fr, de, pt, ru
ARG HF=https://huggingface.co/rhasspy/piper-voices/resolve/main
RUN cd /opt/piper/voices && for v in \
      es/es_MX/ald/medium/es_MX-ald-medium \
      en/en_US/lessac/medium/en_US-lessac-medium \
      fr/fr_FR/siwis/medium/fr_FR-siwis-medium \
      de/de_DE/thorsten/medium/de_DE-thorsten-medium \
      pt/pt_BR/faber/medium/pt_BR-faber-medium \
      ru/ru_RU/irina/medium/ru_RU-irina-medium ; do \
      n=$(basename $v); curl -sSL -o $n.onnx $HF/$v.onnx && curl -sSL -o $n.onnx.json $HF/$v.onnx.json ; done

# Biblias (thiagobodruk/bible, JSON): es, en, fr, de, pt, ru (~27 MB)
RUN mkdir -p /opt/bible && cd /opt/bible && for f in es_rvr en_kjv fr_apee de_schlachter pt_aa ru_synodal; do \
      curl -sSL -o $f.json https://raw.githubusercontent.com/thiagobodruk/bible/master/json/$f.json ; done

WORKDIR /app
COPY package.json bun.lock* ./
RUN bun install --production
COPY src ./src

ENV PIPER_BIN=/opt/piper/piper PIPER_VOICES=/opt/piper/voices BIBLE_DIR=/opt/bible PORT=3000
EXPOSE 3000
CMD ["bun", "run", "src/index.ts"]
