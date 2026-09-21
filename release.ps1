# Compila CrossPoint para la ws397 con un número de build nuevo y lo sube al Hono.
# Uso (desde la carpeta crosspoint-reader):
#   .\release.ps1                  → build + upload (el aparato lo baja con "Check for updates")
#   .\release.ps1 -Usb COM5        → build + flash por USB (sin subir)
#   .\release.ps1 -Upload -Usb COM5 → las dos cosas
#
# Configurar una vez (PowerShell, persistente para tu usuario):
#   [Environment]::SetEnvironmentVariable("WS397_OTA_TOKEN", "un-token-largo", "User")
param([switch]$Upload = $true, [string]$Usb = "")
$ErrorActionPreference = "Stop"

# EL PORTON DE RELEASE (REV-083). La misma regla que release.sh y en el mismo
# archivo, no una segunda copia: arbol limpio + la CI de ESTE SHA en verde,
# ANTES del bump. Hasta aca este camino no consultaba nada, y es justo el que se
# usa en Windows. Solo se exige cuando se va a PUBLICAR: flashear por cable es
# depurar, y ahi el arbol sucio es lo normal.
if ($Upload) {
    python tools/release_gate.py
    if ($LASTEXITCODE -ne 0) { throw "El porton de release dijo que no" }
} else {
    Write-Host "Solo USB: el porton de release no se aplica (no se publica nada)." -ForegroundColor Yellow
}

$otaUrl = if ($env:WS397_OTA_URL) { $env:WS397_OTA_URL } else { "https://paper-esp32.up.railway.app/firmware/latest" }
$buildFile = ".ws397-build"
$build = if (Test-Path $buildFile) { [int](Get-Content $buildFile) + 1 } else { 1 }
$version = "1.5.$build"
# El número de build vive en include/ws397_version.h (no en un -D flag): así solo
# recompilan los 8 archivos que lo incluyen y no el árbol entero.
$hdr = "include\ws397_version.h"
(Get-Content $hdr -Raw) -replace '#define WS397_BUILD \d+', "#define WS397_BUILD $build" | Set-Content $hdr -NoNewline

Write-Host "Compilando $version-ws397..." -ForegroundColor Cyan
python -m platformio run -e ws397
if ($LASTEXITCODE -ne 0) { throw "Falló la compilación" }
Set-Content $buildFile $build

$bin = ".pio\build\ws397\firmware.bin"
if ($Usb) {
    Write-Host "Flasheando por $Usb..." -ForegroundColor Cyan
    python -m platformio run -e ws397 -t upload --upload-port $Usb
}
if ($Upload) {
    if (-not $env:WS397_OTA_TOKEN) { throw "Falta WS397_OTA_TOKEN" }
    $putUrl = $otaUrl -replace "/latest$", ""
    Write-Host "Subiendo $bin a $putUrl..." -ForegroundColor Cyan
    Invoke-RestMethod -Method Put -Uri $putUrl -InFile $bin -ContentType "application/octet-stream" `
        -Headers @{ Authorization = "Bearer $($env:WS397_OTA_TOKEN)"; "X-Version" = $version }

    # El PUT contesto 200, pero eso no dice que quedo SERVIDO. Sin esto el repo
    # podia quedar diciendo N mientras /firmware/latest seguia entregando N-1, y
    # el unico sintoma era que el aparato no veia la actualizacion (la
    # comparacion es major.minor.patch estricta). release.sh ya lo comprobaba;
    # este camino no, y es el mismo agujero por el otro lado.
    Write-Host "Verificando lo que quedo servido en $otaUrl..." -ForegroundColor Cyan
    $servido = (Invoke-RestMethod -Method Get -Uri $otaUrl -TimeoutSec 30).tag_name
    if ($servido -ne $version) {
        throw "Se subio $version pero /firmware/latest entrega '$servido'. El release NO esta publicado."
    }
    Write-Host "OK: /firmware/latest entrega $servido" -ForegroundColor Green
    Write-Host "Listo: en el aparato, Settings -> Check for updates instala $version" -ForegroundColor Green
}
