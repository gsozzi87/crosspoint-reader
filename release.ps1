# Compila CrossPoint para la ws397 con un número de build nuevo y lo sube al Hono.
# Uso (desde la carpeta crosspoint-reader):
#   .\release.ps1                  → build + upload (el aparato lo baja con "Check for updates")
#   .\release.ps1 -Usb COM5        → build + flash por USB (sin subir)
#   .\release.ps1 -Upload -Usb COM5 → las dos cosas
#
# Configurar una vez (PowerShell, persistente para tu usuario):
#   [Environment]::SetEnvironmentVariable("WS397_OTA_URL",  "https://TU-APP.up.railway.app/firmware/latest", "User")
#   [Environment]::SetEnvironmentVariable("WS397_OTA_TOKEN", "un-token-largo", "User")
param([switch]$Upload = $true, [string]$Usb = "")
$ErrorActionPreference = "Stop"

if (-not $env:WS397_OTA_URL)   { throw "Falta WS397_OTA_URL (ver comentario arriba)" }
$buildFile = ".ws397-build"
$build = if (Test-Path $buildFile) { [int](Get-Content $buildFile) + 1 } else { 1 }
$version = "1.5.$build"
$env:WS397_BUILD = "$build"

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
    $putUrl = $env:WS397_OTA_URL -replace "/latest$", ""
    Write-Host "Subiendo $bin a $putUrl..." -ForegroundColor Cyan
    Invoke-RestMethod -Method Put -Uri $putUrl -InFile $bin -ContentType "application/octet-stream" `
        -Headers @{ Authorization = "Bearer $($env:WS397_OTA_TOKEN)"; "X-Version" = $version }
    Write-Host "Listo: en el aparato, Settings -> Check for updates instala $version" -ForegroundColor Green
}
