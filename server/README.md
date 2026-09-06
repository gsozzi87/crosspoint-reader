# Servidor del hub ws397 (Bun + Hono, Railway)

Todo lo que el aparato necesita del lado del servidor, en un solo lugar: OTA del firmware, preguntas al
libro, transcripción, hub (clima, agenda, recordatorios, mensajes), voz con clasificador de intención y
el store de recordatorios, listas, notas y mensajes.

## Railway

- **Root Directory**: `server` (Settings → Source → Root Directory). Con eso Railway solo mira esta carpeta.
- **Start command**: `bun run src/index.ts` (o `bun start`). Detecta Bun por el `package.json`.
- **Volumen** montado en `/data` (firmware subido, `store.json`, `hub-settings.json`, `hub-data.json`).
- Variables:

| Variable | Para qué |
|---|---|
| `OTA_TOKEN` | Bearer que usa `release.sh` / `release.ps1` para subir el `.bin` (`PUT /firmware`). |
| `DEVICE_TOKEN` | Bearer del aparato para todo `/api/*` (web UI del aparato → Servidor → token). |
| `ANTHROPIC_API_KEY` | Claude (preguntas y clasificador de voz). Nunca va al aparato. |
| `STT_API_KEY` (o `OPENAI_API_KEY`) | Transcripción por un endpoint compatible con OpenAI. |
| `STT_BASE_URL`, `STT_MODEL` | Opcionales. Groq: `https://api.groq.com/openai/v1` + `whisper-large-v3-turbo`. |
| `ASK_MODEL`, `VOICE_MODEL` | Opcionales, default `claude-haiku-4-5`. |
| `HUB_LAT`, `HUB_LON`, `HUB_TZ` | Respaldo del clima mientras no se elija lugar por voz desde el aparato. |

## Rutas

| Ruta | Quién | Qué |
|---|---|---|
| `GET /firmware/latest` | aparato (sin token) | JSON con forma de release de GitHub: `tag_name`, `assets[firmware-ws397.bin]`. |
| `GET /firmware/firmware-ws397.bin` | aparato | El binario. |
| `PUT /firmware` | `release.sh` (Bearer `OTA_TOKEN`, `X-Version`) | Sube un binario nuevo. |
| `GET /api/ping` | aparato | Prueba del token. |
| `POST /api/ask` | aparato | Pregunta sobre el libro (`text`) o general (sin `text`). `lang` = idioma de la UI. |
| `POST /api/transcribe?lang=xx` | aparato | WAV → texto en el idioma de la UI. |
| `GET /api/hub?lang=xx` | aparato | Clima, recordatorios, listas, agenda, mensajes y frase, en el idioma de la UI. |
| `GET /api/hub/location/search?q=`, `POST /api/hub/location` | aparato | Lugar del clima por voz. |
| `POST /api/voice?lang=xx` | aparato | Una grabación: transcribe, clasifica la intención y ejecuta. |
| `POST /api/hub/done` | aparato | `{kind: "reminder"\|"item", id}` marca hecho (también desde la cola offline). |

Idiomas soportados (`lang`): `es`, `en`, `fr`, `de`, `pt`, `ru`. El aparato manda el que tiene en Settings;
la transcripción escucha en ese idioma, las respuestas salen en ese idioma y el traductor traduce desde ese
idioma al que se pida (si no se dice, al inglés; desde inglés, al español).

## Local

```
cd server && bun install && OTA_TOKEN=x DEVICE_TOKEN=y ANTHROPIC_API_KEY=... STT_API_KEY=... bun run src/index.ts
```
